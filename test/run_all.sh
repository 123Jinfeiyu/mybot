#!/bin/bash
# bot4cj Lua 离线测试统一入口
# 用法: bash test/run_all.sh [测试名过滤]
#   例: bash test/run_all.sh                # 跑全部
#       bash test/run_all.sh pca9685       # 只跑文件名含 pca9685 的
# 说明：
#   - 统一用 luajit 运行（HAL 依赖 ffi）；可用环境变量 LUACMD 覆盖解释器
#   - 单测超时 TEST_TIMEOUT 秒（默认 60，需 coreutils timeout；缺失则不限时）——
#     防单个用例死循环挂死整个入口
#   - test_jit_warmup.lua 是真机 ABI 回归测试（需 /dev/i2c-*），无总线环境自动跳过
#   - test_ioctl_nums.c 为 C 编译期常量自检，不在本入口范围
cd "$(dirname "$0")/.." || exit 1

LUACMD="${LUACMD:-luajit}"
if ! command -v "$LUACMD" >/dev/null 2>&1; then
	echo "未找到解释器：$LUACMD"
	echo "HAL 依赖 ffi，请安装 LuaJIT：sudo apt install luajit"
	echo "（或 LUACMD=lua5.4 只跑纯 Lua 用例，HAL 相关用例会失败）"
	exit 1
fi

TEST_TIMEOUT="${TEST_TIMEOUT:-60}"
timeout_cmd=""
if command -v timeout >/dev/null 2>&1; then
	timeout_cmd="timeout $TEST_TIMEOUT"
fi

filter="${1:-}"
pass=0
fail=0
failed=""

for t in test/test_*.lua; do
	name="$(basename "$t")"
	if [ -n "$filter" ] && [[ "$name" != *"$filter"* ]]; then
		continue
	fi
	# 真机测试：文件内声明 "-- requires: i2c" 且本机无 /dev/i2c-* 时跳过
	if grep -q '^\-\- requires: i2c' "$t" && ! ls /dev/i2c-* >/dev/null 2>&1; then
		echo "----- $name -----"
		echo "[SKIP] 需要 /dev/i2c-* 真机总线（文件标记 -- requires: i2c），本机无 i2c，跳过"
		continue
	fi
	echo "----- $name -----"
	out="$($timeout_cmd "$LUACMD" "$t" 2>&1)"
	rc=$?
	echo "$out"
	# 兼容两种失败约定：退出码非零，或打印 [FAIL] 行（124 = timeout 超时）
	if [ $rc -eq 124 ] && [ -n "$timeout_cmd" ]; then
		echo "[FAIL] 用例超时（>${TEST_TIMEOUT}s，疑似死循环）"
		fail=$((fail + 1))
		failed="$failed $name"
	elif [ $rc -ne 0 ] || echo "$out" | grep -qF "[FAIL]"; then
		fail=$((fail + 1))
		failed="$failed $name"
	else
		pass=$((pass + 1))
	fi
done

echo "==============================================="

# ======================================================================
# 仓颉级无硬件端到端：编译产物 + 仓颉 runtime 齐备才跑，缺失则跳过
# （纯 Lua 环境/CI 无 cjc 时不阻断回归）。用例走真实 bot4cj 二进制：
# 守护线程→共享槽→Ema→Monitor 全链路，仅 I2C 器件为内存仿真模型。
# ======================================================================
bin="target/release/bin/main"
if [ -n "$filter" ] && [[ "mpu_virt" != *"$filter"* ]]; then
	:
elif [ ! -x "$bin" ]; then
	echo "----- mpu_virt (仓颉 E2E) -----"
	echo "[SKIP] 无编译产物 $bin（执行 bash _set.sh 后可跑无硬件端到端）"
elif ! command -v cjc >/dev/null 2>&1 && [ -z "$CANGJIE_HOME" ] && [ ! -d "$HOME/cangjie" ]; then
	echo "----- mpu_virt (仓颉 E2E) -----"
	echo "[SKIP] 无仓颉 runtime（需 cjc 在 PATH、设置 CANGJIE_HOME，或存在 ~/cangjie）"
else
	echo "----- mpu_virt (仓颉 E2E) -----"
	[ -z "$CANGJIE_HOME" ] && [ -d "$HOME/cangjie" ] && export CANGJIE_HOME="$HOME/cangjie"
	# 21 行动作约 1.2s，给 30s 上限防挂死；_run.sh 退出码原样透传
	if [ -n "$timeout_cmd" ]; then
		out="$(cat scripts/test/mpu_virt.mt | timeout 30 bash _run.sh --robot=robots/test/mpu_virt.sql 2>&1)"
	else
		out="$(cat scripts/test/mpu_virt.mt | bash _run.sh --robot=robots/test/mpu_virt.sql 2>&1)"
	fi
	rc=$?
	echo "$out"
	e2e_ok=true
	[ $rc -ne 0 ] && e2e_ok=false
	echo "$out" | grep -qF "VIRTUAL HARDWARE" || e2e_ok=false
	echo "$out" | grep -qE "chunk setup error|chunk load error|setup failed|tick error|cleanup error" \
		&& e2e_ok=false
	echo "$out" | grep -qF "joined, total publishes=" || e2e_ok=false
	# 守护持续发布：出现 ≥2 个不同 count 且最大 ≥10（21 拍消费、守护 10ms/拍）
	count_max="$(echo "$out" | grep -oE '"count":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)"
	count_uniq="$(echo "$out" | grep -oE '"count":[0-9]+' | sort -u | wc -l)"
	{ [ -z "$count_max" ] || [ "$count_max" -lt 10 ] || [ "$count_uniq" -lt 2 ]; } && e2e_ok=false
	# 七字段快照：温度路 v6 为 41.x°C 浮点（:f 透传，未被 sign=16 取整）
	echo "$out" | grep -qE '"v6":41\.[0-9]+' || e2e_ok=false
	# EMA 已推进且在平滑：某拍同事务内 v0 为带小数值、raw 为整数（α=0.2 混合后必非整数）
	echo "$out" | grep -qE '"v0":-?[0-9]+\.[0-9]+.*"raw":-?[0-9]+[,}]|"raw":-?[0-9]+[,}].*"v0":-?[0-9]+\.[0-9]+' \
		|| e2e_ok=false
	if $e2e_ok; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		failed="$failed mpu_virt(E2E)"
	fi
fi

# 空匹配防线：过滤器拼错时 0 通过 0 失败不得假绿
if [ $((pass + fail)) -eq 0 ]; then
	echo "汇总: 无匹配用例（过滤器 '${filter}' 未命中任何 test/test_*.lua 或仓颉 E2E，请检查拼写）"
	exit 1
fi
echo "汇总: $pass 通过, $fail 失败"
if [ $fail -ne 0 ]; then
	echo "失败用例:$failed"
	exit 1
fi
