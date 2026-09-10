#!/bin/bash
cd "$(dirname "$0")" || exit
export cjStackSize=64MB
export cjHeapSize=128MB

# M3: cjpm run 恒返回 0，会吞掉程序 abort 的退出码，故直跑产物并把退出码原样透传。
# 运行时库路径按当前架构选择（x86_64 / aarch64），与 cjpm.toml 的 target 声明一致
base=$(cd "$(dirname "$0")" && pwd)
# POSIX 可移植写法（find -printf 为 GNU 扩展）：取 .so 所在目录去重后以 : 连接
ldp=$(find "$base/target/release" -maxdepth 2 -name "*.so" 2>/dev/null | while IFS= read -r so; do dirname "$so"; done | sort -u | paste -sd: -)
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

bin="$base/target/release/bin/main"
if [ ! -x "$bin" ]; then
	echo "未找到编译产物 target/release/bin/main，请先执行 bash _set.sh 构建" >&2
	exit 1
fi
# 源码新于产物：提醒可能跑的是过期二进制（行为与文档/最新修复不一致的常见原因）
stale=$(find "$base/src" -name "*.cj" -newer "$bin" -print -quit 2>/dev/null)
if [ -n "$stale" ]; then
	echo "警告: src/ 源码比编译产物新（${stale#$base/}），当前运行的可能是过期二进制；建议先执行 bash _set.sh" >&2
fi
exec "$bin" "$@"
