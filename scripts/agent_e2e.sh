#!/bin/bash
# agent 模式端到端：本地假模型服务 + 真实 mybot 工具循环（无硬件 relay-test）。
#
# 二十二个场景：
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
#   5b) H1 的反面：**配置文件**给的 camera_src 不得自动放行——三条白名单根来源
#      （--camera-src-dir / 命令行 --camera-src 的推导 / BOT4CJ_CAMERA_SRC_DIR）
#      全空时必须走「未放行」，且线上报文里不含 image_url。覆盖的是接线。
#   6) --prompt-file 提示词包——目录 .md 按文件名自然排序、以 "## 文件名" 分节，
#      整包追加在系统提示词内置段落之后（线上报文逐项断言）。
#   7) --audit-log 审计轨迹——每次工具调用各留一条 JSON Lines（ts/session/tool/ok
#      齐备、run_command 带 exit_code），环境变量密钥不出现在日志里。
#   8) 模型调用可取消——SIGINT 在 <5s 内经取消哨兵中止在飞的 libcurl 请求，
#      报错点名「已取消」而非超时，且哨兵文件不残留。
#   9) --agent-policy 策略钩子——确认程序拒绝时 run_command 零副作用（命令没跑、
#      工具回执点名被拦截、循环照常收尾）；确认程序放行时命令照常执行。
#  10) 单轮调用上限截断（SAN-1）——一轮返回 3 条调用、上限 2：全程无 400、
#      截断事件对操作者可见、**第二个任务照常完成**（会话没被永久打成 400）。
#  11) 限流/5xx 的重试（SAN-3）——429 前两次、第三次成功：任务完成 + 重试日志
#      点名 429 + 退避采用服务端 Retry-After；恒 503 则重试用尽后如实失败。
#  12) 宿主的动作回显不得污染 stdout（SAN-5）——stdout 只有模型回答，回显走 stderr。
#  13) 取消走的是「优雅退出」而不是被信号打死（SAN-7）——中断后 deactivate 链仍执行。
#  14) --agent-hot-sql（2026-09-23 热插表）——模型经 apply_sql 给**带电常驻**的机器人
#      追加硬件行：INSERT 后立即装配并上电，下一轮 list_hardware 的回执里必须出现
#      新变量。14b 是对照：缺该开关时工具不在表内（回执点名「未知工具」、零变更）。
#      14c（2026-09-23 热删除）：同一门控下的 DELETE 真的把变量摘掉，且回执点名删了谁。
#  15) --read-var（2026-09-23 回读通道 A）——模型经 read_variables 主动读硬件变量，
#      回执里必须出现驱动返回表的键（relay0 经 controller 转发到 timer-0 的 slept）。
#      15b 是对照：缺该开关时工具不在表内，**且线上工具定义里也没有它**。
#  19) --session-file / --resume（2026-09-23 批次 1）——**两个进程接力**：第一个进程
#      落盘会话文件；第二个进程带 --resume 启动，其**首个请求**里必须带第一个进程的
#      对话（这是"重启不忘记"的唯一判据）。同场对照：指纹不符（换结构脚本）必须拒绝
#      恢复并退出 2；缺 --session-file 的 --resume 必须退出 2（恢复只认命令行路径）。
#  20) --agent --tcp（2026-09-23 批次 2）——远程 REPL：python 客户端跑通
#      「token → 任务 → 工具调用 → 回显」全链路；对照三条：错 token 被拒、
#      空闲超时按输入结束收尾（不是挂死）、**发出任务后断开连接 = 远程取消**
#      （在飞的模型调用被立刻中止，而不是等满 --llm-timeout）。
#  21) LlmAnthropic（2026-09-23 批次 3）——第三个协议驱动：同一二进制换驱动即
#      换协议（system 顶层 / tool_use 与 tool_result 块 / input_schema /
#      x-api-key 头），工具循环与动作执行照常；宿主零改是前提。
#
# 场景 1-3、5 显式 --llm-config none：否则 CWD 里恰好存在的 ./mybot.llm.json 会被
# 自动发现，让「默认驱动」变成别的驱动 —— 守门必须与开发者个人配置无关。
#
# 用法: BOT=./build/mybot bash scripts/agent_e2e.sh [--rust]
#   本文件**只有 Rust 一种模式**：跑 R1-R10（mybot-agent；R1-R5 批次 B 骨架、
#   R6-R9 批次 C 循环层、R10 审计 H2 修复），拓扑是「后端 + mybot-agent」两个进程；
#   缺 agent/target/release/mybot-agent 时直接报错退出（守门那边按 SKIP 计，
#   见 test/lib/sections/net_agent.sh）。`--rust` 保留为兼容空参（既有调用与
#   scripts/build_agent.sh 都还带着它）。
#
#   原先此处的 22 个 C++ 场景随 d384a9b9 移除 C++ Agent REPL 一并消失，按**归属**分流：
#   属后端工具面的 6 条（apply_sql 带电生效/热删除及其门控对照、read_variables 回读
#   及其对照、策略拒绝零副作用）迁到 scripts/mcp_e2e.sh，由纯 Python 帧客户端直连后端
#   跑——不经模型，判据更硬；属循环层的（工具循环、超时、重试、取消、图片后注、
#   stdout 纯净/写失败不 panic）由下面 R 段守，R 段与 C++ 场景**同判据**。
#   仍未覆盖的（如实记下）：--session-file/--resume 两进程接力与 agent over TCP
#   ——Rust 侧尚无实现（批次 D），见 .tmp/bot4cj-eval-20260924.md 中风险第 5 条。
# 退出码: 0 通过 / 1 失败 / 2 参数用法错 / 125 缺 python3（守门按 SKIP 计）
set -u

BOT="${BOT:-./build/mybot}"
AGENT_BIN="${AGENT_BIN:-./agent/target/release/mybot-agent}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 1

# 只有 Rust 一种模式，故不再有模式分派；--rust 作为兼容空参收下。
for _arg in "$@"; do
	case "$_arg" in
		--rust) : ;;
		*)
			echo "未知参数: $_arg（用法: bash scripts/agent_e2e.sh [--rust]）"
			exit 2
			;;
	esac
done
unset _arg
if [ ! -x "$AGENT_BIN" ]; then
	echo "缺 $AGENT_BIN（先 bash scripts/build_agent.sh --release）"
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "缺少 python3（假模型服务），无法运行 agent 端到端"
	exit 125
fi

tmp="$(mktemp -d)"
SRV=""
BACKPID=""
cleanup() {
	[ -n "$SRV" ] && kill "$SRV" 2>/dev/null
	[ -n "$SRV" ] && wait "$SRV" 2>/dev/null
	[ -n "$BACKPID" ] && kill -TERM "$BACKPID" 2>/dev/null
	[ -n "$BACKPID" ] && wait "$BACKPID" 2>/dev/null
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
	# `--` 必须有：模式以 "-" 开头时（SAN-5 场景要抓的动作分隔线就是 "----------"）
	# grep 会把它当成选项串解析并**静默返回非 0**，本函数于是把"找到了"报成"没找到"。
	# 实测踩到：separator 明明在文件里（head 看得见、grep -c 得 1），check_present 报 FAIL。
	if grep -qF -- "$2" "$1" 2>/dev/null; then
		echo "  [OK] $3"
	else
		echo "  [FAIL] $3（未找到: $2）"
		fail=1
	fi
}
check_absent() { # $1=文件 $2=子串 $3=说明
	# 同样加 `--`：少了它，以 "-" 开头的模式会把 check_absent 变成**永远通过**
	# （grep 报错 → 非 0 → 判"没找到"），方向相反但同样是假绿。
	if grep -qF -- "$2" "$1" 2>/dev/null; then
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

# ==================== Rust agent（--rust，2026-09-24 rig 迁移批次 B）====================
# 拓扑与 C++ 场景不同：这里跑的是**两个进程**——
#   [mybot-agent（rig）] --帧/AF_UNIX--> [mybot --mcp-backend] --Lua 驱动--> relay 虚拟机器人
# 假模型服务与 C++ 场景共用（scripts/fake_llm_server.py 的 ok 剧本）。断言面刻意与
# C++ 场景**同判据**（工具定义齐备、回执真的从后端回来、审计字段齐备、stdout 纯净），
# 但有几处必须按 Rust 侧**已成立的语义**写，照抄 C++ 就是假绿：
#   * 工具审计行**不在** agent 的审计文件里，由后端自己的 --audit-log 落（同一次运行）；
#   * 会话持久化（--session-file/--resume）、TCP 远程 REPL 还没搬（批次 D），
#     Rust 段因此不含这两个场景（SIGINT 取消在 R8b 覆盖）。
#
# 批次 C（2026-09-24，循环层语义）在 Rust 段新增四个场景，判据与 C++ 侧**同源**：
#   * R6 单轮调用上限（C++ 场景 10）：超限调用不派发、模型收到「未派发」回执、
#     会话不被污染（第二个任务照常完成）。呈现位置与 C++ 不同（C++ 挂在 assistant
#     文本里，rig 侧挂在工具回执上）——已记档为如实偏差。
#   * R7 限流重试（C++ 场景 11）：429 前两次 + Retry-After: 1，第三次成功；
#     判据含**审计 attempts=3**（重试真的发生过，不是一次就放弃）与恒 503 如实失败。
#   * R8 单次调用超时（C++ 场景 3）：--llm-timeout 1 对睡 5 秒的服务端，必须在 4 秒内
#     失败；R8b 取消：SIGINT 之后不再发起新调用，本轮以「执行被中断」收场、退出码 0。
#   * R9 图片后注（C++ 场景 5）：工具回执进的是 tool 消息，图片只能走**紧随其后的
#     user 消息**（OpenAI 不允许工具结果带图片）——线上报文的次序与 C++ 同序。
# 起后端（`mybot --mcp-backend`）：$1=目录，其余为该后端要开的能力开关。
# socket 目录先按 0700 建好（后端自己会建，但脚本先建可免掉那条权限告警——
# 0700 的**创建语义**由 mcp_e2e.sh 专门守，这里不重复测）。
start_backend() {
	local sd="$1"; shift
	mkdir -p "$sd"; chmod 700 "$sd"
	"$BOT" --robot=robots/relay-test.sql --mcp-backend --mcp-socket "$sd/mcp.sock" \
		--scripts-dir scripts "$@" >"$sd/backend.log" 2>&1 &
	BACKPID=$!
	local _
	for _ in $(seq 1 80); do
		[ -S "$sd/mcp.sock" ] && return 0
		sleep 0.1
	done
	echo "  后端未就绪（$sd/backend.log 尾部）:"
	tail -5 "$sd/backend.log" | sed 's/^/    /'
	return 1
}
stop_backend() {
	[ -n "$BACKPID" ] && kill -TERM "$BACKPID" 2>/dev/null
	[ -n "$BACKPID" ] && wait "$BACKPID" 2>/dev/null
	BACKPID=""
}
# 危险能力（--agent-shell）在后端侧**强制**要 policy（M5），恒放行以便测工具链本身。
RUST_POLICY="$tmp/rust-policy.json"
printf '%s' '{"require_confirm":["run_command","apply_sql"],"confirm_command":"/bin/true","on_timeout":"deny"}' > "$RUST_POLICY"
chmod 600 "$RUST_POLICY"
# 审计断言（JSON Lines 逐行解析；与 C++ 场景 7 的 audit_check 同形，但文件作参数）
audit_expr() { # $1=文件 $2=python 真值表达式 $3=说明
	if AUDIT_FILE="$1" python3 -c "
import json, os, sys
lines = [json.loads(x) for x in open(os.environ['AUDIT_FILE']) if x.strip()]
sys.exit(0 if ($2) else 1)
" 2>/dev/null; then
		echo "  [OK] $3"
	else
		echo "  [FAIL] $3"
		fail=1
	fi
}

# ---- R1：工具循环（报文 + 回执 + 后端侧执行见证）----
echo "----- R1：mybot-agent 工具循环（后端 + agent 两个进程） -----"
PORT=$((25500 + RANDOM % 300))
CAPR1="$tmp/req-rust1.json"
R1="$tmp/rust1"
start_server "$PORT" "$CAPR1" ok || exit 1
start_backend "$R1" --agent-shell --agent-policy "$RUST_POLICY" || exit 1
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R1/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config none --task "$TASK" >"$R1/out.txt" 2>"$R1/err.txt"
rcr1=$?
stop_backend
stop_server
echo "===== stdout ====="; cat "$R1/out.txt"
echo "===== stderr ====="; cat "$R1/err.txt"
if [ "$rcr1" -ne 0 ]; then
	echo "  [FAIL] mybot-agent 退出码 $rcr1"
	fail=1
fi
check_present "$R1/out.txt" "AGENT-E2E-完成" "最终回答到达 stdout"
# stdout 纯净性（与 C++ SAN-5 同口径，只是换成了 agent 自己的就绪行/诊断）：
# 界面与模型输出走 stdout，日志与工具失败诊断走 stderr。
check_absent "$R1/out.txt" "[mybot-agent]" "agent 自己的日志不进 stdout（只走 stderr）"
check_present "$R1/err.txt" "[mybot-agent] 驱动 LlmOpenAI" "就绪行如实报出驱动/接入点（stderr）"
# 三条回执必须**真的从后端回来**：各钉一句只有后端能产出的文本，且要求它在一条
# role=tool 的消息里（系统提示词里也有硬件清单，不看角色就会把提示词当成回执）。
check_json "$CAPR1" "any(m.get('role')=='tool' and '已注册硬件变量' in str(m.get('content','')) for m in d['messages'])" \
	"list_hardware 的回执进了对话（含硬件变量数）"
check_json "$CAPR1" "any(m.get('role')=='tool' and '已执行动作脚本 relay-test.mt' in str(m.get('content','')) for m in d['messages'])" \
	"run_action_script 的回执进了对话"
check_json "$CAPR1" "any(m.get('role')=='tool' and 'agent-echo-ok' in str(m.get('content','')) for m in d['messages'])" \
	"run_command 的回执带着命令的 stdout（shell 真的跑了）"
check_present "$R1/backend.log" "Relay: test=200" "后端侧动作脚本真的驱动了继电器（Lua 驱动回显）"
check_present "$CAPR1" '"temperature"' "默认驱动发的是 OpenAI 形态报文（带 temperature）"
check_absent "$CAPR1" '"thinking"' "不引入 DeepSeek 的 thinking 字段"
# 模型必须真的收到工具定义（与 C++ 场景 1 同判据，按结构断言不看顺序）
check_json "$CAPR1" "all(t.get('type')=='function' and t.get('function',{}).get('name') and t['function'].get('parameters') for t in d.get('tools',[])) and {'list_action_scripts','run_action_script','list_hardware','run_command'} <= {t['function']['name'] for t in d['tools']}" \
	"模型收到全部工具定义（名字与 parameters 齐备）"

# ---- R2：--prompt-file 提示词包进入系统提示词 ----
echo "----- R2：--prompt-file 提示词包 -----"
PORT=$((25800 + RANDOM % 300))
CAPR2="$tmp/req-rust2.json"
R2="$tmp/rust2"
PACKR="$tmp/rust-pack"
mkdir -p "$PACKR"
# 故意乱序创建：线上断言依赖「文件名自然排序」而非创建顺序
printf '你是蜘蛛机器人。\n' > "$PACKR/10-尾声.md"
printf '前方有障碍不要前进。\n' > "$PACKR/2-纪律.md"
printf '蜘蛛机器人身份。\n' > "$PACKR/1-身份.md"
start_server "$PORT" "$CAPR2" ok || exit 1
start_backend "$R2" --agent-shell --agent-policy "$RUST_POLICY" || exit 1
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R2/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config none --prompt-file "$PACKR" --task "$TASK" >"$R2/out.txt" 2>&1
rcr2=$?
stop_backend
stop_server
cat "$R2/out.txt"
if [ "$rcr2" -ne 0 ]; then
	echo "  [FAIL] mybot-agent 退出码 $rcr2"
	fail=1
fi
check_present "$R2/out.txt" "AGENT-E2E-完成" "带提示词包仍跑通完整工具循环"
# rig 把 system 消息序列化成块数组（[{"type":"text","text":...}]），故整条消息
# 转成 JSON 文本再找子串——判据仍是「内容与先后顺序」，不依赖块的形状。
check_json "$CAPR2" "'蜘蛛机器人身份。' in json.dumps(d['messages'][0], ensure_ascii=False) and d['messages'][0]['role']=='system'" \
	"提示词包内容在 system 消息里"
check_json "$CAPR2" "(lambda s: '## 工作方式' in s and s.find('## 工作方式') < s.find('蜘蛛机器人身份。'))(json.dumps(d['messages'][0], ensure_ascii=False))" \
	"提示词包追加在内置段落之后"
check_json "$CAPR2" "(lambda s: s.find('## 1-身份.md') < s.find('## 2-纪律.md') < s.find('## 10-尾声.md'))(json.dumps(d['messages'][0], ensure_ascii=False))" \
	"目录按文件名自然排序分节（2- 在 10- 之前）"

# ---- R3：--audit-log 审计轨迹（agent 侧 model/session，后端侧 tool）----
echo "----- R3：--audit-log 审计轨迹 -----"
PORT=$((26100 + RANDOM % 300))
CAPR3="$tmp/req-rust3.json"
R3="$tmp/rust3"
AAUD="$R3/agent-audit.jsonl"
BAUD="$R3/backend-audit.jsonl"
start_server "$PORT" "$CAPR3" ok || exit 1
start_backend "$R3" --agent-shell --agent-policy "$RUST_POLICY" --audit-log "$BAUD" || exit 1
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R3/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config none --audit-log "$AAUD" --task "$TASK" >"$R3/out.txt" 2>&1
rcr3=$?
stop_backend
stop_server
tail -3 "$R3/out.txt"
if [ "$rcr3" -ne 0 ]; then
	echo "  [FAIL] mybot-agent 退出码 $rcr3"
	fail=1
fi
check_present "$R3/out.txt" "AGENT-E2E-完成" "带审计日志跑通完整工具循环"
audit_expr "$AAUD" "all(isinstance(l, dict) and l.get('ts') and l.get('session') and l.get('kind') for l in lines)" \
	"审计每行都是合法 JSON 且带 ts/session/kind（所有 kind 共有）"
audit_expr "$AAUD" "[l.get('status') for l in lines if l['kind']=='session'] == ['start','end']" \
	"会话头尾各一条 session 行（start/end）"
audit_expr "$AAUD" "any(l['kind']=='session' and l.get('status')=='start' and l.get('driver') and l.get('endpoint') and l.get('api_key_source')=='env' for l in lines)" \
	"start 行带驱动/接入点/密钥来源（可按会话复盘配置，且不记密钥值）"
audit_expr "$AAUD" "any(l['kind']=='model' and l.get('ok') is True and l.get('prompt_tokens')==1234 and l.get('completion_tokens')==56 and l.get('ms', 0) >= 0 for l in lines)" \
	"model 行按 provider 给的 usage 记账（prompt 1234 / completion 56）"
# 批次 C：model 行改为**逐次模型调用**一条（C++ 同口径），turn 从 0 起连续编号；
# ok 剧本要跑 4 次模型调用（3 轮工具 + 收尾），故判据是 4 条、turn=0..3。
audit_expr "$AAUD" "[l.get('turn') for l in lines if l['kind']=='model'] == [0,1,2,3]" \
	"model 行逐次模型调用各一条（turn 0..3，与 C++ 同口径）"
audit_expr "$AAUD" "all(l.get('req_bytes',0)>0 and l.get('resp_bytes',0)>=0 and l.get('attempts')==1 for l in lines if l['kind']=='model')" \
	"每条 model 行都带请求体字节与尝试次数（attempts=1 = 没重试）"
audit_expr "$AAUD" "not any(l['kind']=='tool' for l in lines)" \
	"工具行**不在** agent 审计文件里（由后端那一侧落，见下条）"
check_absent "$AAUD" "e2e-rust-key" "环境变量密钥不出现在 agent 审计日志"
audit_expr "$BAUD" "{'list_hardware','run_action_script','run_command'} <= {l.get('tool') for l in lines if l['kind']=='tool'} and all('ok' in l and 'bytes' in l and 'ms' in l for l in lines if l['kind']=='tool')" \
	"后端审计里三个工具各留一条记录且字段齐备（工具行归属后端）"

# ---- R4：交互 REPL 与能力门控（后端不开 --agent-shell）----
echo "----- R4：REPL 交互 + 能力门控（无 --agent-shell） -----"
PORT=$((26400 + RANDOM % 300))
CAPR4="$tmp/req-rust4.json"
R4="$tmp/rust4"
# manycalls:1：一轮只要求一次 list_hardware，然后收尾——缺省剧本第三轮要
# run_command，而本场景的后端刻意不开 shell（那是**被测的门控**，不是缺陷）。
start_server "$PORT" "$CAPR4" "manycalls:1" || exit 1
start_backend "$R4" || exit 1
printf '%s\n/status\n/hardware\n/sql INSERT INTO Relay (name, controller, note) VALUES (%s);\n/exit\n' \
	"$TASK" "'relay-x','timer-0','x'" | \
	BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R4/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config none >"$R4/out.txt" 2>"$R4/err.txt"
rcr4=$?
stop_backend
stop_server
echo "===== stdout ====="; cat "$R4/out.txt"
if [ "$rcr4" -ne 0 ]; then
	echo "  [FAIL] mybot-agent 退出码 $rcr4"
	fail=1
fi
check_present "$R4/out.txt" "mybot[relay-test|llm0|" "REPL 提示符（机器人名|驱动变量|硬件数）"
check_present "$R4/out.txt" "AGENT-E2E-完成" "交互输入的任务照常跑完工具循环"
check_present "$R4/out.txt" "会话状态:" "/status 给出会话状态一览"
check_present "$R4/out.txt" "已注册硬件变量（3 个）" "/hardware 经工具层拿到硬件清单（只读）"
check_present "$R4/out.txt" "侧开 --agent-hot-sql" "/sql 的写语句预检点名后端缺的开关（配置文件不能放宽）"
check_json "$CAPR4" "'run_command' not in {t['function']['name'] for t in d['tools']}" \
	"缺省不提供 run_command（无 --agent-shell 时线上工具定义里没有它）"

# ---- R5：--check-config（离线校验配置链，不碰后端）----
echo "----- R5：--check-config（离线） -----"
R5="$tmp/rust5"
mkdir -p "$R5"
CFGR5="$R5/check.json"
cat > "$CFGR5" <<JSON
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
chmod 600 "$CFGR5"
outr5="$(env -u BOT4CJ_LLM_KEY -u ROBOT_AGENT_API_KEY -u DEEPSEEK_API_KEY \
	timeout 20 "$AGENT_BIN" --check-config --socket /tmp/mybot-agent-e2e.sock \
	--llm-config "$CFGR5" 2>&1)"
rcr5=$?
echo "$outr5"
if [ "$rcr5" -ne 0 ]; then
	echo "  [FAIL] --check-config 退出码 $rcr5（配置本身是合法的）"
	fail=1
fi
check "$outr5" "配置校验通过" "--check-config 给出可读摘要"
check "$outr5" "cfg-model-from-file" "模型名来自配置文件"
check "$outr5" "配置文件 api_key 字段" "密钥只报来源、不报值"
# 负路径：缺 endpoint 必须退出 2（fail-closed，没有内置缺省——猜一个只会把
# 上下文发到错的地方）。这条不需要后端也不联网。
outr5b="$(BOT4CJ_LLM_KEY=e2e-rust-key timeout 20 "$AGENT_BIN" --check-config \
	--socket /tmp/mybot-agent-e2e.sock --llm-model m1 --llm-config none 2>&1)"
rcr5b=$?
echo "$outr5b"
if [ "$rcr5b" -eq 2 ]; then
	echo "  [OK] 缺 endpoint 时退出 2（fail-closed，不猜网关）"
else
	echo "  [FAIL] 缺 endpoint 时应退出 2，实际 $rcr5b"
	fail=1
fi

# ---- R6：单轮调用上限（SAN-1 / AG-26；C++ 场景 10 的 Rust 版）----
echo "----- R6：单轮调用上限截断 -----"
PORT=$((26700 + RANDOM % 300))
CAPR6="$tmp/req-rust6.json"
R6="$tmp/rust6"
CFGR6="$R6/cfg.json"
mkdir -p "$R6"
# 上限 2 / 轮次 4：夹具一轮返回 3 条调用 → 第 3 条不派发
cat > "$CFGR6" <<'JSON'
{"max_calls_per_round": 2, "max_turns": 4}
JSON
chmod 600 "$CFGR6"
start_server "$PORT" "$CAPR6" "manycalls:3" || exit 1
start_backend "$R6" --agent-shell --agent-policy "$RUST_POLICY" || exit 1
printf 'task-one\ntask-two\n/exit\n' | \
	BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R6/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config "$CFGR6" >"$R6/out.txt" 2>"$R6/err.txt"
rcr6=$?
stop_backend
stop_server
echo "===== stdout ====="; cat "$R6/out.txt"
echo "===== stderr ====="; cat "$R6/err.txt"
if [ "$rcr6" -ne 0 ]; then
	echo "  [FAIL] mybot-agent 退出码 $rcr6"
	fail=1
fi
# SAN-8①：截断是安全相关事实，操作者必须看得见（C++ 侧同样打这条）
check_present "$R6/err.txt" "[上限] 本轮工具调用超过单轮上限 2 条" "截断事件对操作者可见（stderr）"
# 「不派发」必须如实到达模型：C++ 挂在 assistant 文本，rig 侧挂在工具回执上
check_json "$CAPR6" "any(m.get('role')=='tool' and '未派发：本轮工具调用数超过单轮上限（2 条）' in str(m.get('content','')) for m in d['messages'])" \
	"超限的那条调用以「未派发」回执交给模型（不是执行失败）"
check_json "$CAPR6" "any(m.get('role')=='tool' and '没有产生任何副作用' in str(m.get('content','')) for m in d['messages'])" \
	"未派发回执写明没有副作用（不谎报状态）"
check_json "$CAPR6" "'本轮要查几件事。' in json.dumps(d, ensure_ascii=False)" "夹具确实返回了一轮多条调用（场景有效）"
check_absent "$R6/out.txt" "answered more than once" "服务端没有收到重复 tool_call_id"
check_absent "$R6/out.txt" "Invalid status code 400" "整个会话没有出现 400"
check_absent "$R6/out.txt" "未派发" "未派发回执只给模型，不污染 stdout"
nr6="$(grep -cF 'AGENT-E2E-完成' "$R6/out.txt" || true)"
if [ "${nr6:-0}" -eq 2 ]; then
	echo "  [OK] 截断之后第二个任务照常完成（会话未被永久污染）"
else
	echo "  [FAIL] 两个任务应各拿到一次最终回答，实际 ${nr6:-0} 次"
	fail=1
fi

# ---- R7：限流重试（SAN-3；C++ 场景 11 的 Rust 版）----
echo "----- R7：限流 429 的重试与退避 -----"
PORT=$((27000 + RANDOM % 300))
CFGR7="$tmp/rust7/cfg.json"
AAUDR7="$tmp/rust7/agent-audit.jsonl"
mkdir -p "$tmp/rust7"
# retry_backoff_ms=50（本地退避很短）而服务端 Retry-After: 1 —— 退避必须取大值，
# 故「等够 2 秒」本身就是「服务端说了算」的判据。
cat > "$CFGR7" <<'JSON'
{"max_retries": 3, "retry_backoff_ms": 50}
JSON
chmod 600 "$CFGR7"
start_server "$PORT" "$tmp/req-rust7.json" "429x2ok" || exit 1
start_backend "$tmp/rust7" || exit 1
t0=$(date +%s)
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$tmp/rust7/mcp.sock" \
	--llm-config "$CFGR7" --audit-log "$AAUDR7" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--task "$TASK" >"$tmp/rust7/out.txt" 2>"$tmp/rust7/err.txt"
rcr7=$?
t1=$(date +%s)
stop_backend
stop_server
echo "===== stdout ====="; cat "$tmp/rust7/out.txt"
echo "===== stderr ====="; cat "$tmp/rust7/err.txt"
if [ "$rcr7" -ne 0 ]; then
	echo "  [FAIL] 重试之后任务应当成功，实际退出码 $rcr7"
	fail=1
fi
check_present "$tmp/rust7/out.txt" "AGENT-E2E-完成" "限流重试之后任务完成"
check_present "$tmp/rust7/err.txt" "限流" "重试日志点名限流类错误"
check_present "$tmp/rust7/err.txt" "429" "重试日志带上 429（结构化判据看的是 HTTP 状态码）"
check_present "$tmp/rust7/err.txt" "服务端 Retry-After" "退避采用了服务端给的 Retry-After（而非本地指数退避）"
check_absent "$tmp/rust7/out.txt" "重试" "重试日志只走 stderr，不进 stdout"
# 重试真的发生过：审计的 attempts 字段（批次 C 起由模型装饰器记账）
audit_expr "$AAUDR7" "any(l['kind']=='model' and l.get('attempts')==3 and l.get('ok') is True for l in lines)" \
	"审计如实记下实际尝试次数（attempts=3）"
audit_expr "$AAUDR7" "any(l['kind']=='model' and l.get('req_bytes',0)>0 for l in lines)" \
	"审计记下请求体估算字节（req_bytes）"
elapsed7=$((t1 - t0))
if [ "$elapsed7" -ge 2 ]; then
	echo "  [OK] 退避确实等待了（Retry-After: 1 × 2 次 ≈ 2s，实测 ${elapsed7}s）"
else
	echo "  [FAIL] 退避没有生效（实测 ${elapsed7}s < 2s）"
	fail=1
fi

# R7b：恒 503 —— 重试用尽后如实失败（不谎报成功）
echo "----- R7b：恒 503 时重试用尽后如实失败 -----"
PORT=$((27300 + RANDOM % 300))
start_server "$PORT" "$tmp/req-rust7b.json" "http-503" || exit 1
start_backend "$tmp/rust7b" || exit 1
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$tmp/rust7b/mcp.sock" \
	--llm-config "$CFGR7" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--task "随便问一句" >"$tmp/rust7b-out.txt" 2>"$tmp/rust7b-err.txt"
rcr7b=$?
stop_backend
stop_server
echo "===== stderr ====="; cat "$tmp/rust7b-err.txt"
if [ "$rcr7b" -eq 0 ]; then
	echo "  [FAIL] 恒 503 时任务不该成功"
	fail=1
fi
check_present "$tmp/rust7b-err.txt" "503" "重试用尽后如实报出 503（不谎报成功）"
nr7b="$(grep -cF '退避来源' "$tmp/rust7b-err.txt" || true)"
if [ "${nr7b:-0}" -ge 2 ]; then
	echo "  [OK] 503 被重试了（attempts=3 ⇒ 2 条退避日志）"
else
	echo "  [FAIL] 503 只重试了 ${nr7b:-0} 次（期望 ≥2）"
	fail=1
fi

# ---- R8：单次调用超时（C++ 场景 3 的 Rust 版）----
echo "----- R8：--llm-timeout 生效（单次调用超时） -----"
PORT=$((27600 + RANDOM % 300))
mkdir -p "$tmp/rust8"
CFGR8="$tmp/rust8/cfg.json"
# max_retries=1：本场景量的是**单次超时是否被尊重**，重试由 R7 专门守
cat > "$CFGR8" <<'JSON'
{"max_retries": 1}
JSON
chmod 600 "$CFGR8"
start_server "$PORT" "$tmp/req-rust8.json" slow || exit 1
start_backend "$tmp/rust8" || exit 1
t0=$(date +%s)
BOT4CJ_LLM_KEY=e2e-rust-key timeout 30 "$AGENT_BIN" --socket "$tmp/rust8/mcp.sock" \
	--llm-config "$CFGR8" --llm-timeout 1 \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--task "随便问一句" >"$tmp/rust8/out.txt" 2>"$tmp/rust8/err.txt"
rcr8=$?
t1=$(date +%s)
stop_backend
stop_server
cat "$tmp/rust8/err.txt"
elapsed8=$((t1 - t0))
if [ "$rcr8" -ne 0 ] && [ "$elapsed8" -lt 4 ] && grep -qiF "超时" "$tmp/rust8/err.txt"; then
	echo "  [OK] 1 秒超时在 ${elapsed8}s 内生效（单次调用超时真的被尊重）"
else
	echo "  [FAIL] 超时未生效（rc=$rcr8, 用时 ${elapsed8}s）"
	fail=1
fi

# R8b：SIGINT 取消（D4）——置标志后**不再接新任务**，且进程优雅退出（不是被信号打死）
echo "----- R8b：SIGINT 取消 -----"
PORT=$((27900 + RANDOM % 300))
mkdir -p "$tmp/rust8b"
# slow 剧本：每次模型调用睡 5 秒后给一句纯文本回答（无工具调用）。
# 取消落在**第一次调用在飞时**（sleep 2 后发信号）：这一次调用照常返回（rig 侧
# 没有取消哨兵，在飞请求由 --llm-timeout 封顶——如实偏差已记档），随后 REPL 在
# 循环开头看到停止标志即退出——于是**管道里的第二条任务不会被受理**。
start_server "$PORT" "$tmp/req-rust8b.json" slow || exit 1
start_backend "$tmp/rust8b" || exit 1
t0=$(date +%s)
{ printf '第一件事\n第二件事\n/exit\n'; sleep 8; } | \
	BOT4CJ_LLM_KEY=e2e-rust-key timeout 30 "$AGENT_BIN" --socket "$tmp/rust8b/mcp.sock" \
	--llm-config none \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	>"$tmp/rust8b/out.txt" 2>"$tmp/rust8b/err.txt" &
REPLPID=$!
sleep 2
# SIGINT 必须打给 mybot-agent 本体：$! 是管道末端的 timeout 包装进程，而 GNU
# timeout 收到外部 SIGINT 既不退出也不转发（实测：包装时信号被吞、第二件事
# 照跑；直连进程的取消语义完好——置标志、不再受理、优雅退出 0）。
AGENTPID="$(pgrep -P "$REPLPID" -f mybot-agent 2>/dev/null | head -1 || true)"
kill -INT "${AGENTPID:-$REPLPID}" 2>/dev/null
wait "$REPLPID" 2>/dev/null
rcr8b=$?
t1=$(date +%s)
stop_backend
stop_server
echo "===== stdout ====="; cat "$tmp/rust8b/out.txt"
echo "===== stderr ====="; cat "$tmp/rust8b/err.txt"
if [ "$rcr8b" -eq 0 ]; then
	echo "  [OK] 取消后 REPL 正常退出（退出码 0，不是被信号打死）"
else
	echo "  [FAIL] 取消后应当优雅退出（0），实际 $rcr8b"
	fail=1
fi
check_present "$tmp/rust8b/err.txt" "收到停止信号" "取消事件对操作者可见（stderr）"
# 第二件事没被受理：slow 剧本每受理一条任务就产出一次「慢响应」——按条数判
# （比按耗时判稳：不依赖机器快慢）
n8b="$(grep -cF '慢响应' "$tmp/rust8b/out.txt" || true)"
elapsed8b=$((t1 - t0))
if [ "${n8b:-0}" -eq 1 ]; then
	echo "  [OK] 取消之后不再受理新任务（只受理了第一件事，实测 ${elapsed8b}s）"
else
	echo "  [FAIL] 取消后受理了 ${n8b:-0} 条任务（期望 1 条）"
	fail=1
fi

# ---- R9：图片后注（C++ 场景 5 的 Rust 版）----
echo "----- R9：capture_image 图片以 user 消息后注 -----"
PORT=$((28200 + RANDOM % 300))
CAPR9="$tmp/req-rust9.json"
R9="$tmp/rust9"
PICR9="$tmp/pic-rust.jpg"
mkdir -p "$R9"
python3 - "$PICR9" <<'PICEOF'
import base64, sys
# 1x1 真 JPEG（160 字节，FFD8 开头 / FFD9 结尾）——驱动只搬运字节，用真图便于断言
open(sys.argv[1], "wb").write(base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a"
    "HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA"
    "AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q=="))
PICEOF
start_server "$PORT" "$CAPR9" camera || exit 1
start_backend "$R9" --camera-src "$PICR9" || exit 1
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R9/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config none --task "拍张照片看看周围有什么" >"$R9/out.txt" 2>"$R9/err.txt"
rcr9=$?
stop_backend
stop_server
echo "===== stdout ====="; cat "$R9/out.txt"
if [ "$rcr9" -ne 0 ]; then
	echo "  [FAIL] mybot-agent 退出码 $rcr9"
	fail=1
fi
check_present "$R9/out.txt" "AGENT-E2E-完成" "拍照之后仍能给出最终回答"
# 图片只能走 user 消息（OpenAI 拒绝工具结果里的图片）：报文的次序必须是
# [assistant(tool_calls), tool(回执), user(说明+image_url)]，中间不夹 assistant。
check_json "$CAPR9" "(lambda ms: (lambda ti, ii: bool(ti) and bool(ii) and min(ii) > min(ti) and not any(ms[k].get('role')=='assistant' for k in range(min(ti)+1, min(ii))))([i for i,m in enumerate(ms) if m.get('role')=='tool' and '已拍摄照片' in str(m.get('content',''))], [i for i,m in enumerate(ms) if m.get('role')=='user' and isinstance(m.get('content'), list) and any(isinstance(p,dict) and p.get('type')=='image_url' for p in m['content'])]))(d['messages'])" \
	"图片在工具回执**之后**的 user 消息里（与 C++「图片后注」同序）"
check_json "$CAPR9" "any(m.get('role')=='user' and isinstance(m.get('content'), list) and any(isinstance(p,dict) and p.get('text')=='以上工具返回的图片如下，请结合画面内容继续。' for p in m['content']) and any(isinstance(p,dict) and p.get('type')=='image_url' for p in m['content']) for m in d['messages'])" \
	"图片消息带说明文字（逐字同 C++ 的图片后注）"
check_json "$CAPR9" "any(isinstance(m.get('content'), str) and m.get('role')=='tool' for m in d['messages'])" \
	"工具消息本身只有文本（图片不塞进工具结果）"
check_present "$CAPR9" '"data:image/jpeg;base64,/9j/' "注入的是以 data URL 包装的真 JPEG"
check_present "$CAPR9" "已拍摄照片" "工具回执（含照片说明）经后端帧回来（进的是 tool 消息）"
# 图片的「结果字节」不重复：同一张图在线上只出现一次（注入只作用于请求，不进 transcript）
check_json "$CAPR9" "json.dumps(d, ensure_ascii=False).count('data:image/jpeg;base64,/9j/') == 1" \
	"同一张图在本次请求里只出现一次（不重复注入）"

# ---- R10：stdout 写失败不 panic（2026-09-24 审计 H2）----
# 重定向到 /dev/full：每次 write 必得 ENOSPC，无竞态地复现「stdout 坏了」
# （| head 断管道依赖调度时序，这里要的是确定性）。旧实现的 `println!` 对写
# 失败直接 panic——退出码 101、**审计 session end 行丢失**；检查式写出
# （agent/src/out.rs）必须让进程照常走完收尾：退出码 0、stderr 告警一次、
# 审计里 start/end 齐全。
echo "----- R10：stdout 写失败（>/dev/full）不 panic、审计收尾不丢 -----"
PORT=$((28500 + RANDOM % 300))
R10="$tmp/rust10"
AAUD10="$R10/agent-audit.jsonl"
start_server "$PORT" "$tmp/req-rust10.json" ok || exit 1
start_backend "$R10" --agent-shell --agent-policy "$RUST_POLICY" || exit 1
BOT4CJ_LLM_KEY=e2e-rust-key timeout 60 "$AGENT_BIN" --socket "$R10/mcp.sock" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" --llm-model e2e-model \
	--llm-config none --audit-log "$AAUD10" --task "$TASK" >/dev/full 2>"$R10/err.txt"
rcr10=$?
stop_backend
stop_server
echo "===== stderr ====="; cat "$R10/err.txt"
if [ "$rcr10" -eq 0 ]; then
	echo "  [OK] stdout 全程写失败仍优雅退出（rc=0；101 = println panic）"
else
	echo "  [FAIL] stdout 写失败时退出码 $rcr10（期望 0；101 = println panic）"
	fail=1
fi
check_present "$R10/err.txt" "stdout 写入失败" "写失败对操作者可见（stderr 告警一次）"
check_absent "$R10/err.txt" "panicked" "没有 panic 轨迹"
audit_expr "$AAUD10" "[l.get('status') for l in lines if l['kind']=='session'] == ['start','end']" \
	"审计 start/end 齐全（写失败不吞审计收尾——这是 H2 的判据）"
audit_expr "$AAUD10" "any(l['kind']=='model' and l.get('ok') is True for l in lines)" \
	"模型轮次照常记账（工具循环完整跑完，输出丢失不影响执行）"

if [ "$fail" -eq 0 ]; then
	echo "mybot-agent 端到端测试通过"
	exit 0
fi
echo "mybot-agent 端到端测试存在失败"
exit 1
