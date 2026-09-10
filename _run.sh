#!/bin/bash
cd "$(dirname "$0")" || exit
export cjStackSize=64MB
export cjHeapSize=64MB

# M3: cjpm run 恒返回 0，会吞掉程序 abort 的退出码，故直跑产物并把退出码原样透传。
base=$(cd "$(dirname "$0")" && pwd)
ldp=$(find "$base/target/release" -maxdepth 2 -name "*.so" -printf "%h:" 2>/dev/null | sed "s/:$//")
if [ -n "$CANGJIE_HOME" ]; then
	rt="$CANGJIE_HOME/runtime/lib/linux_x86_64_cjnative"
else
	rt="$(dirname "$(dirname "$(command -v cjc)")")/runtime/lib/linux_x86_64_cjnative"
fi
export LD_LIBRARY_PATH="$ldp:$rt"
exec "$base/target/release/bin/main" "$@"