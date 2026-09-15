#!/usr/bin/env bash
# scripts/run.sh —— mybot 直跑启动器（CMakeLists 所述「_run.sh 等价物」）
# 用法：cat scripts/test/xxx.mt | bash scripts/run.sh --robot=robots/test/xxx.sql
#       （额外参数原样透传，如 --tcp <port> / --input <path>）
# 缺省定位构建产物 ./build/mybot；可用 BOT=xxx 覆盖（与 scripts/tcp_e2e.sh 同约定）。
set -euo pipefail

BIN="${BOT:-$(dirname "$0")/../build/mybot}"
if [[ ! -x "$BIN" ]]; then
	echo "错误：找不到可执行文件 '$BIN'（先执行：cmake -S . -B build && cmake --build build）" >&2
	exit 2
fi

exec "$BIN" "$@"