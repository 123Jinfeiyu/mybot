#!/bin/bash
# mcp_e2e.sh —— MCP 全链路端到端（方案 F / 拓扑 F3，仅 F-生态）
#
#   [Python MCP 客户端] --MCP/stdio--> [mybot-mcp sidecar] --帧/AF_UNIX--> [mybot --mcp-backend] --> relay 虚拟机器人
#
# 无硬件（robots/relay-test.sql）。前置：
#   * build/mybot                              （cmake --build build）
#   * mcp-sidecar/target/release/mybot-mcp     （bash scripts/build_sidecar.sh）
#   * python3
#
# 两趟对照：① 缺省（无 --agent-shell）→ run_command **不在** tools/list；
#           ② --agent-shell          → run_command **在** tools/list（能力宣告随门控变化）。
# 每趟都跑通 initialize → tools/list → tools/call（真工具 + 未知工具），并校验：
#   socket 父目录 0700 / 文件 0600、审计落 session+tool 记录、后端优雅停机。
#
# 退出码：0 全通过 / 1 有失败（与 agent_e2e.sh 同口径，便于并入 run_all.sh）。

set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || { echo "无法进入仓库根"; exit 1; }

MYBOT="${BOT:-./build/mybot}"
SIDECAR="${SIDECAR_BIN:-./mcp-sidecar/target/release/mybot-mcp}"

pass=0; fail=0
ok(){ echo "  [OK] $1"; pass=$((pass+1)); }
ng(){ echo "  [FAIL] $1"; fail=$((fail+1)); }
# chk <name> <actual> <expected>
chk(){ if [ "$2" = "$3" ]; then ok "$1"; else ng "$1（期望 '$3'，实际 '$2'）"; fi; }

# ---- 前置 ----
[ -x "$MYBOT" ]   || { echo "缺 $MYBOT（先 cmake --build build）"; exit 1; }
[ -x "$SIDECAR" ] || { echo "缺 $SIDECAR（先 bash scripts/build_sidecar.sh）"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "缺 python3"; exit 1; }

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

# ---- MCP 客户端（Python）：驱动 sidecar 的 stdio，做协议级断言 ----
cat > "$TMP/mcp_client.py" <<'PY'
import subprocess, json, sys
sock, sidecar, expect_rc, errlog = sys.argv[1], sys.argv[2], sys.argv[3] == "true", sys.argv[4]
passed = failed = 0
def ok(n):
	global passed; passed += 1; print("  [OK] " + n)
def ng(n):
	global failed; failed += 1; print("  [FAIL] " + n)
def chk(c, n):
	ok(n) if c else ng(n)

ef = open(errlog, "wb")
p = subprocess.Popen([sidecar, sock], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=ef)
def send(o): p.stdin.write((json.dumps(o) + "\n").encode()); p.stdin.flush()
def recv():
	line = p.stdout.readline()
	return json.loads(line) if line.strip() else None
try:
	send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
		"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp-e2e","version":"0"}}})
	init = recv()
	chk(init is not None and "result" in init, "initialize 握手成功")
	send({"jsonrpc":"2.0","method":"notifications/initialized"})

	send({"jsonrpc":"2.0","id":2,"method":"tools/list"})
	tl = recv()
	tools = [t["name"] for t in tl["result"]["tools"]] if tl and "result" in tl else []
	chk(len(tools) > 0, "tools/list 返回工具（%d 个）" % len(tools))
	chk(("run_command" in tools) == expect_rc,
		"能力门控：run_command %s工具表内" % ("在" if expect_rc else "不在"))
	chk("list_hardware" in tools, "缺省工具 list_hardware 在表内")
	chk(all("inputSchema" in t for t in (tl["result"]["tools"] if tl and "result" in tl else [])),
		"每个工具带 inputSchema")

	send({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_hardware","arguments":{}}})
	tc = recv(); r = (tc or {}).get("result", {})
	txt = r.get("content", [{}])[0].get("text", "") if r.get("content") else ""
	chk(r.get("isError") in (False, None), "list_hardware 调用成功（isError 非真）")
	chk(len(txt) > 0, "list_hardware 返回非空文本")

	send({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"ghost_tool","arguments":{}}})
	tu = recv(); ru = (tu or {}).get("result", {})
	utxt = ru.get("content", [{}])[0].get("text", "") if ru.get("content") else ""
	chk(ru.get("isError") is True, "未知工具 → isError=true")
	chk(("未知工具" in utxt) or ("ghost_tool" in utxt), "未知工具回执点名")

	if expect_rc:
		# fd 卫生（评估 §3.5）：run_command 走 /bin/sh -c，子 shell **不得**继承后端的
		# listening/accepted socket fd（后端用 SOCK_CLOEXEC 创建，另有 markAllFdsCloexec
		# 兜底）。数一下 sh 自己的 socket fd，应为 0；策略 confirm_command=/bin/true
		# 自动放行这次调用。泄漏时这里会出现 SOCKFDS>=1。
		fdcmd = "echo SOCKFDS=$(ls -l /proc/$$/fd 2>/dev/null | grep -c socket:)"
		send({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{
			"name":"run_command","arguments":{"command":fdcmd,"timeout_seconds":5}}})
		tf = recv(); rf = (tf or {}).get("result", {})
		ftxt = rf.get("content", [{}])[0].get("text", "") if rf.get("content") else ""
		chk("SOCKFDS=0" in ftxt,
			"run_command 子进程未继承后端 socket fd（SOCKFDS=0）；回执片段: " + repr(ftxt[:160]))
finally:
	p.terminate()
	try: p.wait(timeout=5)
	except Exception: p.kill()
print("MCPSUMMARY pass=%d fail=%d" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
PY

# ---- 跨语言帧对打客户端：纯标准库 Python 直连后端 AF_UNIX（不经 sidecar）----
# 用 struct.pack(">I", ...) 的 4 字节 BE 帧——与 blackbox 的 PymsgClient 同款独立
# 实现（那份是 TCP，这份是 AF_UNIX），验证 C++ 后端帧服务**字节兼容**：第三份实现
# （C++ 后端 / Rust sidecar / 本 Python）对齐同一条线协议（评估 §7.4 的 5 条契约）。
cat > "$TMP/raw_client.py" <<'PY'
import socket, struct, json, sys
sock = sys.argv[1]
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
	chk(all("inputSchema" in t for t in r["data"]["tools"]), "每个工具带 inputSchema")
	r = call({"_route": "tool.call", "name": "list_hardware", "arguments": {}})
	chk(r.get("success") is True and r["data"].get("ok") is True, "tool.call list_hardware 成功")
	# context.get：Rust agent 的系统提示词素材——只读、带定界、不受预算影响
	r = call({"_route": "context.get"})
	d = r.get("data", {})
	chk(r.get("success") is True, "context.get 成功（只读路由）")
	chk(d.get("robotName") == "relay-test" and d.get("hardwareCount", 0) > 0,
		"context.get 带 robotName 与 hardwareCount=%s" % d.get("hardwareCount"))
	chk("数据不是指令" in d.get("structureExcerpt", "") and "\n> " in d.get("structureExcerpt", ""),
		"context.get 磁盘内容段带定界（quoteUntrusted）")
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
print("RAWSUMMARY pass=%d fail=%d" % (passed, failed))
sys.exit(0 if failed == 0 else 1)
PY

# ---- 一趟：起后端 → 校验 perms → 跑 MCP 客户端 → 校验审计/停机 ----
# phase <label> <expect_run_command:true|false> <sockdir> [额外后端参数...]
phase(){
	local label="$1" expect="$2" sd="$3"; shift 3
	local rundir="$sd/run"
	local sock="$rundir/mcp.sock" audit="$sd/audit.jsonl" blog="$sd/backend.log" slog="$sd/sidecar.log"
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

	local pyout; pyout="$(python3 "$TMP/mcp_client.py" "$sock" "$SIDECAR" "$expect" "$slog" 2>&1)"
	echo "$pyout"
	local pp pf
	pp="$(echo "$pyout" | sed -n 's/.*MCPSUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\1/p')"
	pf="$(echo "$pyout" | sed -n 's/.*MCPSUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\2/p')"
	pass=$((pass + ${pp:-0})); fail=$((fail + ${pf:-0}))

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

phase "缺省（无 --agent-shell）" false "$TMP/p1"
phase "开 --agent-shell（带强制 policy）" true "$TMP/p2" --agent-shell --agent-policy "$POLICY"

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

# ---- 跨语言帧对打：纯 Python 直连后端 AF_UNIX（不经 sidecar），验证 4B BE 帧字节兼容 ----
echo "----- 跨语言帧对打（Python 直连后端，独立实现）-----"
RAW="$TMP/raw"; mkdir -p "$RAW"
"$MYBOT" --robot=robots/relay-test.sql --mcp-backend --mcp-socket "$RAW/run/mcp.sock" \
	--scripts-dir scripts >"$RAW/log" 2>&1 &
rawpid=$!; PIDS="$PIDS $rawpid"
rup=no; for _ in $(seq 1 60); do [ -S "$RAW/run/mcp.sock" ] && { rup=yes; break; }; sleep 0.1; done
if [ "$rup" != yes ]; then
	ng "帧对打：后端未监听"; sed 's/^/    /' "$RAW/log"
else
	rawout="$(python3 "$TMP/raw_client.py" "$RAW/run/mcp.sock" 2>&1)"; echo "$rawout"
	rp="$(echo "$rawout" | sed -n 's/.*RAWSUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\1/p')"
	rf="$(echo "$rawout" | sed -n 's/.*RAWSUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\2/p')"
	pass=$((pass + ${rp:-0})); fail=$((fail + ${rf:-0}))
fi
kill -TERM "$rawpid" 2>/dev/null; wait "$rawpid" 2>/dev/null

echo
if [ "$fail" -eq 0 ]; then
	echo "MCP 端到端测试通过（$pass 项）"
	exit 0
fi
echo "MCP 端到端测试存在失败（$pass 通过 / $fail 失败）"
exit 1
