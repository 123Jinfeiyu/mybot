#!/bin/bash
cd "$(dirname "$0")" || exit
export cjStackSize=64MB
export cjHeapSize=64MB

# M3: cjpm run 恒返回 0，会吞掉程序 abort 的退出码，故直跑产物并把退出码原样透传。
# 运行时库路径按当前架构选择（x86_64 / aarch64），与 cjpm.toml 的 target 声明一致
base=$(cd "$(dirname "$0")" && pwd)
ldp=$(find "$base/target/release" -maxdepth 2 -name "*.so" -printf "%h:" 2>/dev/null | sed "s/:$//")
crjarch="x86_64"
case "$(uname -m)" in
	aarch64|arm64) crjarch="aarch64" ;;
esac
if [ -n "$CANGJIE_HOME" ]; then
	rt="$CANGJIE_HOME/runtime/lib/linux_${crjarch}_cjnative"
else
	rt="$(dirname "$(dirname "$(command -v cjc)")")/runtime/lib/linux_${crjarch}_cjnative"
fi
export LD_LIBRARY_PATH="$ldp:$rt"
exec "$base/target/release/bin/main" "$@"