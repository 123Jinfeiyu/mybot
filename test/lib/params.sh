# test/lib/params.sh —— 守门常量单点（F3-5 余部②③）
#
# 此前两处常量各有多份手抄：sanitizer 构建目录（run_san.sh ×2 ↔ .gitlab-ci.yml
# 的 gate:sanitizer）、单测超时（run_all.sh ↔ CMakeLists ctest TIMEOUT ↔
# docs/testing.md）。改一处忘一处就静默漂移。
#
# 格式约定：bash 环境优先默认 `: "${KEY:=v}"`——bash 侧直接 source（调用方
# 环境已设的值保留，params 只补默认）；CMake 侧 file(STRINGS) + 正则提取
# `:=` 之后的默认值。新增常量时三处消费方（bash / CMake / CI）同步，
# 并入 run_all_selftest.sh 的单点校验。

# Lua/C++ 单测超时秒数（run_all.sh 的 timeout 前缀；ctest TIMEOUT 同源）
: "${TEST_TIMEOUT:=60}"

# sanitizer 构建目录（run_san.sh 第二跑产物路径；gate:sanitizer 构建路径同源）
: "${SAN_DIR:=build-asan}"
