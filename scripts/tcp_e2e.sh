#!/usr/bin/env bash
# scripts/tcp_e2e.sh — TCP 长驻动作源端到端测试
#
# 验证 ActionSource.cj 的 --tcp 长驻语义（一次 bind、反复 accept）：
#   1. 非回环对端在读取任何字节前被拒（缺省白名单，且不计入已服务连接数）
#   2. 同一进程连续服务两个回环连接（证明不是一次性连接）
#   3. 达到 --tcp-max 后正常收尾退出（退出码 0）
#   4. 服务端日志无 Aborted / result=ERR
#
# 用法：bash scripts/tcp_e2e.sh [port]
# 依赖：已编译产物（先 bash _set.sh）、python3、_run.sh、robots/relay-test.sql
#
# 就绪探测刻意使用【非回环源地址】的连接：它会被白名单拒掉且不计数，
# 因此探测本身不会消耗掉真正要测的那个 accept。本机无非回环地址时跳过
# 拒绝子项、改为固定等待（此时若用回环探测会占掉一个 accept，故不做）。

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT="${1:-19240}"
SCRIPT="scripts/relay-test.mt"
ROBOT="robots/relay-test.sql"
LOG="$(mktemp -t bot4cj_tcp_e2e.XXXXXX)"
SRV_PID=""
trap 'rm -f "$LOG"; [ -n "$SRV_PID" ] && kill -9 "$SRV_PID" 2>/dev/null' EXIT

if [ ! -x "target/release/bin/main" ]; then
	echo "错误：无编译产物 target/release/bin/main（先执行 bash _set.sh）"
	exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "错误：需要 python3 作为 TCP 客户端"
	exit 1
fi

LAN="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -n "$LAN" ] || echo "提示：本机无非回环地址，跳过拒绝子项"

echo "=== TCP 长驻端到端：端口 ${PORT}，上限 2 连接 ==="
nohup bash _run.sh --robot="$ROBOT" --tcp "$PORT" --tcp-max 2 > "$LOG" 2>&1 &
SRV_PID=$!

python3 - "$PORT" "$LAN" "$SCRIPT" <<'PYEOF'
import socket, sys, time
port, lan, script = int(sys.argv[1]), sys.argv[2], sys.argv[3]
data = open(script, "rb").read()

def send(host, bind_addr):
    s = socket.socket()
    s.settimeout(5)
    if bind_addr:
        s.bind((bind_addr, 0))
    s.connect((host, port))
    s.sendall(data)
    s.shutdown(socket.SHUT_WR)
    s.close()

# 1) 就绪探测 = 非回环拒绝用例（有局域网地址时），连不上则 0.5s 重试
if lan:
    for _ in range(20):
        try:
            send(lan, lan)
            print(f"[client] 非回环源 {lan} 已发送（预期被服务端拒绝）")
            break
        except OSError:
            time.sleep(0.5)
    else:
        print("[client] 非回环探测始终连不上服务端", file=sys.stderr)
        sys.exit(1)
else:
    time.sleep(2)

# 2) 两个回环连接，须由同一进程依次服务
for i in (1, 2):
    for _ in range(20):
        try:
            send("127.0.0.1", None)
            print(f"[client] 回环连接 {i} 已发送 {len(data)} 字节")
            break
        except OSError:
            time.sleep(0.5)
    else:
        print(f"[client] 回环连接 {i} 连接失败", file=sys.stderr)
        sys.exit(1)
PYEOF
if [ $? -ne 0 ]; then
	echo "✗ 客户端阶段失败"; cat "$LOG"; exit 1
fi

# 3) 服务满 2 个连接后服务端应自行收尾退出
for _ in $(seq 1 15); do
	kill -0 "$SRV_PID" 2>/dev/null || break
	sleep 1
done
if kill -0 "$SRV_PID" 2>/dev/null; then
	echo "✗ 服务端 15s 内未退出（--tcp-max 未生效？）"; cat "$LOG"; exit 1
fi
wait "$SRV_PID" 2>/dev/null; SRV_RC=$?
SRV_PID=""

echo "--- 服务端日志 ---"
cat "$LOG"
echo "------------------"

rc=0
[ "$SRV_RC" -eq 0 ] || { echo "✗ 服务端退出码 $SRV_RC（预期 0）"; rc=1; }
served="$(grep -cF '已服务 ' "$LOG")"
[ "$served" -eq 2 ] || { echo "✗ 已服务连接数 $served（预期 2）——长驻未生效？"; rc=1; }
conns="$(grep -cF 'TCP 客户端已连接' "$LOG")"
[ "$conns" -eq 2 ] || { echo "✗ 接受的连接数 $conns（预期 2）"; rc=1; }
if [ -n "$LAN" ]; then
	grep -qF "拒绝非回环对端" "$LOG" || { echo "✗ 未见非回环拒绝记录（白名单失效？）"; rc=1; }
fi
grep -qE "Aborted|\"result\":\"ERR\"" "$LOG" && { echo "✗ 服务端执行出错"; rc=1; }

[ $rc -eq 0 ] && echo "=== TCP 长驻端到端通过 ===" || echo "=== TCP 长驻端到端失败 ==="
exit $rc
