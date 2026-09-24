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

# ---- 帧客户端（Python 纯标准库）：直连后端 AF_UNIX，独立实现线协议 ----
# 用法: frame_client.py <sock> <expect_run_command:true|false>
cat > "$TMP/frame_client.py" <<'PY'
import socket, struct, json, sys
sock, expect_rc = sys.argv[1], sys.argv[2] == "true"
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
	if expect_rc:
		# fd 卫生（评估 §3.5）：run_command 走 /bin/sh -c，子 shell **不得**继承后端的
		# listening/accepted socket fd（后端用 SOCK_CLOEXEC 创建，另有 markAllFdsCloexec
		# 兜底）。数一下 sh 自己的 socket fd，应为 0；策略 confirm_command=/bin/true
		# 自动放行这次调用。泄漏时这里会出现 SOCKFDS>=1。
		fdcmd = "echo SOCKFDS=$(ls -l /proc/$$/fd 2>/dev/null | grep -c socket:)"
		r = call({"_route": "tool.call", "name": "run_command",
			"arguments": {"command": fdcmd, "timeout_seconds": 5}})
		d = r.get("data", {})
		chk("SOCKFDS=0" in d.get("text", ""),
			"run_command 子进程未继承后端 socket fd（SOCKFDS=0）；回执片段: " + repr(d.get("text", "")[:160]))
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
# phase <label> <expect_run_command:true|false> <sockdir> [额外后端参数...]
phase(){
	local label="$1" expect="$2" sd="$3"; shift 3
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

	local pyout; pyout="$(python3 "$TMP/frame_client.py" "$sock" "$expect" 2>&1)"
	echo "$pyout"
	local pp pf
	pp="$(echo "$pyout" | sed -n 's/.*FRAMESUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\1/p')"
	pf="$(echo "$pyout" | sed -n 's/.*FRAMESUMMARY pass=\([0-9]*\) fail=\([0-9]*\)/\2/p')"
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

echo
if [ "$fail" -eq 0 ]; then
	echo "MCP 端到端测试通过（$pass 项）"
	exit 0
fi
echo "MCP 端到端测试存在失败（$pass 通过 / $fail 失败）"
exit 1