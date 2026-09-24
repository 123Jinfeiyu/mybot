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
#   --rust：只跑 R1-R9（mybot-agent；R1-R5 批次 B 骨架、R6-R9 批次 C 循环层），
#           拓扑是「后端 + mybot-agent」两个进程；缺 agent/target/release/mybot-agent
#           时直接报错退出（守门那边按 SKIP 计，见 test/lib/sections/net_agent.sh）。
# 退出码: 0 通过 / 1 失败 / 2 参数用法错 / 125 缺 python3（守门按 SKIP 计）
set -u

BOT="${BOT:-./build/mybot}"
AGENT_BIN="${AGENT_BIN:-./agent/target/release/mybot-agent}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 1

# Rust 段与 C++ 段共用同一套辅助函数与假模型服务，靠 --rust 分派（批次 B 只钉
# 「两个进程接力」这条链；C++ 的 22 个场景里大半是 C++ 侧特性，Rust 侧要到
# 批次 C/D 才逐步补齐）。
RUST_MODE=0
for _arg in "$@"; do
	case "$_arg" in
		--rust) RUST_MODE=1 ;;
		*)
			echo "未知参数: $_arg（用法: bash scripts/agent_e2e.sh [--rust]）"
			exit 2
			;;
	esac
done
unset _arg
if [ "$RUST_MODE" -eq 1 ] && [ ! -x "$AGENT_BIN" ]; then
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
if [ "$RUST_MODE" -eq 1 ]; then
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
	kill -INT "$REPLPID" 2>/dev/null
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

	if [ "$fail" -eq 0 ]; then
		echo "mybot-agent 端到端测试通过"
		exit 0
	fi
	echo "mybot-agent 端到端测试存在失败"
	exit 1
fi

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
# max_retries=1：本场景量的是**单次超时是否被 curl 尊重**（防 setopt 退回裸变参
# 调用导致整型选项被静默忽略）。SAN-3 修好重试判据之后，缺省 max_retries=3 会让
# 「超时」与「重试」叠加（1s×3 次 + 退避 > 4s），把这条判据变成在量重试——
# 重试本身由场景 11 专门守，这里显式关掉，让两个判据各测各的（互不掩盖）。
CFG3="$tmp/cfg-timeout.json"
cat > "$CFG3" <<'JSON'
{"max_retries": 1}
JSON
start_server "$PORT" "" slow || exit 1
t0=$(date +%s)
out3="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 30 "$BOT" --robot=robots/relay-test.sql \
	--agent --llm-config "$CFG3" --task "随便问一句" --llm-timeout 1 \
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

# ---- AG-20：多轮模型调用必须复用同一份 curl 会话（连接与 TLS 跨轮保留）----
# 判据取自驱动侧的见证行（BOT4CJ_HTTP_PROBE=1，默认不开）：本场景有 2 轮模型
# 调用（先要求拍照、再给最终回答）。修复前 M.post 每次新建并销毁会话，
# sessions 计数会随轮数递增；现在必须自始至终是同一个值。
# **不用句柄地址当判据**：libcurl 每次 init 申请同样大的块，free 后 malloc 常把
# 同一地址还回来——反向验证时「每次新建」也过了地址判据（假绿），计数不会。
start_server "$PORT" "$CAP5" camera || exit 1
probe_out="$(BOT4CJ_LLM_KEY=e2e-fake-key BOT4CJ_HTTP_PROBE=1 timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --llm-config none --camera-src "$PIC5" \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "拍张照片看看周围有什么" 2>&1)"
stop_server
reqs="$(printf '%s\n' "$probe_out" | grep -c '\[http-probe\]' || true)"
seqs="$(printf '%s\n' "$probe_out" | grep -o 'sessions=[0-9]*' | sort -u | wc -l)"
if [ "${reqs:-0}" -ge 2 ] && [ "${seqs:-0}" -eq 1 ]; then
	echo "  [OK] AG-20：$reqs 轮模型调用只建了 1 份会话（连接/TLS 跨轮保留）"
else
	echo "  [FAIL] AG-20：期望 ≥2 轮请求且会话计数唯一，实测 请求=$reqs 不同计数=$seqs"
	fail=1
fi

# ============ 场景 5b：配置文件给的 camera_src 不得自动放行（H1） ============
# 覆盖的是**接线**：C++ 单测两端各自成立（buildAgentConfig 记下「来源不是命令行」、
# 工具层据此不传 src_root），但「配置里的 camera_src 一路走到工具层仍被拒」这条只有
# 端到端能钉住——中间少传一个字段，两端用例都照样绿。
# 修复前（src_root 由 src 自己的 dirname 推导，判据恒真）这里会真把文件读出来注入上下文。
echo "----- 场景 5b：配置文件来源的 camera_src 被拒（H1） -----"
CAP5B="$tmp/req-camera-cfg.json"
CFG5B="$tmp/h1-camera.json"
PIC5B="$tmp/h1-secret.jpg"
cp "$PIC5" "$PIC5B"
cat > "$CFG5B" <<JSON
{
  "driver": "LlmOpenAI",
  "endpoint": "http://127.0.0.1:$PORT/v1/chat/completions",
  "model": "h1-model",
  "api_key": "h1-key",
  "camera_src": "$PIC5B"
}
JSON
chmod 600 "$CFG5B"
start_server "$PORT" "$CAP5B" camera || exit 1	# 复用场景 5 的端口（其服务已停）
# 命令行**不给** --camera-src / --camera-src-dir，并清掉驱动侧的环境白名单：
# 三条根目录来源全空 ⇒ 必须走「未放行」这条默认拒绝路径。
out5b="$(env -u BOT4CJ_CAMERA_SRC_DIR BOT4CJ_LLM_KEY=h1-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --llm-config "$CFG5B" \
	--task "拍张照片看看周围有什么" 2>&1)"
rc5b=$?
stop_server
echo "$out5b"
if [ "$rc5b" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc5b"
	fail=1
fi
check "$out5b" "AGENT-E2E-完成" "拍照被拒之后仍能给出最终回答（会话没被打断）"
check "$out5b" "未放行" "工具回执点名「未放行」（默认拒绝，并给出三条出路）"
if echo "$out5b" | grep -qF "已拍摄照片"; then
	echo "  [FAIL] 配置文件来源的 camera_src 竟然拍成功了（H1 未修 / 接线漏了）"
	fail=1
else
	echo "  [OK] 没有产出照片说明（配置文件不能自己给自己开白名单）"
fi
check_json "$CAP5B" "not any(isinstance(m.get('content'), list) and any(p.get('type')=='image_url' for p in m['content']) for m in d['messages'])" \
	"线上报文里不含 image_url（文件内容没有进模型上下文）"
check_absent "$CAP5B" '"data:image/jpeg;base64,/9j/' "报文里没有那张图的 base64"

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

# ============ 场景 7：--audit-log 审计轨迹 ============
echo "----- 场景 7：--audit-log 审计轨迹 -----"
PORT=$((22200 + RANDOM % 300))
CAP7="$tmp/req-audit.json"
AUDIT7="$tmp/audit.jsonl"
start_server "$PORT" "$CAP7" ok || exit 1
out7="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "$TASK" --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--agent-shell --llm-config none --scripts-dir scripts --audit-log "$AUDIT7" 2>&1)"
rc7=$?
stop_server
echo "$out7"
if [ "$rc7" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc7"
	fail=1
fi
check "$out7" "AGENT-E2E-完成" "带审计日志跑通完整工具循环"
check "$out7" "审计日志: $AUDIT7" "会话开始如实告知审计日志落点"
# 审计文件是 JSON Lines（每行一个 JSON），不走 check_json（那是整文件单文档的断言）
audit_check() { # $1=python 真值表达式 $2=说明
	if AUDIT_FILE="$AUDIT7" python3 -c "
import json, os, sys
lines = [json.loads(x) for x in open(os.environ['AUDIT_FILE']) if x.strip()]
sys.exit(0 if ($1) else 1)
" 2>/dev/null; then
		echo "  [OK] $2"
	else
		echo "  [FAIL] $2"
		fail=1
	fi
}
audit_check "all(isinstance(l, dict) and l.get('ts') and l.get('session') and l.get('kind') for l in lines)" \
	"审计日志每行都是合法 JSON 且带 ts/session/kind（所有 kind 共有）"
# SAN-9 后日志里有三类记录（session/model/tool），所以"每行都有 tool/ok"不再成立
# ——工具行的字段判据改成**只对 kind=tool 的行**要求（加法演进的意义正在于此：
# 老表达式 `l['tool']` 仍只命中工具行，但"每行都有"这种写法必须收窄）。
audit_check "all('ts' in l and 'session' in l and 'kind' in l for l in lines)" \
	"每行都带 ts/session/kind（SAN-9 后的共同字段）"
audit_check "all('tool' in l and 'ok' in l and 'bytes' in l and 'ms' in l for l in lines if l['kind']=='tool')" \
	"工具行的字段齐备（kind=tool 过滤）"
audit_check "{'list_hardware','run_action_script','run_command'} <= {l['tool'] for l in lines if l['kind']=='tool'}" \
	"脚本化流程用到的三个工具各留下了审计记录"
audit_check "any(l.get('tool')=='run_command' and 'exit_code' in l for l in lines)" \
	"run_command 记录带 exit_code"
# 环境变量里的密钥绝不能出现在审计日志（遮蔽判据的端到端见证）
check_absent "$AUDIT7" "e2e-fake-key" "环境变量密钥不出现在审计日志"
# SAN-9：会话边界与模型轮次记录（加法演进）——纯文本回答的任务也必须留下 model 行，
# 会话头尾各一条 session 行，且既有工具行的字段一个不少。
python3 - "$AUDIT7" <<'PYEOF'
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
kinds = [l.get("kind") for l in lines]
ok = True
def bad(msg):
    global ok
    ok = False
    print("  [FAIL] " + msg)
if kinds.count("session") != 2:
    bad("应有两条 session 记录（start/end），实际 %d" % kinds.count("session"))
else:
    print("  [OK] 审计含会话头尾两条记录（SAN-9）")
start = next((l for l in lines if l.get("kind") == "session" and l.get("status") == "start"), None)
if start is None or not start.get("driver") or not start.get("endpoint"):
    bad("start 记录应带 driver 与 endpoint")
else:
    print("  [OK] start 记录带驱动与接入点（可按会话复盘配置）")
mdl = [l for l in lines if l.get("kind") == "model"]
if len(mdl) < 2:
    bad("应有至少两条 model 记录（工具循环至少两轮），实际 %d" % len(mdl))
else:
    print("  [OK] 审计含模型轮次记录 %d 条（SAN-9）" % len(mdl))
# usage 只在最终回答那轮返回；至少要有一条带上 provider 给的 token 数
withtok = [l for l in mdl if "prompt_tokens" in l]
if not withtok:
    bad("应至少有一条 model 记录带上 provider 给的 prompt_tokens")
else:
    print("  [OK] token 用量按 provider 给的数记账（%d 条）" % len(withtok))
tools = [l for l in lines if l.get("kind") == "tool"]
if not tools or any("bytes" not in l or "ok" not in l for l in tools):
    bad("既有工具行字段不全（加法演进被破坏）")
else:
    print("  [OK] 既有工具行字段完整（%d 条，加法演进）" % len(tools))
sys.exit(0 if ok else 1)
PYEOF
if [ $? -ne 0 ]; then fail=1; fi

# ============ 场景 8：模型调用可取消（SIGINT → 取消哨兵 → libcurl 中止）============
echo "----- 场景 8：模型调用可取消 -----"
PORT=$((22500 + RANDOM % 300))
start_server "$PORT" "" slow || exit 1	# slow：服务端睡 5s 才答复
BOTOUT="$tmp/cancel-out.txt"
t0=$(date +%s)
BOT4CJ_LLM_KEY=e2e-fake-key "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --task "随便问一句" --llm-timeout 180 \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	> "$BOTOUT" 2>&1 &
BOTPID=$!
sleep 1.2
kill -INT "$BOTPID" 2>/dev/null
wait "$BOTPID"; rc8=$?
t1=$(date +%s)
stop_server
elapsed=$((t1 - t0))
out8="$(cat "$BOTOUT")"
echo "$out8"
# 修复前：首次 SIGINT 只置标志，在飞的模型调用跑满服务端的 5s 才以超时收场；
# 现在哨兵让 libcurl 在约 1s 内中止，报错点名「已取消」而非「超时」。
if [ "$rc8" -ne 0 ] && [ "$elapsed" -lt 5 ] && echo "$out8" | grep -q "请求已取消"; then
	echo "  [OK] SIGINT 在 ${elapsed}s 内中止了模型调用（报错点名取消而非超时）"
else
	echo "  [FAIL] 取消未生效（rc=$rc8, 用时 ${elapsed}s）——哨兵或进度回调可能失效"
	fail=1
fi
# 哨兵不残留：取消完成后宿主要清掉 /tmp 里的文件
if [ -f "/tmp/mybot-cancel-$BOTPID" ]; then
	echo "  [FAIL] 取消哨兵残留在 /tmp/mybot-cancel-$BOTPID"
	fail=1
else
	echo "  [OK] 取消哨兵已清理"
fi

# ============ 场景 9：--agent-policy 策略钩子（外部确认程序）============
echo "----- 场景 9：--agent-policy 策略钩子 -----"
PORT=$((22800 + RANDOM % 300))
CAP9="$tmp/req-policy.json"
# 拒绝版：确认程序恒退出 1
POLICY_DENY="$tmp/policy-deny.json"
cat > "$POLICY_DENY" <<'JSON'
{"require_confirm": ["run_command"], "confirm_command": "/bin/sh -c 'exit 1'"}
JSON
start_server "$PORT" "$CAP9" ok || exit 1
out9="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "$TASK" --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--agent-shell --llm-config none --scripts-dir scripts \
	--agent-policy "$POLICY_DENY" 2>&1)"
rc9=$?
stop_server
echo "$out9"
if [ "$rc9" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc9"
	fail=1
fi
check "$out9" "策略钩子: $POLICY_DENY" "会话开始如实告知策略钩子文件"
check "$out9" "AGENT-E2E-完成" "确认程序拒绝后工具循环仍照常收尾"
check "$out9" "被策略拦截" "run_command 的回执点名被策略拦截"
POLICYOUT="$tmp/policy-deny-out.txt"
echo "$out9" > "$POLICYOUT"
check_absent "$POLICYOUT" "agent-echo-ok" "被拒的 run_command 零副作用（命令没有真的跑）"
check "$out9" "Relay: test=200" "未命中 require_confirm 的 run_action_script 不受影响"

# 放行版：确认程序恒退出 0——同一循环、同一工具表，命令应照常执行
PORT=$((22800 + RANDOM % 300))
CAP9B="$tmp/req-policy-allow.json"
POLICY_ALLOW="$tmp/policy-allow.json"
cat > "$POLICY_ALLOW" <<'JSON'
{"require_confirm": ["run_command"], "confirm_command": "/bin/true"}
JSON
start_server "$PORT" "$CAP9B" ok || exit 1
out9b="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "$TASK" --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--agent-shell --llm-config none --scripts-dir scripts \
	--agent-policy "$POLICY_ALLOW" 2>&1)"
rc9b=$?
stop_server
if [ "$rc9b" -ne 0 ]; then
	echo "  [FAIL] 放行版 mybot 退出码 $rc9b"
	fail=1
fi
check "$out9b" "agent-echo-ok" "确认程序放行时 run_command 照常执行"

# ============ 场景 10：单轮截断不重复 tool_call_id（SAN-1）============
echo "----- 场景 10：单轮调用上限截断（SAN-1） -----"
# 缺陷形态（已在真机上复现）：截断说明挂到 calls.back().id 上，历史里出现两条
# 同 tool_call_id 的 tool 消息；服务端 400 `answered more than once`，而那条
# 消息落在**不可回滚**的历史里（该轮派发已完成、回滚边界已越过）——此后每个
# 新任务都重发同一段历史、拿同一个 400，只有 /clear 或重启能恢复。
# 判据：① 全程无 400；② 截断事件对操作者可见（SAN-8①）；③ 第二个任务照常完成
# （这是"会话没被永久污染"的直接判据）。
PORT=$((23100 + RANDOM % 300))
CAP10="$tmp/req-capped.json"
CFG10="$tmp/cfg-capped.json"
cat > "$CFG10" <<'JSON'
{"max_calls_per_round": 2, "max_turns": 4}
JSON
start_server "$PORT" "$CAP10" "manycalls:3" || exit 1
out10="$(printf 'task-one\ntask-two\n/exit\n' | BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--llm-config "$CFG10" 2>&1)"
rc10=$?
stop_server
echo "$out10"
printf '%s\n' "$out10" > "$tmp/capped.out"
if [ "$rc10" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc10"
	fail=1
fi
check_absent "$tmp/capped.out" "answered more than once" "服务端没有收到重复 tool_call_id"
check_absent "$tmp/capped.out" "HTTP 400" "整个会话没有出现 400"
check "$out10" "本轮 3 条工具调用超过单轮上限 2 条，已丢弃 1 条未派发" "截断事件对操作者可见（SAN-8①）"
# 最终回答出现两次 = 两个任务都跑完了（会话没被永久污染）
n10="$(printf '%s\n' "$out10" | grep -cF 'AGENT-E2E-完成')"
if [ "$n10" -eq 2 ]; then
	echo "  [OK] 截断之后第二个任务照常完成（会话未被永久污染）"
else
	echo "  [FAIL] 两个任务应各拿到一次最终回答，实际 $n10 次"
	fail=1
fi
check_json "$CAP10" "'本轮要查几件事。' in json.dumps(d, ensure_ascii=False)" "夹具确实返回了一轮多条调用（场景有效）"

# ============ 场景 11：限流/5xx 会被重试（SAN-3）============
echo "----- 场景 11：限流 429 的重试与退避（SAN-3） -----"
# 缺陷形态（已实测）：429/503/真实 curl 错误都不匹配 isRetriableError 的
# 六个子串（那些子串在生产路径上没有生产者，只有测试夹具产出），于是
# max_retries 形同虚设——服务端只收到 1 个 POST，任务当场失败。
# 429x2ok：前两次 429 + Retry-After: 1，第三次正常回答。判据：
#   ① 任务最终成功（重试真的发生了）；② 重试日志点名 429；
#   ③ 退避采用了服务端给的 Retry-After（而非本地指数退避）。
PORT=$((23400 + RANDOM % 300))
CFG11="$tmp/cfg-retry.json"
cat > "$CFG11" <<'JSON'
{"max_retries": 3, "retry_backoff_ms": 50}
JSON
start_server "$PORT" "$tmp/req-retry.json" "429x2ok" || exit 1
t0=$(date +%s)
out11="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "$TASK" --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--agent-shell --llm-config "$CFG11" 2>&1)"
rc11=$?
t1=$(date +%s)
stop_server
echo "$out11"
if [ "$rc11" -ne 0 ]; then
	echo "  [FAIL] 重试之后任务应当成功，实际退出码 $rc11"
	fail=1
fi
check "$out11" "AGENT-E2E-完成" "限流重试之后任务完成"
check "$out11" "429" "重试日志点名了 429"
check "$out11" "服务端 Retry-After" "退避采用了服务端给的 Retry-After（而非本地指数）"
elapsed11=$((t1 - t0))
if [ "$elapsed11" -ge 2 ]; then
	echo "  [OK] 退避确实等待了（Retry-After: 1 × 2 次 ≈ 2s，实测 ${elapsed11}s）"
else
	echo "  [FAIL] 退避没有生效（实测 ${elapsed11}s < 2s）"
	fail=1
fi

# http-503：恒 503。判据：重试**用尽**后如实失败（不谎报成功），且回执里
# 出现多次 503（= 真的重试过，而不是一次就放弃）。
PORT=$((23700 + RANDOM % 300))
CFG11B="$tmp/cfg-503.json"
cat > "$CFG11B" <<'JSON'
{"max_retries": 3, "retry_backoff_ms": 50}
JSON
start_server "$PORT" "$tmp/req-503.json" "http-503" || exit 1
out11b="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "随便问一句" --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--llm-config "$CFG11B" 2>&1)"
rc11b=$?
stop_server
echo "$out11b"
if [ "$rc11b" -eq 0 ]; then
	echo "  [FAIL] 恒 503 时任务不该成功"
	fail=1
fi
check "$out11b" "HTTP 503" "重试用尽后如实报出 503（不谎报成功）"
n11b="$(printf '%s\n' "$out11b" | grep -cF 'HTTP 503')"
if [ "$n11b" -ge 2 ]; then
	echo "  [OK] 503 被重试了（回执里出现 $n11b 次，含每次重试的日志）"
else
	echo "  [FAIL] 503 没有被重试（只出现 $n11b 次）"
	fail=1
fi

# ============ 场景 12：宿主的动作回显不得污染 stdout（SAN-5）============
echo "----- 场景 12：--task 的 stdout 纯净性（SAN-5） -----"
# 缺陷形态（已实测）：ActionRunner 调的是会回显的 play()，而回显落在
# MtVars_Lua::callImpl 的 std::cout 上——于是 `--task` 的 stdout 里混进
# "----------" 与驱动结果表 {"result":"OK",...}，而 docs/agent.md 与
# src/main.cpp 都写着「stdout 只放模型回答」。E2E 此前全程 2>&1 合并两流，
# 结构上看不到这件事；后果是 --task 不能安全地接管道/重定向。
# 判据：宿主自己的两块回显必须**只在 stderr**；模型回答仍在 stdout。
# 注：Lua 驱动自己 print() 的进度行（如 Relay.lua 的 "Relay: test=…"）仍走
# stdout——那是 48 个驱动的既有行为，不在本项范围内，已在 docs 里如实声明。
PORT=$((24000 + RANDOM % 300))
start_server "$PORT" "" ok || exit 1
BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "$TASK" --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--agent-shell --llm-config none --scripts-dir scripts > "$tmp/so12.txt" 2> "$tmp/se12.txt"
rc12=$?
stop_server
echo "===== stdout ====="; cat "$tmp/so12.txt"
echo "===== stderr（尾部）====="; tail -3 "$tmp/se12.txt"
if [ "$rc12" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc12"
	fail=1
fi
check_absent "$tmp/so12.txt" "----------" "宿主的动作分隔线不再进 stdout"
check_absent "$tmp/so12.txt" '"result":"OK"' "驱动的结果表不再进 stdout"
check_present "$tmp/se12.txt" "----------" "动作分隔线出现在 stderr（回显没丢，只是换了流）"
check_present "$tmp/so12.txt" "AGENT-E2E-完成" "模型回答仍在 stdout"

# ============ 场景 13：取消走的是"优雅退出"而不是被信号打死（SAN-7）============
echo "----- 场景 13：取消的退出路径（SAN-7） -----"
# 这条场景的**判据边界**要先说清楚（第一版设计在这里栽过，如实留痕）：
# 我原想用 test/cpp/LifetimeProbe.lua 的 destroy 标记证明"deactivate() 真的执行了"，
# 但反向验证当场推翻——把 main.cpp 里那行 robot.deactivate() 注释掉，marker **照样出现**：
# 因为 ~RobotStruct 会在对象销毁时补一次断电（源码注释里就写着这是兜底）。
# 所以 marker 只能证明"硬件最终被去初始化了"，**分不出**是显式 deactivate 还是析构兜底
# ——拿它当"清理链跑过"的判据是**假绿**。
#
# 这条场景真正能证明的是**退出路径**：首次 SIGINT 之后进程必须走正常返回路径
# （runRobotAndCleanup → deactivate → return 1），而不是被信号直接打死。
# 判据是退出码**恰好 1**：
#   * 被 SIGINT 按缺省处置打死 → 130；
#   * 段错误/abort → 139/134；
#   * 早退什么都没做 → 0。
# 三者都不是 1，所以"恰好 1"是这条路径的可用判据。
PORT=$((24300 + RANDOM % 300))
start_server "$PORT" "" slow || exit 1
BOT4CJ_LLM_KEY=e2e-fake-key "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --task "随便问一句" --llm-timeout 180 \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	> "$tmp/cancel-exit.txt" 2>&1 &
BOTPID=$!
sleep 1.5
kill -INT "$BOTPID" 2>/dev/null
wait "$BOTPID"; rc13=$?
stop_server
out13="$(cat "$tmp/cancel-exit.txt")"
echo "$out13"
check_present "$tmp/cancel-exit.txt" "请求已取消" "首次 SIGINT 中止了在飞的模型调用"
if [ "$rc13" -eq 1 ]; then
	echo "  [OK] 退出码恰好 1：走的是正常返回路径（130=被信号打死 / 139=段错误 / 0=早退都不是它）"
else
	echo "  [FAIL] 退出码应为 1，实际 $rc13（可能被信号直接打死或走了别的路径）"
	fail=1
fi
# 取消哨兵不残留（与场景 8 互补：这边量的是"优雅退出路径上清理也执行了"）
if [ -f "/tmp/mybot-cancel-$BOTPID" ]; then
	echo "  [FAIL] 取消哨兵残留在 /tmp/mybot-cancel-$BOTPID（清理没跑完）"
	fail=1
else
	echo "  [OK] 取消哨兵已清理（优雅退出路径上的清理确实执行了）"
fi

# ============ 场景 14：--agent-hot-sql（结构 SQL 带电生效） ============
# 覆盖的是**接线**：applySql 的事务语义（生效/回滚/拒绝）已由 C++ 单测逐条钉住，
# 这里钉的是端到端那条链——「模型 → 工具派发 → 真机长出新硬件 → 回执列出它」，
# 中间少传一个门控字段或回执吞掉结果，两端单测都照样绿。
echo "----- 场景 14：--agent-hot-sql 结构 SQL 带电生效 -----"
PORT=$((24700 + RANDOM % 300))
CAP14="$tmp/req-hotsql.json"
start_server "$PORT" "$CAP14" hotsql || exit 1
out14="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--agent-hot-sql --llm-config none --scripts-dir scripts \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "给机器人加一路继电器" 2>&1)"
rc14=$?
stop_server
echo "$out14"
if [ "$rc14" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc14"
	fail=1
fi
check "$out14" "AGENT-E2E-完成" "热插之后工具循环仍能收尾"
check "$out14" "已生效：新增硬件变量 1 个" "apply_sql 回执给出了生效摘要"
check "$out14" "relay-hot" "新硬件变量真的注册，并出现在后续 list_hardware 回执里"
check_json "$CAP14" "'apply_sql' in {t['function']['name'] for t in d.get('tools',[])}" \
	"--agent-hot-sql 时 apply_sql 出现在工具定义里"

# ============ 场景 14b：缺 --agent-hot-sql 时 apply_sql 不可用 ============
# 同一条剧本（模型照样要求 apply_sql）的反向判据：工具没注册 ⇒ 回执是「未知工具」，
# 且硬件清单与结构都不变。这是「只能命令行开」这条门控在模型面上的表现——
# 配置文件的 allow_hot_sql 连加载都过不去，由 C++ 单测与 hardening 段各自钉住。
echo "----- 场景 14b：缺 --agent-hot-sql 时 apply_sql 不可用 -----"
CAP14B="$tmp/req-hotsql-off.json"
start_server "$PORT" "$CAP14B" hotsql || exit 1	# 复用场景 14 的端口（其服务已停）
out14b="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "给机器人加一路继电器" 2>&1)"
rc14b=$?
stop_server
echo "$out14b"
if [ "$rc14b" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc14b"
	fail=1
fi
check "$out14b" "未知工具" "缺开关时 apply_sql 不在工具表内（回执点名未知工具）"
if echo "$out14b" | grep -qF "已生效："; then
	echo "  [FAIL] 缺开关时不该有任何硬件变更生效"
	fail=1
else
	echo "  [OK] 缺开关时零硬件变更（没有生效摘要）"
fi
check "$out14b" "AGENT-E2E-完成" "未知工具之后循环照常收尾（不是会话事故）"
check_json "$CAP14B" "'apply_sql' not in {t['function']['name'] for t in d.get('tools',[])}" \
	"缺开关时线上工具定义里没有 apply_sql"

# ============ 场景 14c：热删除（DELETE 真的断电摘器件） ============
# 与场景 14 同门控、同一条链，但方向相反：插一行临时的 → DELETE 掉 → 列硬件。
# 判据是「变量真的从清单里消失」+「回执报出了删了谁」——少了任何一环，
# 模型都会以为删成功了而器件其实还在（或反之），而两端单测各自都绿。
echo "----- 场景 14c：--agent-hot-sql 的热删除（DELETE FROM ... WHERE） -----"
PORT=$((24700 + RANDOM % 300))
CAP14C="$tmp/req-hotdelete.json"
start_server "$PORT" "$CAP14C" hotdelete || exit 1
out14c="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--agent-hot-sql --llm-config none --scripts-dir scripts \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "先加一路临时继电器，然后把它拆掉" 2>&1)"
rc14c=$?
stop_server
echo "$out14c"
if [ "$rc14c" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc14c"
	fail=1
fi
check "$out14c" "已生效：新增硬件变量 1 个" "先插的行真的生效了"
check "$out14c" "已删除硬件变量 1 个（relay-tmp）" "DELETE 回执报出了删掉的是谁"
check "$out14c" "AGENT-E2E-完成" "热删除之后工具循环仍能收尾"
# 删除必须**真的**摘掉变量：第三轮 list_hardware 的回执里不得再有 relay-tmp
if echo "$out14c" | grep -qF "relay-tmp"; then
	# 只允许出现在「插入」与「删除」两条回执里；list_hardware 的清单段不得含它。
	if echo "$out14c" | awk '/已注册硬件变量/{f=1} f&&/relay-tmp/{print; exit}' | grep -q .; then
		echo "  [FAIL] 删除之后硬件清单里仍有 relay-tmp（DELETE 没真的摘掉变量）"
		fail=1
	else
		echo "  [OK] 删除后硬件清单里不再有 relay-tmp"
	fi
fi
check_json "$CAP14C" "'apply_sql' in {t['function']['name'] for t in d.get('tools',[])}" \
	"热删除与热插同门控（apply_sql 在工具定义里）"

# ============ 场景 15：--read-var（主动回读硬件变量） ============
# 覆盖的是**接线**：可读集合的解析与截断/消毒已由 C++ 单测逐条钉住，这里钉端到端
# 那条链——「模型 → 工具派发 → 真读到驱动返回表 → 回执带值」，以及工具真的出现在
# 线上工具定义里（少传一个字段、回执吞掉返回值，两端单测都会漏）。
echo "----- 场景 15：--read-var 主动回读硬件变量 -----"
PORT=$((25000 + RANDOM % 300))
CAP15="$tmp/req-readvars.json"
start_server "$PORT" "$CAP15" readvars || exit 1
out15="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--read-var relay0 --llm-config none --scripts-dir scripts \
	--llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "现在传感器读数是多少" 2>&1)"
rc15=$?
stop_server
echo "$out15"
if [ "$rc15" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc15"
	fail=1
fi
check "$out15" "AGENT-E2E-完成" "回读之后工具循环仍能收尾"
check "$out15" "slept" "read_variables 回执带回了驱动返回表里的读数"
check_json "$CAP15" "'read_variables' in {t['function']['name'] for t in d.get('tools',[])}" \
	"--read-var 时 read_variables 出现在工具定义里"

# ============ 场景 15b：缺 --read-var 时 read_variables 不可用 ============
# 同一条剧本的反向判据：缺省不注册该工具 ⇒ 模型那次调用只剩「未知工具」，
# 且线上工具定义里没有它（"缺省即无此能力"这条口径在模型面上的表现）。
echo "----- 场景 15b：缺 --read-var 时 read_variables 不可用 -----"
CAP15B="$tmp/req-readvars-off.json"
start_server "$PORT" "$CAP15B" readvars || exit 1	# 复用场景 15 的端口（其服务已停）
out15b="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--task "现在传感器读数是多少" 2>&1)"
rc15b=$?
stop_server
echo "$out15b"
if [ "$rc15b" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc15b"
	fail=1
fi
check "$out15b" "未知工具" "缺 --read-var 时 read_variables 不在工具表内（回执点名未知工具）"
check "$out15b" "AGENT-E2E-完成" "未知工具之后循环照常收尾"
check_json "$CAP15B" "'read_variables' not in {t['function']['name'] for t in d.get('tools',[])}" \
	"缺 --read-var 时线上工具定义里没有 read_variables"

# ============ 场景 19：会话持久化与恢复（--session-file / --resume）============
# 两个进程接力：第一个进程跑一个任务并落盘会话文件；第二个进程带 --resume 启动，
# 断言它**首个请求**里带着第一个进程的对话——"重启不忘记"的唯一判据。
# 附两条对照：指纹不符（换结构脚本）拒绝恢复；--resume 缺 --session-file 拒绝启动。
echo "----- 场景 19：--session-file / --resume（会话持久化） -----"
PORT=$((21200 + RANDOM % 300))
SESS="$tmp/session.jsonl"
CAP19A="$tmp/req-session-a.json"
start_server "$PORT" "$CAP19A" ok || exit 1
out19a="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--session-file "$SESS" --task "先让继电器动作一次" 2>&1)"
rc19a=$?
echo "$out19a"
if [ "$rc19a" -ne 0 ]; then
	echo "  [FAIL] 第一个进程退出码 $rc19a"
	fail=1
fi
check_present "$SESS" "session_head" "会话文件首行是会话头"
check_present "$SESS" "先让继电器动作一次" "任务文本已落盘"
check_absent "$SESS" "data:image" "会话文件里没有图片 data URL（写入期排除）"
# 落盘单位是任务块：跑完后文件里不该有半截（每个 tool_call_id 都有对应 tool 行）。
# 判据用 python 直接行使与 C++ 侧 historyProtocolViolation 同源的检查。
if python3 - "$SESS" <<'PY'
import json, sys
declared, answered = [], []
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    d = json.loads(line)
    for c in d.get("tool_calls", []):
        declared.append(c["id"])
    if d.get("role") == "tool":
        answered.append(d.get("tool_call_id"))
ok = all(i in answered for i in declared) and len(set(answered)) == len(answered)
sys.exit(0 if ok else 1)
PY
then
	echo "  [OK] 会话文件里的工具调用序列协议自洽（无孤儿/无重复/无漏答）"
else
	echo "  [FAIL] 会话文件里的工具调用序列不协议自洽"
	fail=1
fi

# 第二个进程：--resume 恢复，首个请求必须带上前一个进程的对话
CAP19B="$tmp/req-session-b.json"
stop_server
start_server "$PORT" "$CAP19B" ok || exit 1
out19b="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--session-file "$SESS" --resume --task "刚才做了什么" 2>&1)"
rc19b=$?
stop_server
echo "$out19b"
if [ "$rc19b" -ne 0 ]; then
	echo "  [FAIL] 恢复进程退出码 $rc19b"
	fail=1
fi
check "$out19b" "已恢复会话" "启动时如实报告已恢复"
check_json "$CAP19B" "any(m.get('role')=='user' and '先让继电器动作一次' in str(m.get('content','')) for m in d.get('messages',[]))" \
	"恢复后的首个请求里带着上一个进程的任务（重启不忘记）"
check_json "$CAP19B" "any(m.get('role')=='assistant' and str(m.get('content','')) for m in d.get('messages',[]))" \
	"恢复后的请求里带着上一个进程的回答"

# 对照一：指纹不符 → 拒绝恢复并退出 2。
# 用 --prompt-file 变更**系统提示词指纹**（而不是换结构脚本）：换结构脚本会连带
# 换硬件清单，而无硬件的守门环境里 spider1 这类脚本在上电阶段就先失败了——
# 指纹核对在系统提示词定稿之后（需要硬件清单），所以这里改提示词这一轴，
# 让被测的判据单独暴露（三轴同判，robot/tools/system 任一不符都拒绝）。
printf '# 额外约束\n只做被要求的事。\n' > "$tmp/extra-prompt.md"
out19c="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--prompt-file "$tmp/extra-prompt.md" \
	--session-file "$SESS" --resume --task "不该跑起来" 2>&1)"
rc19c=$?
echo "$out19c"
if [ "$rc19c" -eq 2 ]; then
	echo "  [OK] 指纹不符时拒绝恢复并以退出码 2 结束"
else
	echo "  [FAIL] 指纹不符时应退出 2，实际 $rc19c"
	fail=1
fi
check "$out19c" "拒绝恢复" "拒绝恢复的文案点名了原因"
check "$out19c" "系统提示词不同" "拒绝的理由点名是哪一轴不一致"

# 对照二：--resume 缺 --session-file → 退出 2（恢复只认命令行路径）
out19d="$(BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--resume --task "不该跑起来" 2>&1)"
rc19d=$?
echo "$out19d"
if [ "$rc19d" -eq 2 ]; then
	echo "  [OK] --resume 缺 --session-file 时退出 2"
else
	echo "  [FAIL] --resume 缺 --session-file 时应退出 2，实际 $rc19d"
	fail=1
fi

# ============ 场景 20：agent over TCP（--agent --tcp，远程 REPL）============
# 远程对话 = 远程工具调用，安全口径与动作源 TCP 同款；本场景验证四件事：
#   ① 全链路：token → 任务 → 工具循环 → 回答逐行回写连接；
#   ② 错 token 被拒（且不进入会话）；
#   ③ 空闲超时按「输入结束」收尾（半开连接不会挂死进程）；
#   ④ 发任务后断开连接 = 远程取消：在飞的模型调用被立即中止
#      （判据是**耗时**——等满 --llm-timeout 会慢一个数量级）。
echo "----- 场景 20：agent over TCP（远程 REPL） -----"
PY20="$tmp/tcp_client.py"
cat > "$PY20" <<'PYEOF'
import socket, sys, time

port, mode = int(sys.argv[1]), sys.argv[2]

def read_some(s, seconds=3.0):
    s.settimeout(seconds)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except Exception:
        pass
    return data.decode(errors="replace")

if mode == "roundtrip":
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall((sys.argv[3] + "\n").encode())
    time.sleep(0.5)
    s.sendall("让继电器动作一次\n".encode())
    time.sleep(3.5)
    s.sendall("/exit\n".encode())
    sys.stdout.write(read_some(s, 3.0))
    s.close()
elif mode == "wrongtoken":
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(b"wrong-token\n")
    sys.stdout.write(read_some(s, 3.0))
    s.close()
elif mode == "idle":
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    t0 = time.time()
    data = read_some(s, 12.0)
    sys.stdout.write("idle_wait=%.1fs\n" % (time.time() - t0))
    sys.stdout.write(data)
    s.close()
elif mode == "disconnect":
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    time.sleep(0.4)
    s.sendall("跑个慢任务\n".encode())
    time.sleep(1.0)
    s.close()
PYEOF

# ---- 20a：全链路（回环缺省 + 显式 token）----
PORT=$((21400 + RANDOM % 200))
CAP20="$tmp/req-agenttcp.json"
start_server "$PORT" "$CAP20" ok || exit 1
TCPPORT=$((21600 + RANDOM % 200))
BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --tcp "$TCPPORT" --tcp-token e2e-tok \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	> "$tmp/tcp-server-a.log" 2>&1 &
SRVAPP=$!
sleep 1.5
out20a="$(timeout 30 python3 "$PY20" "$TCPPORT" roundtrip e2e-tok 2>&1)"
echo "$out20a"
check "$out20a" "mybot[relay-test" "远程 REPL 的提示符回写到连接"
check "$out20a" "AGENT-E2E-完成" "模型回答经连接回写（工具循环跑通）"
check_json "$CAP20" "any(m.get('role')=='user' and '让继电器动作一次' in str(m.get('content','')) for m in d.get('messages',[]))" \
	"经 TCP 下发的任务真的进了模型请求"
wait "$SRVAPP" 2>/dev/null
stop_server

# ---- 20b：错 token 被拒 ----
PORT=$((21400 + RANDOM % 200))
start_server "$PORT" "" ok || exit 1
TCPPORT=$((21600 + RANDOM % 200))
BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --tcp "$TCPPORT" --tcp-token e2e-tok \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	> "$tmp/tcp-server-b.log" 2>&1 &
SRVAPP=$!
sleep 1.5
out20b="$(timeout 30 python3 "$PY20" "$TCPPORT" wrongtoken 2>&1)"
sleep 0.5
kill "$SRVAPP" 2>/dev/null; wait "$SRVAPP" 2>/dev/null
stop_server
echo "$out20b"
check_present "$tmp/tcp-server-b.log" "口令校验失败" "错 token 被显式拒绝（服务端点名）"

# ---- 20c：空闲超时按输入结束收尾（半开连接不挂死）----
PORT=$((21400 + RANDOM % 200))
start_server "$PORT" "" ok || exit 1
TCPPORT=$((21600 + RANDOM % 200))
BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --tcp "$TCPPORT" --tcp-idle 2 \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	> "$tmp/tcp-server-c.log" 2>&1 &
SRVAPP=$!
sleep 1.5
out20c="$(timeout 30 python3 "$PY20" "$TCPPORT" idle 2>&1)"
wait "$SRVAPP"; rc20c=$?
stop_server
echo "$out20c"
check "$out20c" "idle_wait" "客户端观察到了会话结束（连接被服务端收尾）"
if [ "$rc20c" -eq 0 ]; then
	echo "  [OK] 空闲超时后进程按正常结束退出（rc=0，非挂死/强杀）"
else
	echo "  [FAIL] 空闲超时后退出码 $rc20c（应为 0）"
	fail=1
fi

# ---- 20d：断开连接 = 远程取消（在飞模型调用被立即中止）----
PORT=$((21400 + RANDOM % 200))
start_server "$PORT" "" slow || exit 1	# slow：服务端睡 5s 才答复
TCPPORT=$((21600 + RANDOM % 200))
BOT4CJ_LLM_KEY=e2e-fake-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --tcp "$TCPPORT" \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/chat/completions" \
	--llm-timeout 60 \
	> "$tmp/tcp-server-d.log" 2>&1 &
SRVAPP=$!
sleep 1.5
t_start=$(date +%s)
timeout 30 python3 "$PY20" "$TCPPORT" disconnect 2>&1
wait "$SRVAPP"; rc20d=$?
t_end=$(date +%s)
elapsed=$((t_end - t_start))
stop_server
echo "  远程取消耗时 ${elapsed}s（等满 --llm-timeout 会是 60s 级）"
# 阈值 4s：慢速假服务自己睡 5s，所以"没被取消"的路径至少 5s 才可能结束——
# 阈值必须**低于**它才有判别力（15s 这种阈值等于永远通过）。
if [ "$elapsed" -le 4 ]; then
	echo "  [OK] 断开连接后立即中止在飞调用（${elapsed}s ≪ 60s）"
else
	echo "  [FAIL] 断开连接后仍等了 ${elapsed}s——取消通道没生效"
	fail=1
fi
check_present "$tmp/tcp-server-d.log" "请求已取消" "取消走的是「已取消」语义（不是超时/强杀）"
if [ "$rc20d" -eq 0 ]; then
	echo "  [OK] 取消后按优雅退出收尾（rc=0）"
else
	echo "  [FAIL] 取消后退出码 $rc20d（应为 0）"
	fail=1
fi
if ls /tmp/mybot-cancel-* >/dev/null 2>&1; then
	echo "  [FAIL] 取消哨兵文件残留（应随进程退出清理）"
	fail=1
else
	echo "  [OK] 取消哨兵无残留"
fi

# ============ 场景 21：LlmAnthropic（第三个协议驱动，批次 3）============
# 「换模型 = 换 lua 文件」的验证实例：同一份 mybot 二进制、同一条工具循环，
# 只换 --llm-driver，报文就换成 Anthropic Messages 形态。断言分两层：
#   ① 形状差异真的被驱动抹平（system 顶层 / tool_use 块 / tool_result 并进
#      user 消息 / input_schema / required 为数组 / 带 anthropic-version 与
#      x-api-key 而不是 Authorization）；
#   ② 工具循环照常跑通（动作脚本在常驻机器人上真被执行 + 最终回答回写 stdout）。
# 宿主零改是本场景的前提（同一二进制在场景 1 里跑的是 OpenAI 形态）。
echo "----- 场景 21：LlmAnthropic（第三个协议驱动） -----"
PORT=$((21800 + RANDOM % 200))
CAP21="$tmp/req-anthropic.json"
HDR21="$tmp/hdr-anthropic.txt"
start_server "$PORT" "$CAP21" anthropic "$HDR21" || exit 1
out21="$(BOT4CJ_LLM_KEY=e2e-anthropic-key timeout 60 "$BOT" \
	--robot=robots/relay-test.sql --agent --llm-driver LlmAnthropic \
	--llm-config none --llm-endpoint "http://127.0.0.1:$PORT/v1/messages" \
	--task "先看硬件，再让继电器动作一次" 2>&1)"
rc21=$?
stop_server
echo "$out21"
if [ "$rc21" -ne 0 ]; then
	echo "  [FAIL] mybot 退出码 $rc21"
	fail=1
fi
check "$out21" "已执行动作脚本 relay-test.mt" "工具循环跑通（动作脚本真的被执行）"
check "$out21" "AGENT-E2E-完成" "最终回答经既有 stdout 通道返回"
check_json "$CAP21" "'system' in d and all(m['role'] != 'system' for m in d['messages'])" \
	"system 提示词抽到顶层字段（Anthropic 形态），不进 messages"
# 过渡语在前是合法形态（模型可以先说一句再调用），所以判据是「存在 tool_use 块
# 且 input 是对象」，而不是「第一个块必须是 tool_use」——写死索引的判据会把
# 正确的报文判红（本场景首跑就是这么红的）。
check_json "$CAP21" "any(b.get('type') == 'tool_use' and isinstance(b.get('input'), dict) for b in d['messages'][1]['content'])" \
	"工具调用表达为 tool_use 块且 input 是对象（规范化侧是字符串）"
check_json "$CAP21" "d['messages'][2]['role'] == 'user' and d['messages'][2]['content'][0]['type'] == 'tool_result'" \
	"工具结果是 user 消息里的 tool_result 块（不是独立的 tool 角色）"
check_json "$CAP21" "all('input_schema' in t and 'function' not in t for t in d['tools'])" \
	"工具声明用 input_schema（无 function 外壳）"
check_json "$CAP21" "all(isinstance(t['input_schema'].get('required'), list) for t in d['tools'])" \
	"input_schema.required 是数组（空对象形态会被真机 400）"
check_json "$CAP21" "all(d['messages'][i]['role'] != d['messages'][i-1]['role'] for i in range(1, len(d['messages'])))" \
	"messages 严格 user/assistant 交替（本协议的硬要求）"
check_present "$HDR21" "x-api-key: e2e-anthropic-key" "鉴权走 x-api-key 头"
check_absent "$HDR21" "Authorization: Bearer" "不用 OpenAI 系的 Authorization: Bearer"
check_present "$HDR21" "anthropic-version: 2023-06-01" "带 anthropic-version 头"

if [ "$fail" -eq 0 ]; then
	echo "agent 模式端到端测试通过"
	exit 0
fi
echo "agent 模式端到端测试存在失败"
exit 1
