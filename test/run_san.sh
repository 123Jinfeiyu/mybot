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
# LSan 抑制：agent 模式加载 Lua 协议驱动时会经 ffi 拉起 libcurl → libcrypto，
# 后者留下一批进程级初始化残留（栈全在 CRYPTO_malloc 内，与 mybot 自身分配无关）。
# 必须走 LSAN_OPTIONS：ASan 的抑制解析器不认识 `leak:` 类型，放在 ASAN_OPTIONS 里
# 会直接报 "failed to parse suppressions" 并让每个用例都失败。
# 用 ':' 追加而非覆盖，保留调用方可能已设置的其他选项。理由见仓库根 asan.supp。
export LSAN_OPTIONS="suppressions=$(pwd)/asan.supp${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
BOT="${SAN_BOT:-./build-asan/mybot}" BOT_TESTS="${SAN_BOT_TESTS:-./build-asan/mybot_tests}" \
	bash test/run_all.sh "$@" || exit 1

echo ""
echo "双跑守门通过：Release + sanitizer 两跑均全绿"
