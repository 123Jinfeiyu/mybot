#!/bin/bash
# agent 模式端到端：本地假模型服务 + 真实 mybot 工具循环（无硬件 relay-test）。
#
# 三个场景：
#   1) 默认协议驱动（LlmOpenAI，OpenAI 兼容）——工具循环迭代 + 动作脚本在**常驻**
#      机器人上真被执行 + 最终回答只走 stdout；并抓取线上报文断言这是 OpenAI 形态
#      （有 temperature、无 thinking）。
#   2) --llm-driver LlmDeepSeek——同一条循环跑通，并断言 DeepSeek 的协议差异：
#      请求带 thinking、不带 temperature、且**历史 reasoning_content 被回传**
#      （这条有状态要求在官方文档里是 400 级错误，必须由守门钉住）。
#   3) --llm-timeout 真的生效——用睡 5 秒的服务端配 1 秒超时，必须在 4 秒内失败。
#      这个用例专门防 curl_easy_setopt 退回**裸变参调用**：那时整型选项被静默忽略，
#      命令会跑满 5 秒且可能 rc=0（实测过），是这条链路最隐蔽的回归。
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
# 同时覆盖 --llm-key → 进程内环境变量这条路径
out1="$(timeout 60 "$BOT" --robot=robots/relay-test.sql --agent --task "$TASK" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--llm-key "e2e-fake-key" --scripts-dir scripts 2>&1)"
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
check_json "$CAP1" "len(d.get('tools',[]))==4 and all(t.get('type')=='function' and t.get('function',{}).get('name') and t['function'].get('parameters') for t in d['tools'])" \
	"模型收到 4 个带名字与 parameters 的工具定义"

# ============ 场景 2：DeepSeek 协议 ============
echo "----- 场景 2：LlmDeepSeek（DeepSeek 协议） -----"
PORT=$((20400 + RANDOM % 300))
CAP2="$tmp/req-deepseek.json"
start_server "$PORT" "$CAP2" ok || exit 1
# 走环境变量（由驱动自己读），不传 --llm-key
out2="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql \
	--agent --llm-driver LlmDeepSeek --task "$TASK" \
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
check_json "$CAP2" "len(d.get('tools',[]))==4 and all(t.get('function',{}).get('name') for t in d['tools'])" \
	"DeepSeek 请求同样带齐 4 个工具定义"

# ============ 场景 3：超时真的生效（防变参 cast 退化） ============
echo "----- 场景 3：--llm-timeout 生效 -----"
PORT=$((20800 + RANDOM % 300))
start_server "$PORT" "" slow || exit 1
t0=$(date +%s)
out3="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 30 "$BOT" --robot=robots/relay-test.sql \
	--agent --task "随便问一句" --llm-timeout 1 \
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

if [ "$fail" -eq 0 ]; then
	echo "agent 模式端到端测试通过"
	exit 0
fi
exit 1
