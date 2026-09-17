#!/bin/bash
# bot4cj 双跑守门（F-23）：Release 全量 + sanitizer 全量，两跑都过才算过。
#
# 前置（两套构建都要先建好；sanitizer 开关随仓库分发，见 CMakeLists.txt）：
#   cmake -S . -B build && cmake --build build
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
#     -DMYBOT_SANITIZE=address,undefined && cmake --build build-asan
#
# 用法：bash test/run_san.sh [过滤器]
#   BOT/SAN_BOT（BOT_TESTS/SAN_BOT_TESTS）可覆盖两跑的二进制路径。
cd "$(dirname "$0")/.." || exit 1

echo "===== 第一跑：Release（${BOT:-./build/mybot}） ====="
BOT="${BOT:-./build/mybot}" BOT_TESTS="${BOT_TESTS:-./build/mybot_tests}" \
	bash test/run_all.sh "$@" || exit 1

echo ""
echo "===== 第二跑：sanitizer（${SAN_BOT:-./build-asan/mybot}） ====="
BOT="${SAN_BOT:-./build-asan/mybot}" BOT_TESTS="${SAN_BOT_TESTS:-./build-asan/mybot_tests}" \
	bash test/run_all.sh "$@" || exit 1

echo ""
echo "双跑守门通过：Release + sanitizer 两跑均全绿"
