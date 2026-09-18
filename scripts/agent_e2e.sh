#!/bin/bash
# agent 模式端到端：假 OpenAI 兼容服务 + 真实 mybot 工具循环（无硬件 relay-test）。
#
# 断言的是 agent 模式区别于管道模式的三件事：
#   1) 工具循环真的迭代了（list_hardware → run_action_script → run_command → 最终回答）；
#   2) 动作脚本在**常驻**机器人上真被执行（Relay 转发日志）；
#   3) 非动作类工具（run_command）可用，且最终回答只走 stdout。
#
# 用法: BOT=./build/mybot bash scripts/agent_e2e.sh [port]
# 退出码: 0 通过 / 1 失败 / 125 本构建未启用 agent 模式（缺 libus_http，守门按 SKIP 计）
set -u

BOT="${BOT:-./build/mybot}"
port="${1:-$((20000 + RANDOM % 1000))}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 1

if ! command -v python3 >/dev/null 2>&1; then
	echo "缺少 python3（假模型服务），无法运行 agent 端到端"
	exit 125
fi

python3 scripts/fake_llm_server.py "$port" &
srv=$!
cleanup() {
	kill "$srv" 2>/dev/null
	wait "$srv" 2>/dev/null
}
trap cleanup EXIT

# 等端口就绪（最多 5s）：假服务是本地进程，就绪很快，但不 sleep 会偶发竞态
for _ in $(seq 1 50); do
	if python3 - "$port" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.2)
s.connect(("127.0.0.1", int(sys.argv[1])))
s.close()
PY
	then
		break
	fi
	sleep 0.1
done

out="$(timeout 60 "$BOT" --robot=robots/relay-test.sql --agent \
	--task "先看硬件，再让继电器动作一次，最后跑一条命令" \
	--llm-endpoint "http://127.0.0.1:$port/v1/chat/completions" \
	--llm-key "e2e-fake-key" --scripts-dir scripts 2>&1)"
rc=$?
echo "$out"

if echo "$out" | grep -qF "本构建未启用 agent 模式"; then
	echo "[SKIP] 本构建未启用 agent 模式（缺 libus_http）"
	exit 125
fi

if [ "$rc" -ne 0 ]; then
	echo "agent 端到端失败：mybot 退出码 $rc"
	exit 1
fi

fail=0
check() {
	if echo "$out" | grep -qF "$1"; then
		echo "  [OK] $2"
	else
		echo "  [FAIL] $2（未找到: $1）"
		fail=1
	fi
}
check "AGENT-E2E-完成" "最终回答到达 stdout"
check "relay0" "list_hardware 列出了常驻机器人的硬件变量"
check "Relay: test=200" "run_action_script 真的驱动了动作脚本"
check "agent-echo-ok" "run_command 执行了 shell 命令"
check "已执行动作脚本 relay-test.mt" "动作脚本执行结果回传给了模型"

if [ "$fail" -eq 0 ]; then
	echo "agent 模式端到端测试通过"
	exit 0
fi
exit 1
