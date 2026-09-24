#!/usr/bin/env bash
# 构建 mybot-mcp sidecar（方案 F / 拓扑 F3，仅 F-生态）。
# 刻意不碰 CMake：sidecar 是独立 cargo 产物（评估 §5.3）。target/ 已在 .gitignore。
#
# 用法：bash scripts/build_sidecar.sh [--release|--tests|...]
#   缺省构建 release 二进制到 mcp-sidecar/target/release/mybot-mcp
set -euo pipefail

# cargo/rustc 装在 ~/.cargo/bin（rustup），非交互 shell 的 PATH 里没有它。
export PATH="$HOME/.cargo/bin:$PATH"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here/../mcp-sidecar"

if [ "$#" -eq 0 ]; then
  cargo build --release
else
  cargo build "$@"
fi

echo "产物: $(pwd)/target/release/mybot-mcp"
