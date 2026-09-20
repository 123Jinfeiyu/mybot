#!/bin/bash
# agent 模式端到端：本地假模型服务 + 真实 mybot 工具循环（无硬件 relay-test）。
#
# 六个场景：
#   1) 默认协议驱动（LlmOpenAI，OpenAI 兼容）——工具循环迭代 + 动作脚本在**常驻**
#      机器人上真被执行 + 最终回答只走 stdout；并抓取线上报文断言这是 OpenAI 形态
#      （有 temperature、无 thinking）。
#   2) --llm-driver LlmDeepSeek——同一条循环跑通，并断言 DeepSeek 的协议差异：
#      请求带 thinking、不带 temperature、且**历史 reasoning_content 被回传**
#      （这条有状态要求在官方文档里是 400 级错误，必须由守门钉住）。
#   3) --llm-timeout 真的生效——用睡 5 秒的服务端配 1 秒超时，必须在 4 秒内失败。
#      这个用例专门防 curl_easy_setopt 退回**裸变参调用**：那时整型选项被静默忽略，
#      命令会跑满 5 秒且可能 rc=0（实测过），是这条链路最隐蔽的回归。
#   4) --llm-config 配置文件——断言 URL / KEY / MODEL 全部来自文件，且进程环境里
#      不放任何密钥。
#   5) capture_image——拍照并把画面注入上下文，断言线上报文确实带 image_url
#      多模态内容（以 data URL 包装的真 JPEG）。
#   6) --prompt-file 提示词包——目录 .md 按文件名自然排序、以 "## 文件名" 分节，
#      整包追加在系统提示词内置段落之后（线上报文逐项断言）。
#
# 场景 1-3、5 显式 --llm-config none：否则 CWD 里恰好存在的 ./mybot.llm.json 会被
# 自动发现，让「默认驱动」变成别的驱动 —— 守门必须与开发者个人配置无关。
#
# 用法: BOT=./build/mybot bash scripts/agent_e2e.sh
# 退出码: 0 通过 / 1 失败 / 125 缺 python3（守门按 SKIP 计）
set -u

BOT="${BOT:-./build/mybot}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 1

if ! command -v python3 >/dev/null 2>&1; then
	echo "缺少 python3（假模型服务），无法运行 agent 端到端"
	exit 125
fi

tmp="$(mktemp -d)"
SRV=""
cleanup() {
	[ -n "$SRV" ] && kill "$SRV" 2>/dev/null
	[ -n "$SRV" ] && wait "$SRV" 2>/dev/null
	rm -rf "$tmp"
}
trap cleanup EXIT

fail=0
check() { # $1=输出 $2=子串 $3=说明
	if echo "$1" | grep -qF "$2"; then
		echo "  [OK] $3"
	else
		echo "  [FAIL] $3（未找到: $2）"
		fail=1
	fi
}
check_present() { # $1=文件 $2=子串 $3=说明
	if grep -qF "$2" "$1" 2>/dev/null; then
		echo "  [OK] $3"
	else
		echo "  [FAIL] $3（未找到: $2）"
		fail=1
	fi
}
check_absent() { # $1=文件 $2=子串 $3=说明
	if grep -qF "$2" "$1" 2>/dev/null; then
		echo "  [FAIL] $3（不应出现: $2）"
		fail=1
	else
		echo "  [OK] $3"
	fi
}
check_json() { # $1=JSON文件 $2=python 真值表达式 $3=说明
	if python3 -c "
import json, sys
d = json.load(open('$1'))
sys.exit(0 if ($2) else 1)
" 2>/dev/null; then
		echo "  [OK] $3"
	else
		echo "  [FAIL] $3"
		fail=1
	fi
}

start_server() { # $1=port $2=capture $3=mode $4=headers
	python3 scripts/fake_llm_server.py "$1" "$2" "${3:-ok}" "${4:-}" &
	SRV=$!
	for _ in $(seq 1 50); do
		if python3 - "$1" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.2)
s.connect(("127.0.0.1", int(sys.argv[1])))
s.close()
PY
		then
			return 0
		fi
		sleep 0.1
	done
	echo "假模型服务未就绪"
	return 1
}
stop_server() {
	[ -n "$SRV" ] && kill "$SRV" 2>/dev/null
	[ -n "$SRV" ] && wait "$SRV" 2>/dev/null
	SRV=""
}

TASK="先看硬件，再让继电器动作一次，最后跑一条命令"

# ============ 场景 1：当前协议（OpenAI 兼容） ============
echo "----- 场景 1：LlmOpenAI（当前协议） -----"
PORT=$((20000 + RANDOM % 300))
CAP1="$tmp/req-openai.json"
start_server "$PORT" "$CAP1" ok || exit 1
# 同时覆盖「密钥经环境变量 → 驱动读取」这条路径（--llm-key 已于 2026-09-20 评估移除）；
# --agent-shell：run_command 缺省不提供，本场景要断言工具循环真的跑了 shell 命令
out1="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent --task "$TASK" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--agent-shell --llm-config none --scripts-dir scripts 2>&1)"
rc1=$?
stop_server
echo "$out1"
# libcurl 是 agent 模式的唯一运行期依赖（Lua 驱动经 ffi 加载）。缺它是环境条件，
# 按 SKIP 计并在守门汇总的「未执行清单」里可见，而不是伪装成功能失败。
if echo "$out1" | grep -qF "libcurl 不可用"; then
	echo "[SKIP] 运行环境没有 libcurl（agent 模式的运行期依赖）"
	exit 125
fi
if [ "$rc1" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc1"
	fail=1
fi
check "$out1" "AGENT-E2E-完成" "最终回答到达 stdout"
check "$out1" "relay0" "list_hardware 列出了常驻机器人的硬件变量"
check "$out1" "Relay: test=200" "run_action_script 真的驱动了动作脚本"
check "$out1" "agent-echo-ok" "run_command 执行了 shell 命令"
check "$out1" "已执行动作脚本 relay-test.mt" "动作脚本执行结果回传给了模型"
check_present "$CAP1" '"temperature"' "默认驱动发的是 OpenAI 形态报文（带 temperature）"
check_absent "$CAP1" '"thinking"' "默认驱动不引入 DeepSeek 的 thinking 字段"
# 模型必须真的收到工具定义：曾经 C++ 侧把工具提前包成 provider 形状，Lua 驱动
# 又包一层 → function 变空、模型零工具可用，而工具循环表面上仍在跑（假服务不读
# tools 故未暴露）。这条按结构断言，不看字符串顺序。
# 用「必须包含」而非精确计数：新增工具（如 capture_image）不该弄红这条断言
check_json "$CAP1" "all(t.get('type')=='function' and t.get('function',{}).get('name') and t['function'].get('parameters') for t in d.get('tools',[])) and {'list_action_scripts','run_action_script','list_hardware','run_command'} <= {t['function']['name'] for t in d['tools']}" \
	"模型收到全部工具定义（名字与 parameters 齐备）"

# ============ 场景 2：DeepSeek 协议 ============
echo "----- 场景 2：LlmDeepSeek（DeepSeek 协议） -----"
PORT=$((20400 + RANDOM % 300))
CAP2="$tmp/req-deepseek.json"
start_server "$PORT" "$CAP2" ok || exit 1
# 走环境变量（由驱动自己读）；--agent-shell 同场景 1（本场景也断言 run_command 在工具表里）
out2="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql \
	--agent --llm-driver LlmDeepSeek --llm-config none --task "$TASK" --agent-shell \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--scripts-dir scripts 2>&1)"
rc2=$?
stop_server
echo "$out2"
if [ "$rc2" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc2"
	fail=1
fi
check "$out2" "AGENT-E2E-完成" "DeepSeek 驱动跑通完整工具循环"
check "$out2" "Relay: test=200" "DeepSeek 场景下动作脚本同样被执行"
check_present "$CAP2" '"thinking"' "请求带 thinking（DeepSeek 思考模式开关）"
check_absent "$CAP2" '"temperature"' "思考模式不发 temperature（该模式不支持）"
check_present "$CAP2" '"reasoning_content"' "历史 reasoning_content 被回传（否则 API 400）"
check_json "$CAP2" "all(t.get('function',{}).get('name') for t in d.get('tools',[])) and {'list_action_scripts','run_action_script','list_hardware','run_command'} <= {t['function']['name'] for t in d['tools']}" \
	"DeepSeek 请求同样带齐全部工具定义"

# ============ 场景 3：超时真的生效（防变参 cast 退化） ============
echo "----- 场景 3：--llm-timeout 生效 -----"
PORT=$((20800 + RANDOM % 300))
start_server "$PORT" "" slow || exit 1
t0=$(date +%s)
out3="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 30 "$BOT" --robot=robots/relay-test.sql \
	--agent --llm-config none --task "随便问一句" --llm-timeout 1 \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" 2>&1)"
rc3=$?
t1=$(date +%s)
stop_server
echo "$out3"
elapsed=$((t1 - t0))
if [ "$rc3" -ne 0 ] && [ "$elapsed" -lt 4 ] && echo "$out3" | grep -qiE "timeout|超时"; then
	echo "  [OK] 1 秒超时在 ${elapsed}s 内生效（curl 整型选项未被裸变参吞掉）"
else
	echo "  [FAIL] 超时未生效（rc=$rc3, 用时 ${elapsed}s）——"
	echo "         很可能 curl_easy_setopt 退回了裸变参调用，整型选项被静默忽略"
	fail=1
fi

# ============ 场景 4：URL / KEY / MODEL 全部来自配置文件 ============
echo "----- 场景 4：--llm-config 配置文件 -----"
PORT=$((21200 + RANDOM % 300))
CAP4="$tmp/req-cfg.json"
HDR4="$tmp/hdr-cfg.txt"
CFG4="$tmp/mybot.llm.json"
cat > "$CFG4" <<JSON
{
  "driver": "LlmOpenAI",
  "endpoint": "http://127.0.0.1:$PORT/v1/chat/completions",
  "model": "cfg-model-from-file",
  "api_key": "cfg-key-from-file",
  "timeout": 30,
  "max_tokens": 1234,
  "temperature": 0.25,
  "max_turns": 6
}
JSON
chmod 600 "$CFG4"	# 密钥文件应收紧权限（否则驱动会打印告警）
start_server "$PORT" "$CAP4" ok "$HDR4" || exit 1
# 只给 --llm-config：环境里**不放**任何密钥，证明 URL/KEY/MODEL 确实来自文件
out4="$(env -u BOT4CJ_LLM_KEY -u ROBOT_AGENT_API_KEY -u DEEPSEEK_API_KEY \
	timeout 60 "$BOT" --robot=robots/relay-test.sql --agent --task "$TASK" \
	--llm-config "$CFG4" --scripts-dir scripts 2>&1)"
rc4=$?
stop_server
echo "$out4"
if [ "$rc4" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc4"
	fail=1
fi
check "$out4" "AGENT-E2E-完成" "仅凭配置文件跑通完整工具循环"
check "$out4" "配置文件: $CFG4" "会话开始如实告知用了哪个配置文件"
check_present "$CAP4" '"model":"cfg-model-from-file"' "模型名来自配置文件"
check_present "$CAP4" '"max_tokens":1234' "max_tokens 来自配置文件"
check_present "$CAP4" '"temperature":0.25' "temperature 来自配置文件"
check_present "$HDR4" "cfg-key-from-file" "API key 来自配置文件（Authorization 头）"
# 安全缺省（2026-09-20 评估 C12）：配置文件没写 allow_shell、命令行也没 --agent-shell
# 时，run_command 不得出现在工具表里——这条断言正是「缺省关闭」的判别性守门。
check_json "$CAP4" "'run_command' not in {t.get('function',{}).get('name') for t in d.get('tools',[])}" \
	"缺省不提供 run_command（无 --agent-shell / allow_shell 时工具表里没有它）"

# ============ 场景 5：拍照并进入上下文（多模态） ============
echo "----- 场景 5：capture_image 图片进入上下文 -----"
PORT=$((21600 + RANDOM % 300))
CAP5="$tmp/req-camera.json"
PIC5="$tmp/pic.jpg"
python3 - "$PIC5" <<'PICEOF'
import base64, sys
# 1x1 真 JPEG（160 字节，FFD8 开头 / FFD9 结尾）——驱动只搬运字节，用真图便于断言
open(sys.argv[1], "wb").write(base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a"
    "HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA"
    "AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q=="))
PICEOF
start_server "$PORT" "$CAP5" camera || exit 1
out5="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql \
	--agent --llm-config none --camera-src "$PIC5" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "拍张照片看看周围有什么" 2>&1)"
rc5=$?
stop_server
echo "$out5"
if [ "$rc5" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc5"
	fail=1
fi
check "$out5" "AGENT-E2E-完成" "拍照之后仍能给出最终回答"
check "$out5" "capture_image" "工具循环调用了 capture_image"
check "$out5" "已拍摄照片" "工具返回了照片说明（含模拟来源标注）"
check_json "$CAP5" "any(isinstance(m.get('content'), list) and any(p.get('type')=='image_url' for p in m['content']) for m in d['messages'])" \
	"线上报文确实带上了 image_url（多模态内容）"
check_present "$CAP5" '"data:image/jpeg;base64,/9j/' "注入的是以 data URL 包装的真 JPEG"

# ============ 场景 6：--prompt-file 提示词包进入系统提示词 ============
echo "----- 场景 6：--prompt-file 提示词包 -----"
PORT=$((21900 + RANDOM % 300))
CAP6="$tmp/req-promptpack.json"
PACK6="$tmp/promptpack"
mkdir -p "$PACK6"
# 故意乱序创建：线上断言依赖「文件名自然排序」而非创建顺序
printf '你是蜘蛛机器人。\n' > "$PACK6/10-尾声.md"
printf '前方有障碍不要前进。\n' > "$PACK6/2-纪律.md"
printf '蜘蛛机器人身份。\n' > "$PACK6/1-身份.md"
start_server "$PORT" "$CAP6" ok || exit 1
out6="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql \
	--agent --llm-config none --scripts-dir scripts \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--prompt-file "$PACK6" --task "$TASK" 2>&1)"
rc6=$?
stop_server
echo "$out6"
if [ "$rc6" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc6"
	fail=1
fi
check "$out6" "AGENT-E2E-完成" "带提示词包仍跑通完整工具循环"
check_present "$CAP6" '蜘蛛机器人身份。' "提示词包内容进入线上报文"
check_json "$CAP6" "d['messages'][0]['role']=='system' and '蜘蛛机器人身份。' in d['messages'][0]['content']" \
	"提示词包在 system 消息里"
check_json "$CAP6" "'## 工作方式' in d['messages'][0]['content'] and d['messages'][0]['content'].find('## 工作方式') < d['messages'][0]['content'].find('蜘蛛机器人身份。')" \
	"提示词包追加在内置段落之后"
check_json "$CAP6" "'## 1-身份.md' in d['messages'][0]['content'] and d['messages'][0]['content'].find('## 1-身份.md') < d['messages'][0]['content'].find('## 2-纪律.md') < d['messages'][0]['content'].find('## 10-尾声.md')" \
	"目录按文件名自然排序分节（2- 在 10- 之前）"

if [ "$fail" -eq 0 ]; then
	echo "agent 模式端到端测试通过"
	exit 0
fi
exit 1
