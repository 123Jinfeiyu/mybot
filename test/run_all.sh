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
# 守护线程→共享槽→Ema→Monitor 全链路，I2C/SPI 器件均为内存仿真模型。
#
# 用例表：用例名|raw 快照浮点断言 ERE（证明 :f 字段未被取整）|count 下限。
# EMA 混合断言对所有用例通用：某拍 monf 行内 out ≠ raw（滤波确实推进）。
# ======================================================================
bin="target/release/bin/main"
e2e_cases=(
	'mpu_virt|"v6":41\.[0-9]+|10'
	'bme280_virt|"v0":2[0-9]\.[0-9]+|10'
	'ads1256_virt|"v1":[0-2]\.[0-9]+|10'
	'vl53l0x_virt|"v0":[0-9]+\.[0-9]+|10'
	'vl53l4cd_virt|"v0":[0-9]+\.[0-9]+|10'
)

run_one_e2e() {
	local name="$1" float_ere="$2" count_min="$3"
	echo "----- $name (仓颉 E2E) -----"
	if [ ! -x "$bin" ]; then
		echo "[SKIP] 无编译产物 $bin（执行 bash _set.sh 后可跑无硬件端到端）"
		return 2
	fi
	# 动作时长约 1s，给 30s 上限防挂死；_run.sh 退出码原样透传
	if [ -n "$timeout_cmd" ]; then
		out="$(cat "scripts/test/$name.mt" | timeout 30 bash _run.sh --robot="robots/test/$name.sql" 2>&1)"
	else
		out="$(cat "scripts/test/$name.mt" | bash _run.sh --robot="robots/test/$name.sql" 2>&1)"
	fi
	local rc=$?
	echo "$out"
	local e2e_ok=true
	[ $rc -ne 0 ] && e2e_ok=false
	echo "$out" | grep -qF "VIRTUAL HARDWARE" || e2e_ok=false
	echo "$out" | grep -qE "chunk setup error|chunk load error|setup failed|tick error|cleanup error|解析失败" \
		&& e2e_ok=false
	echo "$out" | grep -qF "joined, total publishes=" || e2e_ok=false
	# 守护持续发布：出现 ≥2 个不同 count 且最大值达标
	local count_max count_uniq
	count_max="$(echo "$out" | grep -oE '"count":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)"
	count_uniq="$(echo "$out" | grep -oE '"count":[0-9]+' | sort -u | wc -l)"
	{ [ -z "$count_max" ] || [ "$count_max" -lt "$count_min" ] || [ "$count_uniq" -lt 2 ]; } && e2e_ok=false
	# raw 快照里 :f 浮点字段保持小数（未被 sign=16 取整补码）
	echo "$out" | grep -qE "$float_ere" || e2e_ok=false
	# EMA 已推进：至少一行 monf 快照 out ≠ raw（首拍种子相同，后续 α 混合必不同）。
	# JSON 键序不固定，逐行各自提取 out/raw 数值做比对。
	local mixed_n=0 o r
	while IFS= read -r line; do
		o="$(printf '%s' "$line" | grep -oE '"out":-?[0-9]+(\.[0-9]+)?' | head -1 | cut -d: -f2)"
		r="$(printf '%s' "$line" | grep -oE '"raw":-?[0-9]+(\.[0-9]+)?' | head -1 | cut -d: -f2)"
		if [ -n "$o" ] && [ -n "$r" ] && [ "$o" != "$r" ]; then
			mixed_n=$((mixed_n + 1))
		fi
	done < <(printf '%s' "$out" | grep -F '"raw"')
	[ "$mixed_n" -ge 1 ] || e2e_ok=false
	if $e2e_ok; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		failed="$failed $name(E2E)"
	fi
}

if ! command -v cjc >/dev/null 2>&1 && [ -z "$CANGJIE_HOME" ] && [ ! -d "$HOME/cangjie" ]; then
	for spec in "${e2e_cases[@]}"; do
		name="${spec%%|*}"
		if [ -z "$filter" ] || [[ "$name" == *"$filter"* ]]; then
			echo "----- $name (仓颉 E2E) -----"
			echo "[SKIP] 无仓颉 runtime（需 cjc 在 PATH、设置 CANGJIE_HOME，或存在 ~/cangjie）"
		fi
	done
else
	[ -z "$CANGJIE_HOME" ] && [ -d "$HOME/cangjie" ] && export CANGJIE_HOME="$HOME/cangjie"
	for spec in "${e2e_cases[@]}"; do
		name="${spec%%|*}"
		rest="${spec#*|}"
		float_ere="${rest%%|*}"
		count_min="${rest##*|}"
		if [ -n "$filter" ] && [[ "$name" != *"$filter"* ]]; then
			continue
		fi
		run_one_e2e "$name" "$float_ere" "$count_min"
	done
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
