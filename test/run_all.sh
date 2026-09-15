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
# mybot（C++ 移植）无硬件端到端：编译产物齐备才跑，缺失则跳过。
# 用例走真实 mybot 二进制：守护线程→共享槽→Ema→Monitor 全链路，
# I2C/SPI 器件均为内存仿真模型（modules/lib/virtbus/）。
#
# 用例表：用例名|raw 快照浮点断言 ERE（证明 :f 字段未被取整）|count 下限。
# EMA 混合断言对所有用例通用：某拍 monf 行内 out ≠ raw（滤波确实推进）。
# 二进制缺省 ./build/mybot，可用 BOT=xxx 覆盖。
# ======================================================================
bin="${BOT:-./build/mybot}"
e2e_cases=(
	'mpu_virt|"v6":41\.[0-9]+|10'
	'bme280_virt|"v0":2[0-9]\.[0-9]+|10'
	'ads1256_virt|"v1":[0-2]\.[0-9]+|10'
	'vl53l0x_virt|"v0":[0-9]+\.[0-9]+|10'
	'vl53l4cd_virt|"v0":[0-9]+\.[0-9]+|10'
	'sht3x_virt|"v0":2[0-9]\.[0-9]+|10'
	'mcp3421_virt|"v0":-?[0-9]+\.[0-9]+|10'
	'ms5611_virt|"v0":10[01][0-9]\.[0-9]+|10'
	'sgp30_virt|"v0":[4-7][0-9][0-9]\.[0-9]+|10'
	'ina219_virt|"v0":1[12]\.[0-9]+|10'
	'ads1115_virt|"v0":1\.[0-9]+|10'
	# AS7262 raw 六通道是无符号整数计数（:f 透传无小数），浮点断言锚定 monf
	# 行 Ema 对 V 计数的 α 混合输出 out（必为带小数浮点）
	'as7262_virt|"out":[0-9]+\.[0-9]+|10'
	# MAX30102 raw IR/RED 同为无符号 18 位整数计数，浮点断言锚定 monf 行
	# Ema 对 IR 计数的 α 混合输出 out（必为带小数浮点）
	'max30102_virt|"out":[0-9]+\.[0-9]+|10'
	# AMG8833 raw MAXR/MAXC 是整数行列，浮点断言锚定 monf 行 Ema 对
	# AVGT 平均温度的 α 混合输出 out（必为带小数浮点）
	'amg8833_virt|"out":[0-9]+\.[0-9]+|10'
	# PCF8574 raw PINS/OUT 是无符号 8 位整数引脚码（:f 透传无小数），
	# 浮点断言锚定 monf 行 Ema 对 PINS walking 位码的 α 混合输出 out
	'pcf8574_virt|"out":[0-9]+\.[0-9]+|10'
	# LTC2944 raw 四字段均 :f 浮点；v0=CHARGE/v1=V/v2=I/v3=T，锚定 v1
	# 母线电压正弦（11.6~12.4V，带小数即证明未被 sign=16 取整）
	'ltc2944_virt|"v1":1[12]\.[0-9]+|10'
	# CHT8305 raw 两字段均 :f 浮点；v0=T（20~30°C 正弦）/v1=H（40~60%RH
	# 反相），锚定 v0 温度（带小数即证明未被 sign=16 取整）
	'cht8305_virt|"v0":2[0-9]\.[0-9]+|10'
	# ADS1015 raw 单字段 voltage:f（12 位左对齐孪生，V=raw12*FS/2048），
	# AIN0 正弦 1.40~1.90V，锚定 v0（带小数即证明未被 sign=16 取整）
	'ads1015_virt|"v0":1\.[0-9]+|10'
	# INA226 raw 三字段均 :f 浮点；v0=V/v1=I/v2=P（总线 16 位无移位
	# 1.25mV、分流 2.5µV 全软件换算），锚定 v0 母线电压正弦（11.6~12.4V）
	'ina226_virt|"v0":1[12]\.[0-9]+|10'
	# INA3221 raw 六字段均 :f 浮点；v0=V1/v1=I1/v2=V2/v3=I2/v4=V3/v5=I3
	# （三通道 13 位 <<3：总线 8mV、分流 40µV 全软件换算），锚定 v0 的
	# ch1 母线电压正弦（11.6~12.4V）
	'ina3221_virt|"v0":1[12]\.[0-9]+|10'
	# MAX17043 raw 两字段均 :f 浮点；v0=VCELL（12 位 <<4、1.25mV 全软件
	# 换算，3.60~4.10V 锂电正弦）/v1=SOC（1/256% 定点），锚定 v0
	'max17043_virt|"v0":[34]\.[0-9]+|10'
	# ADXL345 raw 三轴均 sign='16' 展开的有符号整数码（无 :f 字段），
	# 浮点断言锚定 monf 行 Ema 对 ACCEL_X 正弦码（±77，含负值）的 α 混合 out
	'adxl345_virt|"out":-?[0-9]+\.[0-9]+|10'
	# LPS22HB raw 两字段均 :f 浮点；v0=P（24 位小端/4096，1008.25~1018.25hPa
	# 海平面正弦）/v1=T（16 位小端/100，20~24°C），锚定 v0 气压（带小数即
	# 证明未被 sign=16 取整）
	'lps22hb_virt|"v0":10[01][0-9]\.[0-9]+|10'
)

run_one_e2e() {
	local name="$1" float_ere="$2" count_min="$3"
	echo "----- $name (mybot E2E) -----"
	if [ ! -x "$bin" ]; then
		echo "[SKIP] 无编译产物 $bin（先执行 cmake --build build）"
		return 2
	fi
	# 动作时长约 1s，给 30s 上限防挂死；mybot 退出码原样透传
	if [ -n "$timeout_cmd" ]; then
		out="$(cat "scripts/test/$name.mt" | timeout 30 "$bin" --robot="robots/test/$name.sql" 2>&1)"
	else
		out="$(cat "scripts/test/$name.mt" | "$bin" --robot="robots/test/$name.sql" 2>&1)"
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

if [ ! -x "$bin" ]; then
	for spec in "${e2e_cases[@]}"; do
		name="${spec%%|*}"
		if [ -z "$filter" ] || [[ "$name" == *"$filter"* ]]; then
			echo "----- $name (mybot E2E) -----"
			echo "[SKIP] 无编译产物 $bin（先执行：cmake -S . -B build && cmake --build build；BOT=xxx 可指定其他二进制）"
		fi
	done
else
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

# ======================================================================
# mybot 无硬件契约回归（无需 I2C/SPI 真机）：
# 正值：退出码 0 + 输出必须包含锚点；负值：退出码非 0（表名拼错、
# 别名重复、$ 引用标量等应清晰报错绝不裸崩）。覆盖：
#   sqlquote      引号值内 "--"/"/*"/";"/","/括号/空白按字面解析
#   insert_typo   INSERT 目标表拼错 → 解析期报错
#   daemon/filter/ema_chain/monitor/mpu_daemon/multidaemon
#                 守护采样→消费链路（含 Kalman/Ema/Monitor）
#   relay-test / ref_prefix   Relay.controller 转发 + note 透传（$ 前缀字面）
#   alias_inline 动作脚本内联别名生效；alias_override/alias_dup 覆盖/重复负值
#   ref_alias_scalar  $ 引用指向标量 Alias → 报 "non-object" 而非类型崩溃
# 用例表：SQL|MT|期望退出码|输出必须包含（MT/SQL 含 "/" 时视为仓库根相对路径）
# 二进制缺省 ./build/mybot，可用 BOT=xxx 覆盖（与 E2E 段同约定）。
# ======================================================================
parse_cases=(
	'sqlquote.sql|sqlquote.mt|0|joined, total publishes='
	'insert_typo.sql|insert_typo.mt|1|table is not defined'
	'daemon.sql|daemon.mt|0|joined, total publishes='
	'filter.sql|filter.mt|0|joined, total publishes='
	'ema_chain.sql|ema_chain.mt|0|joined, total publishes='
	'monitor.sql|monitor.mt|0|joined, total publishes='
	'mpu_daemon.sql|mpu_daemon.mt|0|joined, total publishes='
	'multidaemon.sql|multidaemon.mt|0|joined, total publishes='
	'robots/relay-test.sql|scripts/relay-test.mt|0|note=hello-relay'
	'ref_prefix.sql|ref_prefix.mt|0|Relay: test=200 note=timer-0'
	'alias_inline.sql|alias_inline.mt|0|served 1 call(s)'
	'alias_override.sql|alias_override.mt|1|missing or not a number'
	'alias_dup.sql|alias_dup.mt|1|Duplicate alias name'
	'ref_alias_scalar.sql|ref_alias_scalar.mt|1|referenced variable returned a non-object'
)
run_one_parse() {
	local sql="$1" mt="$2" want_rc="$3" expect="$4"
	echo "----- $sql/$mt (no-hw contract) -----"
	if [ ! -x "$bin" ]; then
		echo "[SKIP] 无编译产物 $bin（先执行 cmake --build build）"
		return 2
	fi
	local sql_path="$sql" mt_path="$mt"
	[[ "$sql" != */* ]] && sql_path="robots/test/$sql"
	[[ "$mt" != */* ]] && mt_path="scripts/test/$mt"
	if [ -n "$timeout_cmd" ]; then
		out="$(cat "$mt_path" | timeout 30 "$bin" --robot="$sql_path" 2>&1)"
	else
		out="$(cat "$mt_path" | "$bin" --robot="$sql_path" 2>&1)"
	fi
	local rc=$?
	echo "$out"
	local p_ok=true
	[ "$rc" -ne "$want_rc" ] && p_ok=false
	echo "$out" | grep -qF "$expect" || p_ok=false
	if $p_ok; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		failed="$failed $sql/$mt(no-hw)"
	fi
}
for spec in "${parse_cases[@]}"; do
	sql="${spec%%|*}"
	rest="${spec#*|}"
	mt="${rest%%|*}"
	rest="${rest#*|}"
	want_rc="${rest%%|*}"
	expect="${rest##*|}"
	if [ -n "$filter" ] && [[ "$sql" != *"$filter"* ]] && [[ "$mt" != *"$filter"* ]]; then
		continue
	fi
	run_one_parse "$sql" "$mt" "$want_rc" "$expect"
done

# ======================================================================
# TCP 动作源端到端：唯一覆盖 actionsource.cpp（--tcp 监听分支）的守门。
# 复用 scripts/tcp_e2e.sh：起服务端 → Python 客户端下发 relay-test.mt →
# 校验服务端日志。python3 或编译产物缺位则跳过（不假绿不假红）。
# 随机高位端口（19000-19999）规避默认 19240 被占/串扰。
# ======================================================================
run_one_tcp() {
	echo "----- tcp-action-source (mybot TCP E2E) -----"
	if [ ! -x "$bin" ]; then
		echo "[SKIP] 无编译产物 $bin（先执行 cmake --build build）"
		return 2
	fi
	if ! command -v python3 >/dev/null 2>&1; then
		echo "[SKIP] 无 python3（TCP 客户端），跳过"
		return 2
	fi
	local port tcp_rc tcp_out
	port=$((19000 + (RANDOM % 1000)))
	tcp_out="$(BOT="$bin" timeout 40 bash scripts/tcp_e2e.sh "$port" 20 2>&1)"
	tcp_rc=$?
	echo "$tcp_out"
	if [ $tcp_rc -ne 0 ] || ! echo "$tcp_out" | grep -qF "TCP 端到端测试通过"; then
		fail=$((fail + 1))
		failed="$failed tcp-action-source(TCP)"
	else
		pass=$((pass + 1))
	fi
}
if [ -z "$filter" ] || [[ "tcp" == *"$filter"* ]]; then
	run_one_tcp
fi


# 空匹配防线：过滤器拼错时 0 通过 0 失败不得假绿
if [ $((pass + fail)) -eq 0 ]; then
	echo "汇总: 无匹配用例（过滤器 '${filter}' 未命中任何 test/test_*.lua、mybot E2E、无硬件契约回归或 TCP E2E，请检查拼写）"
	exit 1
fi
echo "汇总: $pass 通过, $fail 失败"
if [ $fail -ne 0 ]; then
	echo "失败用例:$failed"
	exit 1
fi
