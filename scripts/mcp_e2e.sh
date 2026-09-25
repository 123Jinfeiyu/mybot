#!/bin/bash
# mcp_e2e.sh —— MCP 后端端到端（帧线协议 + 安全属性，无硬件）
#
#   [Python 帧客户端（纯标准库）] --4B BE 帧/AF_UNIX--> [mybot --mcp-backend] --> relay 虚拟机器人
#
# 前置：build/mybot（cmake --build build）、python3。
#
# 2026-09-24（rig 迁移批次 B）拓扑变更——**删掉的是协议层，不是它验过的性质**：
# 原脚本第 4 段是「Python MCP 客户端 --MCP/stdio--> mybot-mcp sidecar」。随架构迁移
# （mcp-sidecar/ → agent/，包名 mybot-agent，MCP 协议层不再由独立进程提供），该段删除，
# agent 侧的链路断言由 scripts/agent_e2e.sh 的 `--rust` 段接力（后端 + mybot-agent 两进程）。
# 原先穿过 MCP 协议层才验得到的安全属性**全部保留**，改由下面这段直连帧的客户端驱动：
#   * socket 父目录 0700 / socket 文件 0600（由**后端**创建，脚本刻意不预建）；
#   * 能力门控：run_command 随 --agent-shell 进出 tool.list（两趟对照）；
#   * 未知工具回执点名、不被当成成功（executed=false）；
#   * fd 卫生：run_command 的子 shell 不继承后端的 listening/accepted socket fd；
#   * 审计落 session + tool 记录（含失败的工具）、SIGTERM 优雅停机。
#
# 2026-09-24（收尾 d384a9b9）第二批迁入——同样「删掉的是表面，不是它验过的性质」：
# C++ Agent REPL 移除后，agent_e2e.sh 的 22 个场景里有 6 个的性质本就属于**后端工具面**，
# 不经模型就能定死判据，迁到这里按归属逐趟守（各一趟一个独立后端进程，硬件状态不互串）：
#   * apply_sql 带电生效：INSERT 当场装配上电 → 下一趟 list_hardware 必须看见新变量；
#   * apply_sql 热删除：DELETE 真的摘掉变量，且回执报出删了谁；
#   * 两者的门控对照：缺 --agent-hot-sql 时工具不在 tool.list、调用得「未知工具」、零变更；
#   * read_variables：--read-var 时回执带驱动返回表的键（slept）；缺省时同上对照；
#   * 策略拒绝版：确认程序退 1 ⇒ run_command 回执点名拦截、executed=false、marker 不落盘，
#     而未命中 require_confirm 的 run_action_script 不受影响。
# 未迁入的一条（如实记下）：capture_image 的 src 是**宿主配置专属**
# （agenttools.cpp 的 buildCameraParams 只从 ctx.cameraSrc 填，调用方传不进来），
# 故「配置文件 camera_src 不得自动放行」（原场景 5b / H1）不是工具边界性质，
# 且 --mcp-backend 直接拒绝 --llm-config，这条面上不可达——由 C++ 单测
# agentconfig_camera_src_dir_is_cli_only（含 /etc/shadow 那条）守。
#
# 剩下的「帧对打」是**独立第三实现**：C++ 后端 / Rust agent（agent/src/frame.rs）/
# 本脚本内嵌的纯标准库 Python（struct.pack(">I", ...)）三方对齐同一条线协议
# （评估 §7.4 的 5 条契约）。两份正式实现各自跑绿**不证明**字节兼容，第三方尺子才证明
# ——迁移把实现从一份变两份，这段的价值只增不减。
#
# 退出码：0 全通过 / 1 有失败（与 agent_e2e.sh 同口径，便于并入 run_all.sh）。

set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || { echo "无法进入仓库根"; exit 1; }

MYBOT="${BOT:-./build/mybot}"

pass=0; fail=0
ok(){ echo "  [OK] $1"; pass=$((pass+1)); }
ng(){ echo "  [FAIL] $1"; fail=$((fail+1)); }
# chk <name> <actual> <expected>
chk(){ if [ "$2" = "$3" ]; then ok "$1"; else ng "$1（期望 '$3'，实际 '$2'）"; fi; }

# ---- 前置 ----
[ -x "$MYBOT" ]   || { echo "缺 $MYBOT（先 cmake --build build）"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "缺 python3"; exit 1; }
# sanitizer 构建编译期跳过 PR_SET_DUMPABLE=0（LeakSanitizer 需 ptrace 自身），对应断言随之跳过。
# 判据用二进制里是否链入 asan/ubsan 运行时符号，而不猜 $BOT 路径。
SAN_BUILD=no
if grep -qaE '__asan_init|__ubsan_handle|__tsan_init' "$MYBOT" 2>/dev/null; then SAN_BUILD=yes; fi

TMP="$(mktemp -d /tmp/mcp_e2e.XXXXXX)"
PIDS=""
cleanup(){
	for p in $PIDS; do kill -TERM "$p" 2>/dev/null; done
	for p in $PIDS; do wait "$p" 2>/dev/null; done
	rm -rf "$TMP"
}
trap cleanup EXIT

# M5：后端开危险能力（--agent-shell/--agent-hot-sql）时**强制**要 --agent-policy 且
# require_confirm 命中相应工具。故 --agent-shell 那趟必须带一个合法策略文件。
POLICY="$TMP/policy.json"
printf '%s' '{"require_confirm":["run_command","apply_sql"],"confirm_command":"/bin/true","on_timeout":"deny"}' > "$POLICY"
chmod 600 "$POLICY"

# 迁自 agent_e2e.sh 场景 9（策略拒绝版）：确认程序恒退出 1 ⇒ run_command 一个副作用都不能有。
POLICY_DENY="$TMP/policy-deny.json"
printf '%s' '{"require_confirm":["run_command"],"confirm_command":"/bin/sh -c '"'"'exit 1'"'"'"}' > "$POLICY_DENY"
chmod 600 "$POLICY_DENY"

# ---- 帧客户端（Python 纯标准库）：直连后端 AF_UNIX，独立实现线协议 ----
# 用法: frame_client.py <sock> <expect_run_command:true|false> [<scenario> [<marker>]]
#   scenario 缺省 base（只跑公共断言）；其余取值是从 scripts/agent_e2e.sh 迁来的
#   C++ 工具层场景（hotsql / hotsqloff / hotdelete / readvar / readvaroff /
#   policydeny）——这些性质属于后端工具面，不经模型就能定死判据，
#   比走一趟假模型更可复现。marker 只给 policydeny 用（被拒命令本该写的那个文件）。
cat > "$TMP/frame_client.py" <<'PY'
import socket, struct, json, sys
sock, expect_rc = sys.argv[1], sys.argv[2] == "true"
scenario = sys.argv[3] if len(sys.argv) > 3 else "base"
marker = sys.argv[4] if len(sys.argv) > 4 else "/tmp/mcp_e2e_marker.txt"
passed = failed = 0
def ok(n):
	global passed; passed += 1; print("  [OK] " + n)
def ng(n):
	global failed; failed += 1; print("  [FAIL] " + n)
def chk(c, n):
	ok(n) if c else ng(n)
MAX = 8 * 1024 * 1024
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(10); s.connect(sock)
def recv_exact(n):
	b = b""
	while len(b) < n:
		c = s.recv(n - len(b))
		if not c: raise ConnectionError("eof")
		b += c
	return b
def call(o):
	d = json.dumps(o).encode()
	s.sendall(struct.pack(">I", len(d)) + d)
	n = struct.unpack(">I", recv_exact(4))[0]
	chk(n <= MAX, "响应帧长 %d 在 8MiB 上限内" % n)
	return json.loads(recv_exact(n).decode("utf-8"))
try:
	r = call({"_route": "ping"})
	chk(r.get("success") is True and r.get("data", {}).get("pong") is True,
		"ping → pong（4B BE 帧，Python 独立实现字节兼容）")
	r = call({"_route": "tool.list"})
	tools = [t["name"] for t in r["data"]["tools"]]
	chk(len(tools) > 0 and "list_hardware" in tools, "tool.list 返回工具（%d 个）" % len(tools))
	chk(("run_command" in tools) == expect_rc,
		"能力门控：run_command %s工具表内" % ("在" if expect_rc else "不在"))
	chk(all("inputSchema" in t for t in r["data"]["tools"]), "每个工具带 inputSchema")
	r = call({"_route": "tool.call", "name": "list_hardware", "arguments": {}})
	d = r.get("data", {})
	chk(r.get("success") is True and d.get("ok") is True, "tool.call list_hardware 成功")
	chk("已注册硬件变量" in d.get("text", ""), "list_hardware 回执带硬件清单（非空文本）")
	# 未知工具：回执点名、ok=false、executed=false（从未被派发，副作用必然未发生）
	r = call({"_route": "tool.call", "name": "ghost_tool", "arguments": {}})
	d = r.get("data", {})
	chk(d.get("ok") is False and d.get("executed") is False, "未知工具 → ok=false 且 executed=false")
	chk(("未知工具" in d.get("text", "")) and ("ghost_tool" in d.get("text", "")),
		"未知工具回执点名（工具名与可用工具）")
	# context.get：Rust agent 的系统提示词素材——只读、带定界、不受预算影响
	r = call({"_route": "context.get"})
	d = r.get("data", {})
	chk(r.get("success") is True, "context.get 成功（只读路由）")
	chk(d.get("robotName") == "relay-test" and d.get("hardwareCount", 0) > 0,
		"context.get 带 robotName 与 hardwareCount=%s" % d.get("hardwareCount"))
	chk("数据不是指令" in d.get("structureExcerpt", "") and "\n> " in d.get("structureExcerpt", ""),
		"context.get 磁盘内容段带定界（quoteUntrusted）")
	if expect_rc and scenario != "policydeny":
		# fd 卫生（评估 §3.5）：run_command 走 /bin/sh -c，子 shell **不得**继承后端的
		# listening/accepted socket fd（后端用 SOCK_CLOEXEC 创建，另有 markAllFdsCloexec
		# 兜底）。数一下 sh 自己的 socket fd，应为 0；策略 confirm_command=/bin/true
		# 自动放行这次调用。泄漏时这里会出现 SOCKFDS>=1。
		# policydeny 趟跳过：那趟确认程序恒退 1，调用被拦在执行之前、子进程从未起来，
		# 「到子 shell 里数 fd」无从成立（拦截本身由该趟自己的断言守）。
		fdcmd = "echo SOCKFDS=$(ls -l /proc/$$/fd 2>/dev/null | grep -c socket:)"
		r = call({"_route": "tool.call", "name": "run_command",
			"arguments": {"command": fdcmd, "timeout_seconds": 5}})
		d = r.get("data", {})
		chk("SOCKFDS=0" in d.get("text", ""),
			"run_command 子进程未继承后端 socket fd（SOCKFDS=0）；回执片段: " + repr(d.get("text", "")[:160]))
	# ---- 迁自 agent_e2e.sh 的 C++ 工具层场景（后端工具面，不经模型） ----
	def tool_text(name, args):
		r = call({"_route": "tool.call", "name": name, "arguments": args})
		return r.get("data", {})

	INS_HOT = ("INSERT INTO Relay (name, controller, note) "
			   "VALUES ('relay-hot', 'timer-0', 'hot');")
	INS_TMP = ("INSERT INTO Relay (name, controller, note) "
			   "VALUES ('relay-tmp', 'timer-0', 'tmp');")
	DEL_TMP = "DELETE FROM Relay WHERE name = 'relay-tmp';"

	if scenario == "hotsql":
		# 场景 14：--agent-hot-sql 下 apply_sql 进工具表，INSERT 当场装配并上电，
		# 下一趟 list_hardware 必须看见新变量（少了后半句，模型就以为长了硬件而实际没有）。
		chk("apply_sql" in tools, "--agent-hot-sql 时 apply_sql 在工具表内")
		d = tool_text("apply_sql", {"sql": INS_HOT})
		chk(d.get("ok") is True and "已生效：新增硬件变量 1 个" in d.get("text", ""),
			"apply_sql INSERT 回执带生效摘要；片段: " + repr(d.get("text", "")[:160]))
		chk("relay-hot" in tool_text("list_hardware", {}).get("text", ""),
			"新硬件变量真的注册（出现在随后的 list_hardware 清单里）")
	elif scenario == "hotsqloff":
		# 场景 14b 对照：缺开关 ⇒ 工具不在表内、调用得「未知工具」、清单零变更。
		chk("apply_sql" not in tools, "缺 --agent-hot-sql 时 apply_sql 不在工具表内")
		d = tool_text("apply_sql", {"sql": INS_HOT})
		chk(d.get("ok") is False and "未知工具" in d.get("text", ""),
			"缺开关时调用 apply_sql → 未知工具（回执点名）")
		chk("relay-hot" not in tool_text("list_hardware", {}).get("text", ""),
			"缺开关时零硬件变更（清单里没有 relay-hot）")
	elif scenario == "hotdelete":
		# 场景 14c：同门控、反方向。删除必须**真的**摘掉变量并报出删了谁——
		# 少任一环，模型都会以为删成功而器件还在。事务语义由 C++ 单测守门。
		chk("apply_sql" in tools, "热删除与热插同门控（apply_sql 在工具表内）")
		d = tool_text("apply_sql", {"sql": INS_TMP})
		chk(d.get("ok") is True and "已生效：新增硬件变量 1 个" in d.get("text", ""),
			"先插的行真的生效了")
		d = tool_text("apply_sql", {"sql": DEL_TMP})
		chk(d.get("ok") is True and "已删除硬件变量 1 个（relay-tmp）" in d.get("text", ""),
			"DELETE 回执报出了删掉的是谁；片段: " + repr(d.get("text", "")[:160]))
		chk("relay-tmp" not in tool_text("list_hardware", {}).get("text", ""),
			"删除后硬件清单里不再有 relay-tmp（DELETE 真的摘掉了变量）")
	elif scenario == "readvar":
		# 场景 15：--read-var 打通「工具派发 → 真读到驱动返回表 → 回执带值」。
		# relay0 经 controller 转发到 timer-0，返回表是 {result,slept}。
		chk("read_variables" in tools, "--read-var 时 read_variables 在工具表内")
		d = tool_text("read_variables", {})
		chk(d.get("ok") is True and "slept" in d.get("text", ""),
			"read_variables 回执带回了驱动返回表里的读数；片段: " + repr(d.get("text", "")[:160]))
		# wait_variable 与 read_variables 同门控：真跨帧等一次（首采样即满足），
		# 结论在定界外、读数在定界内、summary 给审计机器可读结论。
		chk("wait_variable" in tools, "--read-var 时 wait_variable 同时在工具表内")
		d = tool_text("wait_variable", {"name": "relay0", "key": "result", "condition": "eq", "value": "OK"})
		chk(d.get("ok") is True and "条件满足" in d.get("text", "") and "slept" in d.get("text", ""),
			"wait_variable 首采样即满足并带回读数；片段: " + repr(d.get("text", "")[:160]))
		chk(d.get("summary", "").startswith("satisfied=true"),
			"wait_variable 的 summary 机器可读；实际: " + repr(d.get("summary")))
		d = tool_text("wait_variable", {"name": "relay0", "key": "result", "condition": "between", "value": 1})
		chk(d.get("ok") is False and d.get("paramError") is True and d.get("executed") is False,
			"wait_variable 非法 condition → paramError（没执行）")
	elif scenario == "readvaroff":
		# 场景 15b 对照：缺省不注册该工具 ⇒「缺省即无此能力」。
		chk("read_variables" not in tools, "缺 --read-var 时 read_variables 不在工具表内")
		chk("wait_variable" not in tools, "缺 --read-var 时 wait_variable 也不在工具表内")
		d = tool_text("read_variables", {})
		chk(d.get("ok") is False and "未知工具" in d.get("text", ""),
			"缺 --read-var 时调用 read_variables → 未知工具")
	elif scenario == "policydeny":
		# 场景 9（拒绝版）：确认程序恒退 1 ⇒ 策略拦截发生在执行**之前**，
		# 被拒的调用一个副作用都不能有。marker 路径由 argv[4] 传入，跑完必须不存在。
		import os
		d = tool_text("run_command", {"command": "touch " + marker, "timeout_seconds": 5})
		chk(d.get("ok") is False and "被策略拦截" in d.get("text", ""),
			"run_command 回执点名被策略拦截；片段: " + repr(d.get("text", "")[:160]))
		chk(d.get("executed") is False, "被策略拦截的调用 executed=false（副作用未发生）")
		chk(not os.path.exists(marker), "被拒的 run_command 零副作用（marker 文件不存在）")
		# 未命中 require_confirm 的工具不受该策略影响（同一后端进程、同一趟）。
		d = tool_text("run_action_script", {"script": "relay-test.mt"})
		chk(d.get("ok") is True, "未命中 require_confirm 的 run_action_script 照常执行")
	# 非法 JSON（帧合法、载荷坏）→ 固定串、不回显、连接不断（可继续后续调用）
	bad = b"not json"
	s.sendall(struct.pack(">I", len(bad)) + bad)
	n = struct.unpack(">I", recv_exact(4))[0]
	rr = json.loads(recv_exact(n).decode("utf-8"))
	chk(rr.get("success") is False and rr.get("error") == "invalid JSON request",
		"非法 JSON → invalid JSON request（不回显）")
	r = call({"_route": "bogus"})
	chk(r.get("success") is False and "unknown route" in r.get("error", ""), "未知路由 → unknown route")
finally:
	s.close()
print("FRAMESUMMARY pass=%d fail=%d" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
PY

# ---- 一趟：起后端 → 校验 perms → 跑帧客户端 → 校验审计/停机 ----
# phase <label> <expect_run_command:true|false> <sockdir> <scenario> [额外后端参数...]
phase(){
	local label="$1" expect="$2" sd="$3" scenario="$4"; shift 4
	local rundir="$sd/run"
	local sock="$rundir/mcp.sock" audit="$sd/audit.jsonl" blog="$sd/backend.log"
	echo "----- $label -----"
	# 只建 $sd 放审计/日志；socket 父目录 $rundir **刻意不建**——由后端以 0700 建，
	# 这才是要验的安全属性（脚本先建会带上 umask，测不到后端的 0700 创建）。
	mkdir -p "$sd"
	"$MYBOT" --robot=robots/relay-test.sql --mcp-backend --mcp-socket "$sock" \
		--scripts-dir scripts --audit-log "$audit" "$@" >"$blog" 2>&1 &
	local bpid=$!; PIDS="$PIDS $bpid"
	local up=no; for _ in $(seq 1 60); do [ -S "$sock" ] && { up=yes; break; }; sleep 0.1; done
	if [ "$up" != yes ]; then ng "$label: 后端未监听（见下）"; sed 's/^/    /' "$blog"; return; fi
	ok "$label: 后端监听 + socket 就绪"
	chk "$label: socket 父目录 0700" "$(stat -c '%a' "$rundir" 2>/dev/null)" "700"
	chk "$label: socket 文件 0600"  "$(stat -c '%a' "$sock" 2>/dev/null)" "600"
	# PR_SET_DUMPABLE=0 的可观测结果：/proc/<pid>/environ 属主变 root、同 UID 读不到。
	if [ "$SAN_BUILD" = yes ]; then
		echo "  [SKIP] $label: 后端不可 dump（sanitizer 构建编译期跳过）"
	else
		chk "$label: 后端不可 dump（/proc/<pid>/environ 属主 root）" "$(stat -c '%U' "/proc/$bpid/environ" 2>/dev/null)" "root"
		if cat "/proc/$bpid/environ" >/dev/null 2>&1; then ng "$label: 同 UID 竟可读后端 environ"; else ok "$label: 同 UID 读后端 environ 被拒"; fi
	fi

	local pyout; pyout="$(python3 "$TMP/frame_client.py" "$sock" "$expect" "$scenario" "$sd/marker-should-not-exist" 2>&1)"
	echo "$pyout"
	local pp pf
	pp="$(echo "$pyout" | sed -n 's/.*FRAMESUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\1/p')"
	pf="$(echo "$pyout" | sed -n 's/.*FRAMESUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\2/p')"
	pass=$((pass + ${pp:-0})); fail=$((fail + ${pf:-0}))

	# 可选：等后端日志出现某一行再往下（断连安全动作在客户端关连接**之后**才跑，
	# 不等就去 kill，脚本会被停机信号打断，测的就不是"断连后动作跑完"了）
	if [ -n "${WAIT_LOG:-}" ]; then
		local seen=no; for _ in $(seq 1 100); do grep -q "$WAIT_LOG" "$blog" && { seen=yes; break; }; sleep 0.1; done
		[ "$seen" = yes ] && ok "$label: 后端日志出现「$WAIT_LOG」" || ng "$label: 后端日志 10s 内未见「$WAIT_LOG」"
	fi

	# 审计：整服务端一 session（start/end）+ 每次工具调用一条（穿过 DispatchGate）
	if [ -f "$audit" ]; then
		grep -q '"kind":"session"' "$audit"      && ok "$label: 审计有 session 记录"      || ng "$label: 审计缺 session 记录"
		grep -q '"tool":"list_hardware"' "$audit" && ok "$label: 审计有 list_hardware 记录" || ng "$label: 审计缺 list_hardware 记录"
		grep -q '"tool":"ghost_tool"' "$audit"    && ok "$label: 审计有 ghost_tool（失败也记）" || ng "$label: 审计缺 ghost_tool 记录"
	else
		ng "$label: 审计文件未生成"
	fi

	kill -TERM "$bpid" 2>/dev/null; wait "$bpid" 2>/dev/null
	grep -q '已停机' "$blog" && ok "$label: 后端收到 SIGTERM 优雅停机" || ng "$label: 后端未见优雅停机日志"
}

phase "缺省（无 --agent-shell）" false "$TMP/p1" base
phase "开 --agent-shell（带强制 policy）" true "$TMP/p2" base --agent-shell --agent-policy "$POLICY"

# ---- 工具层场景（迁自 agent_e2e.sh 场景 14/14b/14c/15/15b，2026-09-24）----
# 每趟一个独立后端进程：apply_sql 的带电改装配到本进程自己的硬件表，不跨趟污染。
# --agent-hot-sql 与 --agent-shell 同受 M5 强制策略约束，故这几趟都带 $POLICY。
phase "--agent-hot-sql 带电生效（INSERT → 清单可见）" false "$TMP/p3" hotsql \
	--agent-hot-sql --agent-policy "$POLICY"
phase "缺 --agent-hot-sql（工具不在表内、零变更）" false "$TMP/p4" hotsqloff
phase "--agent-hot-sql 热删除（INSERT → DELETE → 清单消失）" false "$TMP/p5" hotdelete \
	--agent-hot-sql --agent-policy "$POLICY"
phase "--read-var relay0（回读带驱动返回表的键）" false "$TMP/p6" readvar --read-var relay0
phase "缺 --read-var（工具不在表内）" false "$TMP/p7" readvaroff

# 策略拒绝版（迁自 agent_e2e.sh 场景 9）：确认程序退 1 ⇒ run_command 零副作用。
phase "--agent-policy 拒绝 ⇒ run_command 零副作用" true "$TMP/p8" policydeny \
	--agent-shell --agent-policy "$POLICY_DENY"

# ---- 断连安全动作（2026-09-25，--on-disconnect）：客户端关连接 ⇒ 后端自行跑脚本 ----
# 客户端正常 close 即"连接结束（任一原因）"的一种；relay-test.mt 带 Timer 读数，
# 完成行带字节数。审计落 kind=session/status=disconnect_action，summary 点名脚本。
WAIT_LOG="断连安全动作完成" phase "--on-disconnect relay-test.mt（断连后自行执行）" false "$TMP/p9" base \
	--on-disconnect relay-test.mt
if grep -q '"status":"disconnect_action"' "$TMP/p9/audit.jsonl" 2>/dev/null; then
	ok "断连动作：审计有 disconnect_action 记录"
	grep -q '"status":"disconnect_action".*"summary":"relay-test.mt"' "$TMP/p9/audit.jsonl" \
		&& ok "断连动作：审计 summary 点名脚本" || ng "断连动作：审计 summary 未点名脚本"
	grep '"status":"disconnect_action"' "$TMP/p9/audit.jsonl" | grep -q '"error"' \
		&& ng "断连动作：审计记录带 error（脚本应成功）" || ok "断连动作：审计记录无 error"
else
	ng "断连动作：审计缺 disconnect_action 记录"
fi
grep -q '执行断连安全动作 relay-test.mt' "$TMP/p9/backend.log" && ok "断连动作：日志点名脚本" || ng "断连动作：日志未点名脚本"
# 反面：未配 --on-disconnect 的前几趟不得出现该记录（缺省不执行 = 旧行为）
if grep -q '"status":"disconnect_action"' "$TMP/p1/audit.jsonl" 2>/dev/null; then
	ng "断连动作：未配置时不应有 disconnect_action 记录"
else
	ok "断连动作：未配置时无 disconnect_action 记录"
fi

# ---- 反面（--on-disconnect 预检）：脚本不存在必须拒启（退出 2，上电前）----
echo "----- 反面：--on-disconnect 脚本不存在必须拒启 -----"
REJD="$TMP/reject-disc"; mkdir -p "$REJD"
"$MYBOT" --robot=robots/relay-test.sql --mcp-backend --mcp-socket "$REJD/run/mcp.sock" \
	--scripts-dir scripts --on-disconnect no-such-safe.mt >"$REJD/log" 2>&1 &
rdpid=$!; PIDS="$PIDS $rdpid"
for _ in $(seq 1 40); do kill -0 "$rdpid" 2>/dev/null || break; sleep 0.1; done
wait "$rdpid"; rdrc=$?
chk "反面（断连脚本）：退出码 2" "$rdrc" "2"
if [ -S "$REJD/run/mcp.sock" ]; then ng "反面（断连脚本）：socket 不应被创建"; else ok "反面（断连脚本）：socket 未创建"; fi
grep -q 'on-disconnect' "$REJD/log" && grep -q 'no-such-safe.mt' "$REJD/log" \
	&& ok "反面（断连脚本）：报错点名选项与脚本" || ng "反面（断连脚本）：报错未点名选项与脚本"

# ---- 反面（M5 强制 policy）：--agent-shell 无 --agent-policy 必须拒启（退出 2）----
echo "----- 反面：--agent-shell 无 policy 必须拒启 -----"
REJ="$TMP/reject"; mkdir -p "$REJ"
"$MYBOT" --robot=robots/relay-test.sql --mcp-backend --mcp-socket "$REJ/run/mcp.sock" \
	--scripts-dir scripts --agent-shell >"$REJ/log" 2>&1 &
rpid=$!; PIDS="$PIDS $rpid"
for _ in $(seq 1 40); do kill -0 "$rpid" 2>/dev/null || break; sleep 0.1; done
wait "$rpid"; rrc=$?
chk "反面：退出码 2（配置错误不碰真机）" "$rrc" "2"
if [ -S "$REJ/run/mcp.sock" ]; then ng "反面：socket 不应被创建"; else ok "反面：socket 未创建"; fi
grep -q 'run_command' "$REJ/log" && ok "反面：报错点名 run_command" || ng "反面：报错未点名 run_command"

# ---- 调试逃生口：BOT4CJ_ALLOW_PTRACE=1 ⇒ 保持可 dump，且 stderr 有「注意」----
if [ "$SAN_BUILD" = yes ]; then
	echo "  [SKIP] 逃生口 BOT4CJ_ALLOW_PTRACE（sanitizer 构建不看该变量）"
else
	echo "----- 逃生口：BOT4CJ_ALLOW_PTRACE=1 保持可 dump -----"
	HATCH="$TMP/hatch"; mkdir -p "$HATCH"
	BOT4CJ_ALLOW_PTRACE=1 "$MYBOT" --robot=robots/relay-test.sql --mcp-backend \
		--mcp-socket "$HATCH/run/mcp.sock" --scripts-dir scripts >"$HATCH/log" 2>&1 &
	hpid=$!; PIDS="$PIDS $hpid"
	hup=no; for _ in $(seq 1 60); do [ -S "$HATCH/run/mcp.sock" ] && { hup=yes; break; }; sleep 0.1; done
	if [ "$hup" = yes ]; then
		chk "逃生口：/proc/<pid>/environ 属主为当前用户" "$(stat -c '%U' "/proc/$hpid/environ" 2>/dev/null)" "$(id -un)"
		grep -q 'BOT4CJ_ALLOW_PTRACE=1' "$HATCH/log" && ok "逃生口：stderr 有「注意」行" || ng "逃生口：stderr 缺「注意」行"
	else
		ng "逃生口：后端未监听"; sed 's/^/    /' "$HATCH/log"
	fi
	kill -TERM "$hpid" 2>/dev/null; wait "$hpid" 2>/dev/null
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "MCP 端到端测试通过（$pass 项）"
	exit 0
fi
echo "MCP 端到端测试存在失败（$pass 通过 / $fail 失败）"
exit 1