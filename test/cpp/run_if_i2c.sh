#!/bin/sh
# 真机用例包装（与 run_all.sh 同语义）：文件声明 "-- requires: i2c"
# 且本机无 /dev/i2c-* 时以 CTest 的 skipped 状态退出（"does not require testing"），
# 有真机总线则正常执行 LuaJIT 用例。
set -e
LUACMD="$1"; shift
if ! ls /dev/i2c-* >/dev/null 2>&1; then
	echo "[SKIP] 需要 /dev/i2c-* 真机总线（文件标记 -- requires: i2c），本机无 i2c"
	exit 125
fi
exec "$LUACMD" "$@"
