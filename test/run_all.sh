#!/bin/bash
# bot4cj Lua 离线测试统一入口
# 用法: bash test/run_all.sh [测试名过滤]
#   例: bash test/run_all.sh                # 跑全部
#       bash test/run_all.sh pca9685       # 只跑文件名含 pca9685 的
# 说明：
#   - 统一用 luajit 运行（HAL 依赖 ffi）；可用环境变量 LUACMD 覆盖解释器
#   - 单测超时 TEST_TIMEOUT 秒（默认 60，需 coreutils timeout；缺失则不限时）——
#     防单个用例死循环挂死整个入口
#   - test_jit_warmup.lua 是真机 ABI 回归（需 /dev/i2c-*）、test_camera_v4l2.lua 是
#     真机摄像头回归（需 /dev/video*）：文件首行标记 -- requires: i2c / video，无设备自动跳过
#   - 守门完整性：编译产物（mybot / mybot_tests）缺失的分段**判失败**而非静默跳过——
#     否则整段 C++ 守门可以在不构建的情况下消失而汇总仍报「0 失败」。
#     仅三类例外按跳过计（汇总里以「N 跳过 + 未执行清单」显式可见）：
#     真机 i2c 用例（本机无 /dev/i2c-*）、TCP/agent 段缺 python3（README 声明的
#     软依赖）、agent 端到端缺 libcurl（agent 模式的运行期依赖，Lua 驱动经 ffi
#     加载；缺它时驱动 build 报「libcurl 不可用」，本入口据此判 SKIP 而非假绿）。
#     只想跑 Lua 段的逃生口：ALLOW_SKIP=1 bash test/run_all.sh
#     （显式给了 BOT=/BOT_TESTS= 却指向不存在的文件，一律判失败，ALLOW_SKIP 也不放行）
#   - test_ioctl_nums.c 为 C 编译期常量自检，不在本入口范围
# 本文件绝对路径：下面 cd 之后相对 $0 不再可解析，而 count_calls 靠它数用例
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
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
skip=0
hardening_reported=""
skipped_list=""

# ---- 记账单点（N-31）----
# 11 个 runner 原各写一遍「if ok; then pass++; else fail++; failed+=…」三行，
# 加一段就再抄一遍、改口径（如新增「跳过」类别或改汇总措辞）要全文件找。
# 收敛为三个函数：通过只计数；失败/跳过额外把可读标签写进汇总清单。
record_pass() { pass=$((pass + 1)); }
record_fail() { fail=$((fail + 1)); failed="$failed $1"; }
record_skip() { skip=$((skip + 1)); skipped_list="$skipped_list $1"; }

# 某 runner 的声明用例数：数本文件里的调用点，让「加用例忘改数字」的消息漂移
# 无从发生（artifact_missing 的条数文案一律由此推导）。
count_calls() { grep -c "^[[:space:]]*$1 '" "$SELF"; }

# ---- 编译产物（全局解析，供各分段共用）----
# 缺省路径可被 BOT / BOT_TESTS 覆盖；是否「显式指定」决定下面的降级策略。
bin="${BOT:-./build/mybot}"
tests_bin="${BOT_TESTS:-./build/mybot_tests}"
bot_explicit=0
[ -n "${BOT:-}" ] && bot_explicit=1
tests_explicit=0
[ -n "${BOT_TESTS:-}" ] && tests_explicit=1

# ---- 产物缺失的分段不得静默消失（N1 防线）----
# 原语义是「无编译产物则逐条 [SKIP]」，于是整段 C++ 守门（E2E + 无硬件契约 +
# TCP/信号/加固 + C++ 单测）可以在产物不存在时整体不跑，汇总仍是「N 通过, 0 失败」
# 且退出码 0——只剩 Lua 段在跑。守门必须完整：缺产物即判失败。
# 仅当 ALLOW_SKIP=1（明知故犯，如源码态只想跑 Lua）**且**该路径是缺省值时才降级
# 为 SKIP；调用方显式给了 BOT=/BOT_TESTS= 却说文件不在，本就是要测它，绝不放行。
artifact_missing() {
	local phase="$1" count="$2" path="$3" explicit="$4"
	if [ "$ALLOW_SKIP" = "1" ] && [ "$explicit" != "1" ]; then
		echo "[SKIP] $phase —— $count未执行（缺编译产物 $path，ALLOW_SKIP=1 放行）"
		record_skip "$phase"
		return 0
	fi
	echo "[FAIL] 守门不完整：$phase —— $count未执行（缺可执行产物 $path）"
	echo "       先构建：cmake -S . -B build && cmake --build build"
	echo "       只想跑 Lua 段请显式声明：ALLOW_SKIP=1 bash test/run_all.sh"
	record_fail "$phase(缺产物)"
	return 1
}

# 当前过滤器下某分段的声明用例数（把「整段未执行」折算成准确条数）
count_e2e_declared() {
	local n=0 spec name
	for spec in "${e2e_cases[@]}"; do
		name="${spec%%|*}"
		if [ -z "$filter" ] || [[ "$name" == *"$filter"* ]]; then n=$((n + 1)); fi
	done
	echo "$n"
}
count_parse_declared() {
	local n=0 spec sql rest mt
	for spec in "${parse_cases[@]}"; do
		sql="${spec%%|*}"
		rest="${spec#*|}"
		mt="${rest%%|*}"
		if [ -z "$filter" ] || [[ "$sql" == *"$filter"* ]] || [[ "$mt" == *"$filter"* ]]; then
			n=$((n + 1))
		fi
	done
	echo "$n"
}

for t in test/test_*.lua; do
	name="$(basename "$t")"
	if [ -n "$filter" ] && [[ "$name" != *"$filter"* ]]; then
		continue
	fi
	# 真机测试：文件内声明 "-- requires: i2c" 且本机无 /dev/i2c-* 时跳过
	if grep -q '^\-\- requires: i2c' "$t" && ! ls /dev/i2c-* >/dev/null 2>&1; then
		echo "----- $name -----"
		echo "[SKIP] 需要 /dev/i2c-* 真机总线（文件标记 -- requires: i2c），本机无 i2c，跳过"
		record_skip "$name"
		continue
	fi
	# 真机摄像头：文件内声明 "-- requires: video" 且本机无 /dev/video* 时跳过
	if grep -q '^\-\- requires: video' "$t" && ! ls /dev/video* >/dev/null 2>&1; then
		echo "----- $name -----"
		echo "[SKIP] 需要 /dev/video* 真机摄像头（文件标记 -- requires: video），本机无 video 设备，跳过"
		record_skip "$name"
		continue
	fi
	echo "----- $name -----"
	out="$($timeout_cmd "$LUACMD" "$t" 2>&1)"
	rc=$?
	echo "$out"
	pass_n="$(printf '%s\n' "$out" | grep -cF '[PASS]')"
	# 兼容三种失败约定：退出码非零、打印 [FAIL] 行（124 = timeout 超时）、
	# 或假绿形态（P0-2）：早退/守卫短路导致未见 summary 的 [DONE] 完成标记，
	# 或一条断言都没跑（[PASS] 计数为 0）——「安静退出」不再视同通过
	if [ $rc -eq 124 ] && [ -n "$timeout_cmd" ]; then
		echo "[FAIL] 用例超时（>${TEST_TIMEOUT}s，疑似死循环）"
		record_fail "$name"
	elif [ $rc -ne 0 ] || echo "$out" | grep -qF "[FAIL]"; then
		record_fail "$name"
	elif [ "$pass_n" -eq 0 ] || ! echo "$out" | grep -qF "[DONE]"; then
		echo "[FAIL] 假绿嫌疑：未见 [DONE] 完成标记或 [PASS] 计数为 0（早退/守卫短路/零断言）"
		record_fail "$name"
	else
		record_pass
	fi
done

echo "==============================================="

# ======================================================================
# mybot（C++ 移植）无硬件端到端：编译产物缺失即判失败（见 artifact_missing）。
# 用例走真实 mybot 二进制：守护线程→共享槽→Ema→Monitor 全链路，
# I2C/SPI 器件均为内存仿真模型（内嵌于各驱动尾部 <DRIVER>_VIRT 工厂，
# lib/virtbus/ 仅存通用底座 regmap.lua）。
#
# 用例表：用例名|raw 快照浮点断言 ERE（证明 :f 字段未被取整）|count 下限。
# EMA 混合断言对所有用例通用：某拍 monf 行内 out ≠ raw（滤波确实推进）。
# 二进制缺省 ./build/mybot，可用 BOT=xxx 覆盖。
# ======================================================================
e2e_cases=(
	'mpu_virt|"v6":41\.[0-9]+|5'
	'bme280_virt|"v0":2[0-9]\.[0-9]+|5'
	'ads1256_virt|"v1":[0-2]\.[0-9]+|5'
	'vl53l0x_virt|"v0":[0-9]+\.[0-9]+|5'
	'vl53l4cd_virt|"v0":[0-9]+\.[0-9]+|5'
	'sht3x_virt|"v0":2[0-9]\.[0-9]+|5'
	'mcp3421_virt|"v0":-?[0-9]+\.[0-9]+|5'
	'ms5611_virt|"v0":10[01][0-9]\.[0-9]+|5'
	'sgp30_virt|"v0":[4-7][0-9][0-9]\.[0-9]+|5'
	'ina219_virt|"v0":1[12]\.[0-9]+|5'
	'ads1115_virt|"v0":1\.[0-9]+|5'
	# AS7262 raw 六通道是无符号整数计数（:f 透传无小数），浮点断言锚定 monf
	# 行 Ema 对 V 计数的 α 混合输出 out（必为带小数浮点）
	'as7262_virt|"out":[0-9]+\.[0-9]+|5'
	# MAX30102 raw IR/RED 同为无符号 18 位整数计数，浮点断言锚定 monf 行
	# Ema 对 IR 计数的 α 混合输出 out（必为带小数浮点）
	'max30102_virt|"out":[0-9]+\.[0-9]+|5'
	# AMG8833 raw MAXR/MAXC 是整数行列，浮点断言锚定 monf 行 Ema 对
	# AVGT 平均温度的 α 混合输出 out（必为带小数浮点）
	'amg8833_virt|"out":[0-9]+\.[0-9]+|5'
	# PCF8574 raw PINS/OUT 是无符号 8 位整数引脚码（:f 透传无小数），
	# 浮点断言锚定 monf 行 Ema 对 PINS walking 位码的 α 混合输出 out
	'pcf8574_virt|"out":[0-9]+\.[0-9]+|5'
	# LTC2944 raw 四字段均 :f 浮点；v0=CHARGE/v1=V/v2=I/v3=T，锚定 v1
	# 母线电压正弦（11.6~12.4V，带小数即证明未被 sign=16 取整）
	'ltc2944_virt|"v1":1[12]\.[0-9]+|5'
	# CHT8305 raw 两字段均 :f 浮点；v0=T（20~30°C 正弦）/v1=H（40~60%RH
	# 反相），锚定 v0 温度（带小数即证明未被 sign=16 取整）
	'cht8305_virt|"v0":2[0-9]\.[0-9]+|5'
	# ADS1015 raw 单字段 voltage:f（12 位左对齐孪生，V=raw12*FS/2048），
	# AIN0 正弦 1.40~1.90V，锚定 v0（带小数即证明未被 sign=16 取整）
	'ads1015_virt|"v0":1\.[0-9]+|5'
	# INA226 raw 三字段均 :f 浮点；v0=V/v1=I/v2=P（总线 16 位无移位
	# 1.25mV、分流 2.5µV 全软件换算），锚定 v0 母线电压正弦（11.6~12.4V）
	'ina226_virt|"v0":1[12]\.[0-9]+|5'
	# INA3221 raw 六字段均 :f 浮点；v0=V1/v1=I1/v2=V2/v3=I2/v4=V3/v5=I3
	# （三通道 13 位 <<3：总线 8mV、分流 40µV 全软件换算），锚定 v0 的
	# ch1 母线电压正弦（11.6~12.4V）
	'ina3221_virt|"v0":1[12]\.[0-9]+|5'
	# MAX17043 raw 两字段均 :f 浮点；v0=VCELL（12 位 <<4、1.25mV 全软件
	# 换算，3.60~4.10V 锂电正弦）/v1=SOC（1/256% 定点），锚定 v0
	'max17043_virt|"v0":[34]\.[0-9]+|5'
	# MAX17048 raw 两字段均 :f 浮点；v0=VCELL（16 位直读无移位、78.125µV
	# 全软件换算，3.60~4.10V 锂电正弦）/v1=SOC（1/256% 定点），锚定 v0
	'max17048_virt|"v0":[34]\.[0-9]+|5'
	# BH1745 raw 四通道是无符号 16 位整数计数（:f 透传无小数），浮点断言
	# 锚定 monf 行 Ema 对 R 计数（19k~25k 正弦）的 α 混合输出 out
	'bh1745_virt|"out":[12][0-9]{4}\.[0-9]+|5'
	# BH1750 命令式环境光（单字节 opcode、测量值纯读 2 字节大端 count）：
	# raw v0=LUX:f 浮点（virt_tick 32 拍正弦 count 12002~18002 → 10001.7~15001.7）。
	# 正弦里有少数拍 count 恰为 6 的倍数（如 17496→14580）raw 成整数、而守护
	# 20ms 拍与 50ms pump 存在混叠（快照采到哪拍不固定），故浮点断言锚定 monf
	# 行 Ema 的 α 混合输出 out（与 bh1745/mhz19 同口径）证明 :f 未被 sign=16 取整
	'bh1750_virt|"out":1[0-9]{4}\.[0-9]+|5'
	# APDS9900 raw LUX 本身即 :f 浮点照度（内核两线 IR 补偿，缺省档
	# 1354~3160 lux 正弦），CH0/CH1/PROX 整数计数 :f 透传；锚定 v0=LUX
	'apds9900_virt|"v0":[12][0-9]{3}\.[0-9]+|5'
	# BMM150 raw X/Y/Z 三轴地磁 µT（XY 13 位、Z 15 位 + RHALL 14 位，Bosch
	# 整数补偿管线收尾 idiv /16，输出恒为整数值、JSON 无小数点），浮点断言
	# 锚定 monf 行 Ema 对 Y 轴（-27~3µT 正弦，含负值）的 α 混合输出 out
	'bmm150_virt|"out":-?[0-9]+\.[0-9]+|5'
	# MLX90393 raw 四字段均 :f 浮点；v0=X/v1=Y/v2=Z（µT，命令式 status-first
	# 器件，gain0 0.751/1.210µT/LSB 量化后正算回物理量）/v3=T（°C），锚定
	# v0 的 X 轴 ±30µT 正弦（含负值，带小数即证明未被 sign=16 取整）
	'mlx90393_virt|"v0":-?[0-9]+\.[0-9]+|5'
	# AS3935 事件型：v0=CODE 0~3/v1=DIST_KM 公里/-1 两整数，v2=ENERGY
	# 21 位能量（tick 闪电拍 217157/782842，整数物理量 :f 透传无小数点；
	# 若无 :f 会被 sign=16 截断到 ≤65535），锚定 raw 行 v2 六位整数；
	# Ema 滤 v1 DIST_KM（-1 与 10~35 混合），通用断言验 monf out≠raw
	'as3935_virt|"v2":[0-9]{6}[,}]|5'
	# EMC2101 风扇控制器：v0=TEMP_IN 整数/v1=TEMP_EXT:f 1/8°C 小数/
	# v2=RPM:f 转速，tick 正弦恒四位数 1800~4200（锚 raw v2 四位整数
	# 部分，整除拍 JSON 无小数点、其余拍带小数，两种形态都算通过，避免
	# 采样相位碰不到整除拍的抖动；Ema 滤 v1 外温，通用断言验 monf）
	'emc2101_virt|"v2":[0-9]{4}(\.[0-9]+)?[,}]|5'
	# BME688 四合一：v0=T:f（22~28°C 正弦）/v1=P:f/v2=H:f/v3=GAS:f
	# 气体电阻/v4=GAS_OK 整数，forced 门靠 virt_tick 预武装零睡眠，
	# 锚 v0 温度（带小数即证明未被 sign=16 取整）
	'bme688_virt|"v0":2[0-9]\.[0-9]+|5'
	# BMI270 六轴 IMU：ACCEL_X/Y/Z + GYRO_X/Y/Z 六字段均 sign='16'
	# 展开的有符号整数码（无 :f），配置 blob 上传 + 150ms 加载门靠
	# virt_tick 预武装（build 仅睡 114ms），浮点断言锚定 monf 行 Ema
	# 对 ACCEL_X 正弦码（100~500）的 α 混合输出 out
	'bmi270_virt|"out":-?[0-9]+\.[0-9]+|5'
	# LSM6DSV16X 六轴 IMU：ACCEL_X/Y/Z + GYRO_X/Y/Z 六字段均 sign='16'
	# 展开的有符号整数码（无 :f），无 blob/无转换门（build 实睡 110ms：
	# 10ms 复位轮询+100ms 陀螺启动），浮点断言锚定 monf 行 Ema 对
	# ACCEL_X 正弦码（100~500）的 α 混合输出 out
	'lsm6dsv_virt|"out":-?[0-9]+\.[0-9]+|5'
	# MT6826S 15 位磁编码器（SPI Mode3，首个全双工同帧器件）：v0=ANGLE:f
	# 度数（virt_tick 32 拍线性爬坡，11.25° 步进，非零拍必带小数）/
	# v1=RAW/v2=STATUS/v3=CRC_OK 整数；锚 v0 浮点（证明 :f 未被取整）
	'mt6826s_virt|"v0":[0-9]+\.[0-9]+|5'
	# MH-Z19B NDIR CO₂（首个 UART virt 器件，/dev/serial0@9600 请求-应答帧）：
	# v0=CO2:f 整数 ppm（virt_tick 32 拍一周爬坡 400~4368、128ppm 步进，
	# :f 透传无小数）/v1=TEMP/v2=S；浮点断言锚定 monf 行 Ema 对浓度的 α 混合 out
	'mhz19_virt|"out":[0-9]+\.[0-9]+|5'
	# SDS011 激光颗粒物（第二个 UART virt 器件，/dev/serial0@9600 被动查询模式）：
	# v0=PM25:f / v1=PM10:f 浮点 µg/m³（virt_tick 5 拍一周扬尘爬坡，0.8 步进、
	# raw 码 mod 10 恒非 0 ⇒ raw 行必带小数，锚 mon0 的 v0 即证明 :f 未取整）
	'sds011_virt|"v0":[0-9]+\.[0-9]+|5'
	# ADXL345 raw 三轴均 sign='16' 展开的有符号整数码（无 :f 字段），
	# 浮点断言锚定 monf 行 Ema 对 ACCEL_X 正弦码（±77，含负值）的 α 混合 out
	'adxl345_virt|"out":-?[0-9]+\.[0-9]+|5'
	# LPS22HB raw 两字段均 :f 浮点；v0=P（24 位小端/4096，1008.25~1018.25hPa
	# 海平面正弦）/v1=T（16 位小端/100，20~24°C），锚定 v0 气压（带小数即
	# 证明未被 sign=16 取整）
	'lps22hb_virt|"v0":10[01][0-9]\.[0-9]+|5'
)

run_one_e2e() {
	local name="$1" float_ere="$2" count_min="$3"
	echo "----- $name (mybot E2E) -----"
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
		record_pass
	else
		record_fail "$name(E2E)"
	fi
}

if [ ! -x "$bin" ]; then
	n_declared="$(count_e2e_declared)"
	[ "$n_declared" -gt 0 ] && artifact_missing "mybot E2E" "$n_declared 条用例" "$bin" "$bot_explicit"
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
#   number_type   CREATE TABLE 支持 Number 列（解析期一次数值校验，值仍文本驻留）
#   number_bad    Number 列写入非数值字面量 → 解析期清晰报错
#   daemon/filter/ema_chain/monitor/mpu_daemon/multidaemon
#                 守护采样→消费链路（含 Kalman/Ema/Monitor）
#   daemon_setup_fail  守护 setup 失败响亮契约（N-27）：失败码进槽 status 字段
#                 而非伪装成 v0 采样值，宿主按 ERR 报错并含 "slot status -8"
#   daemon_badproto/daemon_fieldmode/daemon_fieldempty
#                 守护配置 schema 加载期校验（E2）：非法 proto/fields 在结构脚本
#                 加载期报错（含守护名/列/表/行号），不再静默按 i2c 走或当浮点透传
#   relay-test / ref_prefix   Relay.controller 转发 + note 透传（$ 前缀字面）
#   alias_inline 动作脚本内联别名生效；alias_override/alias_dup 覆盖/重复负值
#   vars_num     @Vars 仅支持 Number：结构表 $ 数值常量填充 Relay.note
#   vars_bad/vars_nonnum/vars_dup  Vars 缺 value 列 / 非数值 / 动作脚本内重复定义 → 负值契约
#   expr_num     $(...) 表达式：Vars 内 + SQL 元数据列 + @Action 单元格三处
#   expr_undefined 元数据列表达式引用未定义标识符 → 装配期报错（先于任何 build）
#   expr_overflow 元数据列表达式溢出为 inf → 求值期报错（有限值防线，inf/nan 不入驱动）
#   mt_noblank  矩阵间只有单个换行（无空行）→ 第 2+ 个矩阵名仍须正确识别
#   mt_crlf     同上但 CRLF 行尾 → 修复不得只按 LF 语义而破坏 CRLF
#   mt_ragged   行内单元格数与表头不齐 → 须报 "Does Not Sync" 且行号准确
#   ref_alias_scalar  $ 引用指向标量 Alias → 报 "non-object" 而非类型崩溃
#   ref_forward    $ 前向引用合法（装配后统一校验）+ destroy 只取字面列
#                  （不再出现读已释放引用的 Deactive warning，F-04/F-05）
#   ref_undefined  $ 指向未定义变量 → 装配期报错且先于任何驱动 build（无 spawned）
#   ref_override   N-5：会话 @Alias 覆盖结构 @Vars 名字，$ 引用列仍绑定结构期
#                  变量（seed 期锁定目标指针）——note=200 而非按名重查到别名报
#                  "referenced variable returned a non-object"
#   pca9685_virt / pca9685_badchan
#                 首条【非守护】virt 覆盖：@Action 直写舵机驱动板走内存仿真
#                 （drv_common 入口前置注入 + adopt_fd 跨状态机采纳 fd）；
#                 正值断言虚拟横幅，负值断言通道越界被守卫（证明该链路非空断言）
#   mcp4725_virt  第二条【非守护】virt 覆盖（I2C DAC 直写）：同时钉住
#                 「驱动 build/call 走 arg_table（代调 virt_prepare）」——
#                 旧风格入口绕过该钩子时，本用例的 i2c_open 会打到真实
#                 /dev/i2c-99 响亮失败而非仿真（2026-09-19 审计修复回归）
#   scscl_virt / scscl_badid
#                 首个【UART 非守护】virt 覆盖（BUS="uart" 模型 attach_uart +
#                 adopt uart fd）：@Action 直写总线舵机（SCSCL，大端帧/1M 波特率）；
#                 负值断言站号非法在 build 期被守卫
#   pca9685_virt  第 5 字段禁 "Deactive warning"（F-04′）：destroy 期元数据必须
#                 带着 $i2c-v 的 fd 快照，否则去初始化报 Invalid Argument: fd or address
#                 （该失败只打警告不改退出码，故只能靠禁断言抓）
#   ref_bus_forward 接口器件前向引用（总线声明在器件之后）→ build 期即失败（F-04′）：
#                 fd 由器件行经 $i2c-N 从 @I2C 快照而来，而「总线必须先于器件声明」
#                 是 build 期强制的（器件 build 要 fd）——同一条不变式也保证倒序销毁时
#                 总线最后关闭 fd。全程不触总线：PCA9685 build 在 i2c_pair 处即返回，
#                 @I2C 的 build 根本未执行，故插着真舵机的机器上同样安全。
#   trig_basic  trigger 前置调用（controller 的镜像入边）：relay-src 从不出现在
#                 @Action，其 Relay 打印出现即证明前置调用发生；controller 链
#                 闭合（Timer 经转发收到 delay=300）。顺序与数据到位的观察面
#                 （行序、call 参数 dump）在本文件「trigger 前置调用」专段补齐
#   trig_param  .mt 参数列不得占用触发器名——结果子表按触发器名并入调用参数
#   trig_forward/trig_scalar/trig_reserved/trig_collide/trig_dollar/trig_n6
#                 trigger 负值契约：前向引用 / @Alias·@Vars 标量目标 / 框架保留键 /
#                 与本表元数据列同名 / $ 数据引用写法 / 会话别名覆盖（N-6）
#   trig_err    触发器 ERR 整帧 halt：触发目标 Timer 缺 delay 参数必报 ERR，
#                 派发帧响亮中止且退出码 1
#   ctrl2_basic/trig2_basic
#                 controller/trigger 多列（基名+数字编号列）：扇出广播（每个
#                 controller 收同一份 payload，返回值取最后者）/ 依序前置调用
#                 （结果各自按名到位）；行序与同 payload 观察面在本文件
#                 「trigger 前置调用」专段补齐
#   flow_dup/flow_dupc/flow_fwd2/flow_scalar2/trig_scalar2
#                 多列负值契约：同列族重复指向 / controller2 前向引用 /
#                 编号列拒绝 @Alias·@Vars 标量目标
#   ctrl2_err   controller 扇出 fail-fast：第一个 controller ERR 即停止，
#                 后续 controller 不再被转发（relay 目击者禁言断言）
# 用例表：SQL|MT|期望退出码|输出必须包含|输出必须不含（第 5 字段可省；MT/SQL 含 "/" 时视为仓库根相对路径）
# 第 6 字段可省，"I" = 用 --input 文件通道代替 stdin 管道（片段以自身目录为
# include 基准，无法从仓库根以 stdin 单跑——用 I 让它们以正确基准被守门）。
# 二进制缺省 ./build/mybot，可用 BOT=xxx 覆盖（与 E2E 段同约定）。
# ======================================================================
parse_cases=(
	'sqlquote.sql|sqlquote.mt|0|joined, total publishes='
	'insert_typo.sql|insert_typo.mt|1|table is not defined'
	'number_type.sql|number_type.mt|0|note=hello-number'
	'number_bad.sql|number_bad.mt|1|declared Number'
	'daemon.sql|daemon.mt|0|joined, total publishes='
	'filter.sql|filter.mt|0|joined, total publishes='
	'ema_chain.sql|ema_chain.mt|0|joined, total publishes='
	'monitor.sql|monitor.mt|0|joined, total publishes='
	'mpu_daemon.sql|mpu_daemon.mt|0|joined, total publishes='
	'daemon_setup_fail.sql|daemon_setup_fail.mt|1|slot status -8'
	'daemon_badproto.sql|daemon_schema.mt|1|not a supported bus'
	'daemon_fieldmode.sql|daemon_schema.mt|1|unknown field mode'
	'daemon_fieldempty.sql|daemon_schema.mt|1|empty field name'
	'float_s16_daemon.sql|float_s16_daemon.mt|1|would be truncated to integer'
	'multidaemon.sql|multidaemon.mt|0|joined, total publishes='
	'robots/relay-test.sql|scripts/relay-test.mt|0|note=hello-relay'
	'ref_prefix.sql|ref_prefix.mt|0|Relay: test=200 note=timer-0'
	'alias_inline.sql|alias_inline.mt|0|served 1 call(s)'
	'alias_inline.sql|mt_noblank.mt|0|served 1 call(s)'
	'alias_inline.sql|mt_crlf.mt|0|served 1 call(s)'
	# 孤儿检测（F3/N-32）发现的两条「夹具在但没登记」用例：都是纯逻辑离线可跑
	#   servo      控制器为 dummy：offset 漂移 / |min|/|max| 限幅 / 负 max 反向
	#   h03_arr    数组参数跨语言双向往返：JsonArray→Lua 表（# 与下标）→ 回 JsonArray
	'servo.sql|servo.mt|0|Dummy served 2 call(s)'
	'h03_arr.sql|h03_arr.mt|0|"list":[10,20,30]'
	'robots/relay-test.sql|mt_ragged.mt|1|Does Not Sync. we got line: 6'
	'alias_override.sql|alias_override.mt|1|missing or not a number'
	'alias_dup.sql|alias_dup.mt|1|Duplicate alias name'
	'vars_num.sql|vars_num.mt|0|note=200'
	"vars_bad.sql|vars_bad.mt|1|has no 'value' column"
	'vars_nonnum.sql|vars_nonnum.mt|1|neither a Number'
	'alias_dup.sql|vars_dup.mt|1|Duplicate Vars name'
	'vars_num.sql|vars_override.mt|1|must not override the'
	'expr_num.sql|expr_num.mt|0|note=50'
	'expr_undefined.sql|expr_undefined.mt|1|ghost'
	'expr_overflow.sql|expr_overflow.mt|1|non-finite'
	'ref_alias_scalar.sql|ref_alias_scalar.mt|1|referenced variable returned a non-object'
	'ref_cycle.sql|ref_cycle.mt|1|reference detected'
	'ref_forward.sql|ref_forward.mt|0|joined, total publishes=|Deactive warning'
	'ref_undefined.sql|ref_undefined.mt|1|but it is not defined|spawned, config:'
	'ref_override.sql|ref_override.mt|0|note=200|referenced variable returned a non-object'
	'lex_semicolons.sql|lex_semicolons.mt|1|Token ;'
	'param_override.sql|param_override.mt|1|is rejected because'
	'daemon_badfield.sql|daemon_badfield.mt|0|absent or non-numeric in driver result'
	'pca9685_virt.sql|pca9685_virt.mt|0|VIRTUAL HARDWARE|Deactive warning'
	'pca9685_badchan.sql|pca9685_badchan.mt|1|out of range 0-15'
	'mcp4725_virt.sql|mcp4725_virt.mt|0|VIRTUAL HARDWARE'
	'scscl_virt.sql|scscl_virt.mt|0|VIRTUAL HARDWARE'
	'scscl_badid.sql|scscl_badid.mt|1|Invalid Argument: id'
	'scscl_burn_guard.sql|scscl_burn_guard.mt|1|requires allow_burn'
	'ref_bus_forward.sql|ref_bus_forward.mt|1|Invalid Argument: fd or address'
	'trig_basic.sql|trig_basic.mt|0|note=src'
	'trig_basic.sql|trig_param.mt|1|is a trigger reference name'
	'trig_forward.sql|trig_forward.mt|1|The trigger must be defined before'
	'trig_scalar.sql|trig_scalar.mt|1|cannot be trigger targets'
	'trig_reserved.sql|trig_reserved.mt|1|cannot name a trigger'
	'trig_collide.sql|trig_collide.mt|1|collides with metadata column'
	'trig_dollar.sql|trig_dollar.mt|1|does not support '
	'trig_n6.sql|trig_n6.mt|1|referenced as a controller or trigger'
	'trig_err.sql|trig_err.mt|1|Invalid Argument: delay'
	'ctrl2_basic.sql|ctrl2_basic.mt|0|slept'
	'trig2_basic.sql|trig2_basic.mt|0|note=s2'
	'flow_dup.sql|flow_dup.mt|1|more than once'
	'flow_dupc.sql|flow_dupc.mt|1|more than once'
	'flow_fwd2.sql|flow_fwd2.mt|1|The controller must be defined before'
	'flow_scalar2.sql|flow_scalar2.mt|1|cannot be controller targets'
	'trig_scalar2.sql|trig_scalar2.mt|1|cannot be trigger targets'
	'ctrl2_err.sql|ctrl2_err.mt|1|Invalid Argument: delay|note=witness'
	# @Include 展开（运行端动作脚本归并，归并内核与 mtmerge 共用）：
	#   include_a/include_b    两个被包含脚本单独跑也合法（正值，登记夹具防孤儿）
	#   include_inner          主脚本无 @Action：首个 include 顶替累加器
	#   include_nested         嵌套正值：主脚本 → 组合片段（无自有 @Action）→ 两叶
	#   include_main           归并正例（观察面专段补全量值断言，此处钉 110 出现 + 0 不出现）
	#   include_overlap        主脚本与 include 共享设备 → 设备集相交报错
	#   include_missing        引用不存在的文件 → 清晰报错
	#   include_twice          普通 .mt 重复包含 → 报错（抓笔误；共享用 *.once.mt）
	#   include_gait.once.mt   参数化片段单独跑：$(amp) 取自身缺省 21
	#   include_knob           旋钮正值：主脚本 @Vars 覆盖片段 amp → test=35 而非 21
	#   shared/fa/fb.once.mt   菱形三件套单独跑均合法（fb 单独跑时 shared 未包含过）
	#   include_diamond        菱形正值：两臂都包含 shared.once.mt，第二路跳过（注记）
	'include_demo.sql|include_a.mt|0|test=110'
	'include_demo.sql|include_b.mt|0|test=220'
	'include_demo.sql|include_inner.mt|0|test=220'
	'include_demo.sql|include_main.mt|0|test=110|test=0'
	'include_demo.sql|include_nested.mt|0|test=220'
	'include_demo.sql|include_overlap.mt|1|appears in both scripts'
	'include_demo.sql|include_missing.mt|1|cannot open include file'
	'include_demo.sql|include_twice.mt|1|more than once'
	'include_demo.sql|include_gait.once.mt|0|test=21||I'
	'include_demo.sql|include_knob.mt|0|test=35|test=21'
	'include_demo.sql|shared.once.mt|0|test=300||I'
	'include_demo.sql|fa.once.mt|0|test=100||I'
	'include_demo.sql|fb.once.mt|0|test=200||I'
	'include_demo.sql|include_scene.once.mt|0|test=220||I'
	'include_demo.sql|include_diamond.mt|0|test=300'
)
run_one_parse() {
	local sql="$1" mt="$2" want_rc="$3" expect="$4" forbid="$5" via="${6:-}"
	echo "----- $sql/$mt (no-hw contract) -----"
	local sql_path="$sql" mt_path="$mt"
	[[ "$sql" != */* ]] && sql_path="robots/test/$sql"
	[[ "$mt" != */* ]] && mt_path="scripts/test/$mt"
	if [ "$via" = "I" ]; then
		# --input 文件通道：include 基准 = 脚本所在目录（片段夹具的正确基准）
		if [ -n "$timeout_cmd" ]; then
			out="$($timeout_cmd "$bin" --robot="$sql_path" --input="$mt_path" 2>&1)"
		else
			out="$("$bin" --robot="$sql_path" --input="$mt_path" 2>&1)"
		fi
	elif [ -n "$timeout_cmd" ]; then
		out="$(cat "$mt_path" | timeout 30 "$bin" --robot="$sql_path" 2>&1)"
	else
		out="$(cat "$mt_path" | "$bin" --robot="$sql_path" 2>&1)"
	fi
	local rc=$?
	echo "$out"
	local p_ok=true
	[ "$rc" -ne "$want_rc" ] && p_ok=false
	echo "$out" | grep -qF "$expect" || p_ok=false
	# 可选「禁止出现」断言（用例表第 5 字段）：用于「修复后不再出现的旧症状」
	# （如 $ 引用已释放变量的 Deactive warning、build 期不该发生的 spawned）
	if [ -n "$forbid" ]; then
		echo "$out" | grep -qF "$forbid" && p_ok=false
	fi
	if $p_ok; then
		record_pass
	else
		record_fail "$sql/$mt(no-hw)"
	fi
}
# parse_cases 的切分与过滤循环（原先内联在顶层——bash 的 local 只能在函数内
# 使用，循环体里的 `local f` 每次迭代都向 stderr 报错一次且变量泄漏为全局）。
# 收进函数后切分变量按函数局部作用域，`filter` 仍读全局。
run_parse_cases() {
	local spec sql rest mt want_rc f expect forbid via
	for spec in "${parse_cases[@]}"; do
		sql="${spec%%|*}"
		rest="${spec#*|}"
		mt="${rest%%|*}"
		rest="${rest#*|}"
		want_rc="${rest%%|*}"
		rest="${rest#*|}"
		# 剩余字段：expect[|forbid[|via]]（via="I" = --input 文件通道，见用例表注释）
		IFS='|' read -ra f <<< "$rest"
		expect="${f[0]:-}"
		forbid="${f[1]:-}"
		via="${f[2]:-}"
		if [ -n "$filter" ] && [[ "$sql" != *"$filter"* ]] && [[ "$mt" != *"$filter"* ]]; then
			continue
		fi
		run_one_parse "$sql" "$mt" "$want_rc" "$expect" "$forbid" "$via"
	done
}
if [ ! -x "$bin" ]; then
	n_declared="$(count_parse_declared)"
	[ "$n_declared" -gt 0 ] && artifact_missing "无硬件契约" "$n_declared 条用例" "$bin" "$bot_explicit"
else
	run_parse_cases
fi

# ======================================================================
# controller/trigger 多列：顺序与数据到位（BOT4CJ_DEBUG=1）。
# 正身断言已登记 parse_cases，本节补需要调试开关/行序的三条观察面：
#   trig_basic  触发先于消费、返回表以触发器名为键并入 call 参数 dump、
#               触发器数据是输入侧（controller 转发 payload 不含触发器子表）
#   trig2_basic 两个触发器按列声明序触发（s1 < s2 < sink），返回表各自按名到位
#   ctrl2_basic 扇出广播：payload 构建一次（Forward dump 恰一次），每个
#               controller 各收到同一份数据（各自 Controller result dump）
# ======================================================================
if [ ! -x "$bin" ]; then
	artifact_missing "controller/trigger 多列观察面" "3 条用例" "$bin" "$bot_explicit"
else
	echo "----- trig_basic (trigger 前置调用观察面) -----"
	tout="$(cat scripts/test/trig_basic.mt | BOT4CJ_DEBUG=1 $timeout_cmd "$bin" --robot=robots/test/trig_basic.sql 2>&1)"
	trc=$?
	echo "$tout"
	t_ok=true
	[ "$trc" -ne 0 ] && t_ok=false
	src_line="$(printf '%s\n' "$tout" | grep -nF 'Relay: test=300 note=src' | head -1 | cut -d: -f1)"
	sink_line="$(printf '%s\n' "$tout" | grep -nF 'Relay: test=300 note=sink' | head -1 | cut -d: -f1)"
	if [ -z "$src_line" ] || [ -z "$sink_line" ]; then
		t_ok=false
	elif [ "$src_line" -ge "$sink_line" ]; then
		t_ok=false
	fi
	echo "$tout" | grep -qF '"relay-src":{"delay":300}' || t_ok=false
	echo "$tout" | grep -F 'Forward to controller:' | grep -qF '"relay-src"' && t_ok=false
	if $t_ok; then
		record_pass
	else
		record_fail "trig_basic(trigger 观察面)"
	fi

	echo "----- trig2_basic (trigger 多列观察面) -----"
	tout="$(cat scripts/test/trig2_basic.mt | BOT4CJ_DEBUG=1 $timeout_cmd "$bin" --robot=robots/test/trig2_basic.sql 2>&1)"
	trc=$?
	echo "$tout"
	t2_ok=true
	[ "$trc" -ne 0 ] && t2_ok=false
	s1_line="$(printf '%s\n' "$tout" | grep -nF 'Relay: test=300 note=s1' | head -1 | cut -d: -f1)"
	s2_line="$(printf '%s\n' "$tout" | grep -nF 'Relay: test=301 note=s2' | head -1 | cut -d: -f1)"
	sk_line="$(printf '%s\n' "$tout" | grep -nF 'Relay: test=301 note=sink' | head -1 | cut -d: -f1)"
	if [ -z "$s1_line" ] || [ -z "$s2_line" ] || [ -z "$sk_line" ]; then
		t2_ok=false
	elif [ "$s1_line" -ge "$s2_line" ] || [ "$s2_line" -ge "$sk_line" ]; then
		t2_ok=false
	fi
	echo "$tout" | grep -qF '"relay-src1":{"delay":300}' || t2_ok=false
	echo "$tout" | grep -qF '"relay-src2":{"delay":301}' || t2_ok=false
	if $t2_ok; then
		record_pass
	else
		record_fail "trig2_basic(trigger 多列观察面)"
	fi

	echo "----- ctrl2_basic (controller 多列观察面) -----"
	tout="$(cat scripts/test/ctrl2_basic.mt | BOT4CJ_DEBUG=1 $timeout_cmd "$bin" --robot=robots/test/ctrl2_basic.sql 2>&1)"
	trc=$?
	echo "$tout"
	c2_ok=true
	[ "$trc" -ne 0 ] && c2_ok=false
	# 两个 controller 都被转发（各自的 Controller result dump 带名字）
	echo "$tout" | grep -qF 'Controller result (dummy-0)' || c2_ok=false
	echo "$tout" | grep -qF 'Controller result (timer-b)' || c2_ok=false
	# 扇出广播：payload 只构建一次（同一份数据发给每个 controller）
	[ "$(printf '%s\n' "$tout" | grep -cF 'Forward to controller:')" = "1" ] || c2_ok=false
	if $c2_ok; then
		record_pass
	else
		record_fail "ctrl2_basic(controller 多列观察面)"
	fi
fi

# ======================================================================
# mtmerge 合成器：@Action 相位归并 + @Alias/@Vars 按行叠加（同名报错）。
# 纯离线；最后一条把产物回喂 mybot，证明内联 Alias/Vars 在合并产物里仍生效。
# 缺 ./build/mtmerge 判失败（本段是 mtmerge 的唯一覆盖）。
# ======================================================================
mtmerge_bin="${MTMERGE:-./build/mtmerge}"
mtmerge_explicit=0
[ -n "${MTMERGE:-}" ] && mtmerge_explicit=1
mtmerge_reported=""
# 本节用例数 = run_one_mtmerge 调用点 + 1 条「产物回喂 mybot」E2E（下方内联）。
# 数调用点而非写死数字：加用例忘改文案的漂移在此消除（N-31）。
mtmerge_cases_n=$(( $(count_calls run_one_mtmerge) + 1 ))
run_one_mtmerge() {
	local name="$1" want_rc="$2" expects="$3"; shift 3
	echo "----- $name (mtmerge) -----"
	if [ ! -x "$mtmerge_bin" ]; then
		if [ -z "$mtmerge_reported" ]; then
			mtmerge_reported=1
			artifact_missing "mtmerge 合成器" "$mtmerge_cases_n 条用例" "$mtmerge_bin" "$mtmerge_explicit"
		fi
		return 2
	fi
	local out rc ok=true e
	out="$("$@" 2>&1)"
	rc=$?
	echo "$out"
	[ "$rc" -ne "$want_rc" ] && ok=false
	local IFS='|'
	for e in $expects; do
		echo "$out" | grep -qF -- "$e" || ok=false
	done
	if $ok; then
		record_pass
	else
		record_fail "$name(mtmerge)"
	fi
}
if [ -z "$filter" ] || [[ "mtmerge" == *"$filter"* ]]; then
	mkdir -p .tmp/mtm
	cat > .tmp/mtm/a.mt <<'MTEOF'
@Alias
name alias;
  A  angle;
@

@Vars
name value;
  K  1;
@

@Action
s1.A;
  45;
@
MTEOF
	cat > .tmp/mtm/b.mt <<'MTEOF'
@Alias
name alias;
  B  delay;
@

@Vars
name value;
  L  2;
@

@Action
spk.B;
  200;
@
MTEOF
	cat > .tmp/mtm/dup_alias.mt <<'MTEOF'
@Alias
name alias;
  A  other;
@

@Action
s2.A;
  10;
@
MTEOF
	cat > .tmp/mtm/dup_vars.mt <<'MTEOF'
@Vars
name value;
  K  3;
@

@Action
s2.K;
  10;
@
MTEOF
	cat > .tmp/mtm/multiblock.mt <<'MTEOF'
@Alias
name alias;
  C  freq;
@

@Alias
name alias;
  D  speed;
@

@Action
s3.C;
  1;
@
MTEOF
	cat > .tmp/mtm/robot.sql <<'SQLEOF'
CREATE TABLE dummy (
	name String
);
INSERT INTO dummy (name) VALUES ('dummy-0');

CREATE TABLE Servo (
	name       String,
	max        String,
	min        String,
	offset     String,
	controller String,
	channel    String
);
INSERT INTO Servo (name, max, min, offset, controller, channel) VALUES ('s1', '90', '-', '0', 'dummy-0', '6');
INSERT INTO Servo (name, max, min, offset, controller, channel) VALUES ('s2', '90', '-', '0', '-', '5');

CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('spk');
SQLEOF

	# 正值：Alias/Vars 两侧行都并入，@Action 设备集不相交照常归并
	run_one_mtmerge 'mtmerge_merge' 0 \
		'A angle|B delay|K 1|L 2|s1.A spk.B' \
		"$mtmerge_bin" .tmp/mtm/a.mt .tmp/mtm/b.mt

	# 同一输入内多个 @Alias 块都要并入（不得只看第一个而静默丢弃其余块）
	run_one_mtmerge 'mtmerge_multiblock' 0 \
		'C freq|D speed|s1.A s3.C' \
		"$mtmerge_bin" .tmp/mtm/a.mt .tmp/mtm/multiblock.mt

	# 负值：同名 Alias / Vars 行（跨输入）必须报错退出 2
	run_one_mtmerge 'mtmerge_alias_dup' 2 "duplicate Alias name 'A'" \
		"$mtmerge_bin" .tmp/mtm/a.mt .tmp/mtm/dup_alias.mt
	run_one_mtmerge 'mtmerge_vars_dup' 2 "duplicate Vars name 'K'" \
		"$mtmerge_bin" .tmp/mtm/a.mt .tmp/mtm/dup_vars.mt

	# 端到端：合并产物回喂 mybot，内联 Alias(A->angle, B->delay) 生效
	echo "----- mtmerge_e2e (mtmerge) -----"
	if [ ! -x "$mtmerge_bin" ]; then
		[ -z "$mtmerge_reported" ] && { mtmerge_reported=1; artifact_missing "mtmerge 合成器" "$mtmerge_cases_n 条用例" "$mtmerge_bin" "$mtmerge_explicit"; }
	elif [ ! -x "$bin" ]; then
		artifact_missing "mtmerge 回喂 mybot" "该用例（回喂 E2E）" "$bin" "$bot_explicit"
	else
		out="$( ("$mtmerge_bin" .tmp/mtm/a.mt .tmp/mtm/b.mt | "$bin" --robot=.tmp/mtm/robot.sql) 2>&1 )"
		rc=$?
		echo "$out" | tail -4
		if [ "$rc" -eq 0 ] && echo "$out" | grep -qF 'Dummy served 1 call(s)'; then
			record_pass
		else
			record_fail "mtmerge_e2e(mtmerge)"
		fi
	fi
fi

# ======================================================================
# @Include 归并观察面（parse_cases 只能钉单点断言，这里做多值/计数断言）：
#   include_main   三路设备合并到单时间轴——r1 滞后 1 拍、r2 滞后 2 拍；
#                  两个 include 的全部动作（100/110 与 200/210/220）都在输出里；
#                  相位前导行未起拍的设备不得以 0.0 误发（test=0 禁止出现）；
#                  主脚本自己的 timer-0 正常走完（slept 结果 JSON 在场）。
#   diamond        菱形复用：fa/fb 两臂都包含 shared.once.mt——skip 注记恰好
#                  一条（include-once 生效的直接证据；第二路若未跳过会以
#                  设备集相交报错退出）；三路动作全部在场。
# ======================================================================
run_include_observation() {
	if [ ! -x "$bin" ]; then
		artifact_missing "@Include 归并观察面" "2 条用例" "$bin" "$bot_explicit"
		return 2
	fi
	# ---- include_main ----
	echo "----- include-main (@Include 归并观察面) -----"
	local out rc i_ok v
	out="$(cat scripts/test/include_main.mt | timeout 30 "$bin" --robot=robots/test/include_demo.sql 2>&1)"
	rc=$?
	echo "$out"
	i_ok=true
	[ "$rc" -ne 0 ] && i_ok=false
	for v in 100 110 200 210 220; do
		echo "$out" | grep -qF "test=$v" || i_ok=false
	done
	echo "$out" | grep -qF "test=0" && i_ok=false
	echo "$out" | grep -qF '"slept":200' || i_ok=false
	if $i_ok; then
		record_pass
	else
		record_fail "include-main(@Include 归并)"
	fi
	# ---- diamond ----
	echo "----- include-diamond (include-once 观察面) -----"
	local dout d_ok skip_n
	dout="$(cat scripts/test/include_diamond.mt | timeout 30 "$bin" --robot=robots/test/include_demo.sql 2>&1)"
	rc=$?
	echo "$dout"
	d_ok=true
	[ "$rc" -ne 0 ] && d_ok=false
	for v in 100 200 300; do
		echo "$dout" | grep -qF "test=$v" || d_ok=false
	done
	skip_n="$(echo "$dout" | grep -cF "already included")"
	[ "$skip_n" -eq 1 ] || d_ok=false
	if $d_ok; then
		record_pass
	else
		record_fail "include-diamond(include-once)"
	fi
}
if [ -z "$filter" ] || [[ "include" == *"$filter"* ]]; then
	run_include_observation
fi

# ======================================================================
# TCP 动作源端到端：唯一覆盖 actionsource.cpp（--tcp 监听分支）的守门。
# 复用 scripts/tcp_e2e.sh：起服务端 → Python 客户端下发 relay-test.mt →
# 校验服务端日志。缺编译产物判失败（本段是 actionsource.cpp 监听分支的唯一覆盖）；
# 缺 python3 仍按 README 声明的软依赖跳过并计入汇总。
# 随机高位端口（19000-19999）规避默认 19240 被占/串扰。
# ======================================================================
run_one_tcp() {
	echo "----- tcp-action-source (mybot TCP E2E) -----"
	if [ ! -x "$bin" ]; then
		artifact_missing "TCP 动作源" "该用例" "$bin" "$bot_explicit"
		return 2
	fi
	# python3 是 README 声明的可选软依赖（缺则本段自动跳过），仍计入 skip 供汇总可见
	if ! command -v python3 >/dev/null 2>&1; then
		echo "[SKIP] 无 python3（TCP 客户端），跳过本段"
		record_skip "tcp-action-source(缺python3)"
		return 2
	fi
	local port tcp_rc tcp_out
	port=$((19000 + (RANDOM % 1000)))
	tcp_out="$(BOT="$bin" timeout 40 bash scripts/tcp_e2e.sh "$port" 20 2>&1)"
	tcp_rc=$?
	echo "$tcp_out"
	if [ $tcp_rc -ne 0 ] || ! echo "$tcp_out" | grep -qF "TCP 端到端测试通过"; then
		record_fail "tcp-action-source(TCP)"
	else
		record_pass
	fi
}
if [ -z "$filter" ] || [[ "tcp" == *"$filter"* ]]; then
	run_one_tcp
fi

# ======================================================================
# TCP 异常注入（审计覆盖缺口 #2）：tcp_e2e.sh 只有 happy path，半包/断连/
# 空闲挂死/垃圾帧/口令负路径此前零覆盖。复用 scripts/tcp_adversarial.sh
# 场景矩阵（详见该脚本头注释），每场景独立服务端 + 看门狗——「挂死不退」
# 本身是被断言的故障形态。缺 python3 按 SKIP 计（与 TCP 段同约定）。
# 场景数从该脚本的 run_scenario 调用点数出来（本文件不做第二份计数）。
# ======================================================================
run_one_tcp_adv() {
	echo "----- tcp-adversarial (TCP 异常注入) -----"
	if [ ! -x "$bin" ]; then
		artifact_missing "TCP 异常注入" \
			"$(grep -c '^run_scenario [a-z]' scripts/tcp_adversarial.sh) 个场景" \
			"$bin" "$bot_explicit"
		return 2
	fi
	if ! command -v python3 >/dev/null 2>&1; then
		echo "[SKIP] 无 python3（TCP 客户端），跳过本段"
		record_skip "tcp-adversarial(缺python3)"
		return 2
	fi
	local out rc
	out="$(BOT="$bin" timeout 120 bash scripts/tcp_adversarial.sh 2>&1)"
	rc=$?
	echo "$out"
	if [ $rc -ne 0 ] || ! echo "$out" | grep -qF "TCP 异常注入测试通过"; then
		record_fail "tcp-adversarial(异常注入)"
	else
		record_pass
	fi
}
if [ -z "$filter" ] || [[ "tcp" == *"$filter"* ]]; then
	run_one_tcp_adv
fi

# ======================================================================
# agent 模式端到端（scripts/agent_e2e.sh）：本地假模型服务 + 真实 mybot，五个场景。
#   1) 默认协议驱动（LlmOpenAI）：工具循环真的迭代、动作脚本在**常驻**机器人上
#      被执行、最终回答只走 stdout，并抓取线上报文断言是 OpenAI 形态；
#   2) --llm-driver LlmDeepSeek：断言 thinking / 不发 temperature /
#      历史 reasoning_content 被回传（官方文档里的 400 级有状态要求）；
#   3) --llm-timeout 在 4s 内生效：专防 curl_easy_setopt 退回裸变参调用
#      （那时整型选项被静默忽略，实测会跑满 5s 且可能 rc=0）；
#   4) --llm-config 配置文件：URL/KEY/MODEL 全部来自文件，且环境里不放任何密钥。
#   5) capture_image 拍照：断言线上报文带 image_url 多模态内容（data URL 包真 JPEG）。
#
# 环境软依赖：python3（假模型服务）与 libcurl（agent 模式的运行期依赖，
# Lua 驱动经 ffi 加载）。任一缺失按 SKIP 计入「未执行清单」，不是静默消失；
# 缺编译产物仍判失败。
# ======================================================================
run_one_agent_e2e() {
	echo "----- agent-e2e (agent 模式端到端) -----"
	if [ ! -x "$bin" ]; then
		artifact_missing "agent 模式端到端" "该段（scripts/agent_e2e.sh）" "$bin" "$bot_explicit"
		return 2
	fi
	if ! command -v python3 >/dev/null 2>&1; then
		echo "[SKIP] 无 python3（假模型服务），跳过本段"
		record_skip "agent-e2e(缺python3)"
		return 2
	fi
	local out rc
	out="$(BOT="$bin" timeout 90 bash scripts/agent_e2e.sh 2>&1)"
	rc=$?
	echo "$out"
	if [ $rc -eq 125 ]; then
		record_skip "agent-e2e(缺python3或libcurl)"
	elif [ $rc -ne 0 ] || ! echo "$out" | grep -qF "agent 模式端到端测试通过"; then
		record_fail "agent-e2e(agent 模式)"
	else
		record_pass
	fi
}
if [ -z "$filter" ] || [[ "agent" == *"$filter"* ]]; then
	run_one_agent_e2e
fi

# ======================================================================
# mtmerge 相位语义回归（scripts/mtmerge_e2e.sh）：相位 0/±1 逐格比对、
# 设备集相交报错、三输入链式、负相位丢行告警、合并产物回喂 mybot 时
# 相位前导 "-" 的设备必须被跳过而非 play(0.0)（未起拍语义）。
# 与上面的逐条 mtmerge 用例互补：这里只盯相位边界，缺产物判失败。
# ======================================================================
run_one_mtmerge_phase() {
	echo "----- mtmerge-phase (相位语义) -----"
	if [ ! -x "$mtmerge_bin" ]; then
		artifact_missing "mtmerge 相位回归" "该段（scripts/mtmerge_e2e.sh）" "$mtmerge_bin" "$mtmerge_explicit"
		return 2
	fi
	local out rc
	out="$(MTMERGE="$mtmerge_bin" BOT="$bin" timeout 60 bash scripts/mtmerge_e2e.sh 2>&1)"
	rc=$?
	echo "$out"
	if [ $rc -ne 0 ] || ! echo "$out" | grep -qF "mtmerge 端到端测试通过"; then
		record_fail "mtmerge-phase(相位语义)"
	else
		record_pass
	fi
}
if [ -z "$filter" ] || [[ "mtmerge" == *"$filter"* ]]; then
	run_one_mtmerge_phase
fi

# ======================================================================
# 审计加固回归（#1/#2/#4）：输入源信任边界与守护 setup 失败回滚。
#   tcp_host_bad    --tcp-host 非点分 IPv4 → 清晰报错、退出码非 0
#   action_toolarge 动作脚本超 8MiB 上限 → 拒绝而非把整份输入吃进内存
#                   （防线前实测 100MB 输入 → RSS 4.2GB）
#   daemon_build_raise @Daemon 的 driver 误配成无全局 build 的模块 →
#                   必须走 fail() 回滚（打印 "driver build() raised"），
#                   而非把 Lua error 抛出 chunk 使 C++ 侧跳过 cleanup
#   mode_key_cannot_override_dispatch
#                   上游驱动返回值里的 mode 键不得改写 controller 的派发目标
#                   （N2：mode 由框架独占，必须最后写入）
#   driver_returns_nothing
#                   驱动 call() 漏写 return 不得被当成成功（N3：非表返回即契约违反）
#   driver_returns_cyclic_table
#                   驱动返回自引用表曾是宿主栈溢出裸 SIGSEGV（C-9：转换深度上限，
#                   超限时就地转 ERR——不清栈会让该 lua_State 之后无法复用）
# 夹具写在 .tmp/（已 gitignore），经 BOT4CJ_MODULE_PATH 多路径注入。
# ======================================================================
run_one_hardening() {
	local name="$1" want_rc="$2" expect="$3"; shift 3
	echo "----- $name (hardening) -----"
	if [ ! -x "$bin" ]; then
		# 本段各用例同因（缺 mybot）：只折算一次失败、打印一遍指引
		if [ -z "$hardening_reported" ]; then
			hardening_reported=1
			# 条数从本文件的调用点数出来，避免「加用例忘改数字」的消息漂移
			artifact_missing "输入边界加固" \
				"$(count_calls run_one_hardening) 条用例" \
				"$bin" "$bot_explicit"
		fi
		return 2
	fi
	local out rc h_ok
	out="$("$@" 2>&1)"
	rc=$?
	echo "$out" | tail -6
	h_ok=true
	[ "$rc" -ne "$want_rc" ] && h_ok=false
	echo "$out" | grep -qF -- "$expect" || h_ok=false
	if $h_ok; then
		record_pass
	else
		record_fail "$name(hardening)"
	fi
}

if [ -z "$filter" ] || [[ "hardening" == *"$filter"* ]] || [[ "tcp_host" == *"$filter"* ]]; then
	run_one_hardening 'tcp_host_bad' 1 '--tcp-host 地址无效' \
		timeout 20 "$bin" --robot=robots/relay-test.sql --tcp 19301 --tcp-host 'not-an-ip'

	# 审计 H-2：非回环监听必须设 --tcp-token，缺省即拒绝启动（不再仅告警）
	run_one_hardening 'tcp_noloop_requires_token' 1 '非回环且未设 --tcp-token' \
		timeout 20 "$bin" --robot=robots/relay-test.sql --tcp 19302 --tcp-host 0.0.0.0

	# 审计中-3：裸 --tcp（漏写端口号）明确报错，不再静默回落端口 1 走到 bind 失败
	run_one_hardening 'tcp_no_port' 1 '--tcp 需要端口号' \
		timeout 20 "$bin" --robot=robots/relay-test.sql --tcp

	mkdir -p .tmp
	# 9MiB 合法动作脚本：超过 8MiB 上限，必须在读取阶段就被拒绝
	{ echo '@Action'; echo 'relay0.test;'; yes '    200;'; } 2>/dev/null | head -c 9000000 > .tmp/toolarge.mt
	run_one_hardening 'action_toolarge' 1 'exceeds the byte limit' \
		timeout 30 "$bin" --robot=robots/relay-test.sql --input .tmp/toolarge.mt
	rm -f .tmp/toolarge.mt

	# F-02：结构脚本（.sql）与动作脚本统一 8 MiB 上限——旧实现只有 .mt 限流，
	# 超大 .sql 会把全部 token 驻留内存（长引号值还会按 O(n²) 钉死一核）。
	# 行长压到 ~120 B（token 数 ~76 万），避免守门内存峰值被 sanitizer 放大。
	pad="$(printf 'x%.0s' $(seq 1 100))"
	{ echo 'CREATE TABLE T ( name String );'; yes "INSERT INTO T ( name ) VALUES ('$pad');"; } 2>/dev/null | head -c 9000000 > .tmp/sql_toolarge.sql
	run_one_hardening 'sql_toolarge' 1 'exceeds the byte limit' \
		timeout 30 "$bin" --robot=.tmp/sql_toolarge.sql --input /dev/null
	rm -f .tmp/sql_toolarge.sql

	# F-20：未知/拼错的 --key 不再「打印帮助 + exit 0」——CI 里的假成功出口
	run_one_hardening 'unknown_arg' 2 '未知参数：--robott' \
		timeout 20 "$bin" --robott=robots/relay-test.sql

	mkdir -p .tmp/fixmod
	cat > .tmp/fixmod/leakdrv.lua <<'LUAEOF'
-- 审计 #4 回归夹具：只提供 virt 器件模型，故意不定义全局 build/call/destroy，
-- 复现 @Daemon 的 driver 列误配成「加载成功但没有 build」的模块这一真实坑。
leakdrv_VIRT = { new = function() return {} end }
LUAEOF
	cat > .tmp/fixmod.sql <<'SQLEOF'
CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (
	name     String,
	chunk    String,
	driver   String,
	proto    String,
	addr     String,
	interval String,
	fields   String
);
INSERT INTO Daemon (name, chunk, driver, proto, addr, interval, fields)
  VALUES ('ld0', 'lib/sensor_daemon.lua', 'leakdrv', 'virt', '0x68', '10', 'v0');
SQLEOF
	printf '@Action\nt0.delay;\n   200;\n    50;\n    50;\n@\n' > .tmp/fixmod.mt
	run_one_hardening 'daemon_build_raise' 0 'driver build() raised' \
		env BOT4CJ_MODULE_PATH="./modules:.tmp/fixmod" timeout 30 "$bin" \
		--robot=.tmp/fixmod.sql --input=.tmp/fixmod.mt

	# N2 回归：上游驱动的返回表里混进 mode 键（controller payload 会原样带着它），
	# mode 必须由框架独占、下游派发目标必须仍是 call——否则上游一句话就能让
	# 下游驱动的 call() 完全不执行且退出码仍为 0。锚点取下游自己写出的键。
	cat > .tmp/fixmod/HijUp.lua <<'LUAEOF'
-- 审计 N2 回归夹具：故意在返回值里塞 mode，模拟被带进 controller payload 的键
function build(data) return { result = "OK" } end
function destroy(data) return { result = "OK" } end
function call(data) return { result = "OK", mode = "print", from_up = "yes" } end
LUAEOF
	cat > .tmp/fixmod/HijDown.lua <<'LUAEOF'
function build(data) return { result = "OK" } end
function destroy(data) return { result = "OK" } end
function call(data)
	if data.mode ~= "call" then
		return { result = "ERR", message = "controller dispatch target was overridden: " .. tostring(data.mode) }
	end
	return { result = "OK", down_served = "yes" }
end
LUAEOF
	cat > .tmp/fixmod_hij.sql <<'SQLEOF'
CREATE TABLE HijDown (
	name String
);
INSERT INTO HijDown (name) VALUES ('d0');

CREATE TABLE HijUp (
	name       String,
	controller String
);
INSERT INTO HijUp (name, controller) VALUES ('u0', 'd0');
SQLEOF
	printf '@Action\nu0.x;\n   1;\n@\n' > .tmp/fixmod_hij.mt
	run_one_hardening 'mode_key_cannot_override_dispatch' 0 'down_served' \
		env BOT4CJ_MODULE_PATH="./modules:.tmp/fixmod" timeout 30 "$bin" \
		--robot=.tmp/fixmod_hij.sql --input=.tmp/fixmod_hij.mt

	# N3 回归：call() 漏写 return（返回 0 个值）曾被折成空表、被 ensureOk 当成
	# 成功——硬件没动却报绿。现在必须清晰报错并以非 0 退出。
	cat > .tmp/fixmod/NoRet.lua <<'LUAEOF'
-- 审计 N3 回归夹具：call 少写一行 return，等价于返回「无值」
function build(data) return { result = "OK" } end
function destroy(data) return { result = "OK" } end
function call(data)
	local ignored = data.x
end
LUAEOF
	cat > .tmp/fixmod_noret.sql <<'SQLEOF'
CREATE TABLE NoRet (
	name String
);
INSERT INTO NoRet (name) VALUES ('n0');
SQLEOF
	printf '@Action\nn0.x;\n   1;\n@\n' > .tmp/fixmod_noret.mt
	run_one_hardening 'driver_returns_nothing' 1 "returned no value" \
		env BOT4CJ_MODULE_PATH="./modules:.tmp/fixmod" timeout 30 "$bin" \
		--robot=.tmp/fixmod_noret.sql --input=.tmp/fixmod_noret.mt

	# C-9 回归：驱动返回自引用表（t.self = t，最常见的笔误形态）曾是宿主栈溢出
	# 裸 SIGSEGV（退出码 139，且跳过 deactivate 清理链）——必须变成清晰契约报错。
	cat > .tmp/fixmod/CycRet.lua <<'LUAEOF'
-- 审计 C-9 回归夹具：call 返回一张引用自己的表
function build(data) return { result = "OK" } end
function destroy(data) return { result = "OK" } end
function call(data)
	local t = { result = "OK", v0 = 1.5 }
	t.self = t
	return t
end
LUAEOF
	cat > .tmp/fixmod_cyc.sql <<'SQLEOF'
CREATE TABLE CycRet (
	name String
);
INSERT INTO CycRet (name) VALUES ('c0');
SQLEOF
	printf '@Action\nc0.x;\n   1;\n@\n' > .tmp/fixmod_cyc.mt
	run_one_hardening 'driver_returns_cyclic_table' 1 'nests deeper than' \
		env BOT4CJ_MODULE_PATH="./modules:.tmp/fixmod" timeout 30 "$bin" \
		--robot=.tmp/fixmod_cyc.sql --input=.tmp/fixmod_cyc.mt

	# ---- agent CLI 负路径三连（F-20 同族，main.cpp 的组合校验此前无守门用例，
	# docs/testing.md §9 却已声称覆盖——补齐让声称成真）----
	# 1) agent 专属选项出现在非 agent 模式：意图与参数不一致，显式拒绝退出 2
	run_one_hardening 'agent_opt_outside_agent' 2 '只在 --agent 模式下有效' \
		timeout 20 "$bin" --robot=robots/relay-test.sql --task '让继电器动作一次'

	# 2) --agent 与 --tcp/--input 互斥：两者都要求主导输入源，显式拒绝退出 2
	run_one_hardening 'agent_tcp_mutex' 2 '--agent 与 --tcp/--input 互斥' \
		timeout 20 "$bin" --robot=robots/relay-test.sql --agent --tcp 19399

	# 3) agent 模式缺 API key：buildAgentConfig 失败退出 2。三个密钥环境变量
	#    全清 + --llm-config none（防 CWD 的 mybot.llm.json / ~/.config 污染，
	#    agent_e2e.sh 同款口径），保证「缺 key」与密钥来源无关地成立
	run_one_hardening 'agent_missing_key' 2 '缺少 API key' \
		env -u BOT4CJ_LLM_KEY -u ROBOT_AGENT_API_KEY -u DEEPSEEK_API_KEY \
		timeout 20 "$bin" --robot=robots/relay-test.sql --agent --task 'x' --llm-config none
fi

# ======================================================================
# 守护 tick 期故障注入（审计覆盖缺口 #3）：总线持续 NACK → tick error
# 日志 + count 停滞 + 主流程不受影响（rc=0，仍正常 join/清理）。
# 旧守门只断言「count 达标」（正向路径）；sensor_daemon「坏样本不发布、
# count 停滞即诊断线索」的负向设计从未被 E2E 断言。
# 夹具：NackDrv.lua（.tmp/fixmod，同 hardening 的 BOT4CJ_MODULE_PATH 注入），
# 内嵌 NackDrv_VIRT 模型：virt_tick 拍号 < 8 时正常应答，此后 I2C 写/读恒
# NACK——驱动 call 返回 ERR，守护按 tick 错误处理、不再 publish。
# 断言：rc=0、VIRTUAL HARDWARE 横幅、tick error 日志、count 恰停在 8
# （setup 预武装 virt_tick(0) 后，拍 0~7 共 8 次发布）、count 有多个取值
# （证明前 8 拍确实在发布，而非从头就没发布）、joined 行在位。
# ======================================================================
run_one_daemon_nack() {
	echo "----- daemon_nack (守护 tick 故障注入) -----"
	if [ ! -x "$bin" ]; then
		artifact_missing "守护 tick 故障注入" "该用例" "$bin" "$bot_explicit"
		return 2
	fi
	mkdir -p .tmp/fixmod
	cat > .tmp/fixmod/NackDrv.lua <<'LUAEOF'
-- 守护 tick 故障注入夹具（run_all.sh 生成）：I2C 最小驱动 + 内嵌 virt 模型。
-- 模型按 virt_tick 拍号切换状态：拍号 < 8 正常应答，>= 8 写/读恒 NACK——
-- 用于 E2E 断言「tick 期总线持续 NACK → 驱动 ERR → 守护不 publish（count 停滞）」。
local mpath = dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua")
dofile(mpath.resolve("lib/drv_common.lua"))

local NACK_AFTER = 8

function build(data)
	local ok, ek = arg_table(data)
	if not ok then return ek end
	return { result = "OK" }
end

function call(data)
	local ok, ek = arg_table(data)
	if not ok then return ek end
	local fd, addr, e2 = i2c_pair(data)
	if fd == nil then return e2 end
	local v = i2c_read_multi(fd, 0x00, 2)
	if v < 0 then
		return { result = "ERR", message = "NACK on measurement read" }
	end
	return { result = "OK", v0 = v }
end

function destroy(data)
	local ok, ek = arg_table(data)
	if not ok then return ek end
	return { result = "OK" }
end

NackDrv_VIRT = {
	new = function()
		local n = -1
		return {
			-- 守护经 vdev:virt_tick(k) 冒号调用（带 self）；virtbus 的
			-- dev.write/read 以单参直调（无 self）——签名不可混用
			virt_tick = function(_, k) n = k end,
			write = function(_)
				if n >= NACK_AFTER then return false end
				return true
			end,
			read = function(len)
				if n >= NACK_AFTER then return nil end
				local t = {}
				for i = 1, len do t[i] = (n + 1) % 256 end
				return t
			end,
		}
	end,
}
LUAEOF
	cat > .tmp/nack_robot.sql <<'SQLEOF'
CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (
	name     String,
	chunk    String,
	driver   String,
	proto    String,
	path     String,
	addr     String,
	interval String,
	fields   String
);
INSERT INTO Daemon (name, chunk, driver, proto, path, addr, interval, fields)
  VALUES ('d0', 'lib/sensor_daemon.lua', 'NackDrv', 'virt',
          '/dev/i2c-1', '0x68', '5', 'v0');

/* Monitor 消费守护：mon0.go 每拍调用守护 play()（readSample）——
 * count 快照由此进入输出 JSON，停滞断言才有观测点 */
CREATE TABLE Monitor (
	name String,
	src  String
);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$d0');
SQLEOF
	# ~1.1s 动作（200ms 预热 + 5×200ms），interval=5ms → 足够多拍越过 NACK 门限
	printf '@Action\nt0.delay  mon0.go;\n   200       -    ;\n' > .tmp/nack.mt
	for _ in 1 2 3 4 5; do printf '   200       -    ;\n' >> .tmp/nack.mt; done
	printf '@\n' >> .tmp/nack.mt
	local out rc n_ok count_max count_uniq
	out="$(env BOT4CJ_MODULE_PATH="./modules:.tmp/fixmod" timeout 30 "$bin" \
		--robot=.tmp/nack_robot.sql --input=.tmp/nack.mt 2>&1)"
	rc=$?
	echo "$out"
	n_ok=true
	[ "$rc" -ne 0 ] && n_ok=false
	echo "$out" | grep -qF "VIRTUAL HARDWARE" || n_ok=false
	echo "$out" | grep -qF "tick error" || n_ok=false
	echo "$out" | grep -qF "joined, total publishes=" || n_ok=false
	count_max="$(echo "$out" | grep -oE '"count":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)"
	# count 停滞：前 8 拍正常发布 → 最大恰为 8（容差 6~10 抗时序抖动）。
	# 「发布过且随后停滞」的确定性证据是 join 行的 total publishes（恰等于
	# 正常拍数）——Monitor 200ms 预热后才首次读快照，此时早已停滞，
	# 故输出里只有 count:8 一个值，不能用 uniq 断言「多值」。
	publishes="$(echo "$out" | grep -oE 'total publishes=[0-9]+' | grep -oE '[0-9]+' | tail -1)"
	{ [ -z "$count_max" ] || [ "$count_max" -lt 6 ] || [ "$count_max" -gt 10 ]; } && n_ok=false
	{ [ -z "$publishes" ] || [ "$publishes" -lt 6 ] || [ "$publishes" -gt 10 ]; } && n_ok=false
	if $n_ok; then
		record_pass
	else
		record_fail "daemon_nack(故障注入)"
	fi
}
if [ -z "$filter" ] || [[ "nack" == *"$filter"* ]]; then
	run_one_daemon_nack
fi

# ======================================================================
# 进程级 fd 探针（审计覆盖缺口 #4）：长动作 + 守护持续采样期间，
# 采样 /proc/<pid>/fd 计数——稳态播放期 fd 总数必须有界且平稳，
# 「每拍/每次调用漏关一个 fd」的泄漏会表现为计数单调增长。
# 虚拟总线 fd 不占进程 fd 表（纯内存），此处盯的是真实 fd 消耗点：
# TCP listen/accept、stdin、LuaJIT 内部管道等；Lua 层的精细生命周期
# 由 test_virtbus_fdleak.lua（virtbus.fd_count）补齐。
# ======================================================================
run_one_fd_probe() {
	echo "----- fd-probe (进程级 fd 泄漏探针) -----"
	if [ ! -x "$bin" ]; then
		artifact_missing "fd 泄漏探针" "该用例" "$bin" "$bot_explicit"
		return 2
	fi
	mkdir -p .tmp
	# 复用 signal 段夹具形态：Timer 长动作 + 无硬件守护（daemon_probe 发布链路）
	cat > .tmp/fdprobe_robot.sql <<'SQLEOF'
CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (
	name  String,
	chunk String
);
INSERT INTO Daemon (name, chunk) VALUES ('d0', 'lib/daemon_probe.lua');
SQLEOF
	{ echo '@Action'; echo 't0.delay;'; for _ in $(seq 1 30); do echo '   200;'; done; echo '@'; } > .tmp/fdprobe.mt

	local out pid samples n min max
	samples=""
	"$bin" --robot=.tmp/fdprobe_robot.sql --input=.tmp/fdprobe.mt >/dev/null 2>&1 &
	pid=$!
	# 采样 ~6s（动作时长）：每 250ms 数一次 /proc/<pid>/fd
	for _ in $(seq 1 24); do
		kill -0 "$pid" 2>/dev/null || break
		if [ -d "/proc/$pid/fd" ]; then
			n="$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)"
			samples="$samples $n"
		fi
		sleep 0.25
	done
	wait "$pid"
	local rc=$?
	echo "fd 采样序列:$samples (rc=$rc)"
	min="" ; max=""
	for n in $samples; do
		if [ -z "$min" ] || [ "$n" -lt "$min" ]; then min=$n; fi
		if [ -z "$max" ] || [ "$n" -gt "$max" ]; then max=$n; fi
	done
	if [ "$rc" -eq 0 ] && [ -n "$min" ] && [ -n "$max" ] && [ "$max" -le $((min + 2)) ] && [ "$max" -le 64 ]; then
		record_pass
	else
		record_fail "fd-probe(fd泄漏)"
		echo "[FAIL] fd 计数不平稳或有界性被破坏（min=$min max=$max rc=$rc）"
	fi
}
if [ -z "$filter" ] || [[ "fd" == *"$filter"* ]]; then
	run_one_fd_probe
fi

# ======================================================================
# F-08 信号/退出路径：SIGTERM/SIGINT 只置停止标志，随后必须走正常清理链
# （runAction 停派发 → deactivate → 守护 join → 驱动 destroy），不能裸死。
# 此前 main.cpp 的 sigaction 与 robot.cpp 的停机分支全仓零覆盖。
#   signal_halt  播放中发 SIGTERM（12s 长动作，1s 后打断）
#                → rc=0 + 停派发行 + 「joined, total publishes=」行
#   signal_accept --tcp 等待客户端时发 SIGTERM → rc=1 + 明确中断报错（accept
#                 被 EINTR 打断并检查停止标志的路径）
# 夹具写在 .tmp/（gitignore）：Timer 长动作 + 无硬件守护（lib/daemon_probe.lua）。
# ======================================================================
run_one_signal() {
	local name="$1" want_rc="$2" p1="$3" p2="$4"; shift 4
	echo "----- $name (signal) -----"
	if [ ! -x "$bin" ]; then
		artifact_missing "信号停机" "$(count_calls run_one_signal) 条用例" "$bin" "$bot_explicit"
		return 2
	fi
	local out rc pid watchdog
	out="$("$@" 2>&1 &
		pid=$!
		sleep 1
		kill -TERM "$pid" 2>/dev/null
		# 防挂死看门狗：wait 本身无超时，10s 后 SIGKILL（rc 随之失配 → 判失败），
		# 避免「停机路径回归成死等」把整个守门挂死
		( sleep 10; kill -KILL "$pid" 2>/dev/null ) &
		watchdog=$!
		wait "$pid" 2>/dev/null
		rc=$?
		kill "$watchdog" 2>/dev/null
		echo "__RC__=$rc")"
	rc="$(printf '%s\n' "$out" | sed -n 's/^__RC__=//p')"
	printf '%s\n' "$out" | sed '/^__RC__=/d' | tail -8
	local s_ok=true
	[ "$rc" != "$want_rc" ] && s_ok=false
	printf '%s\n' "$out" | grep -qF -- "$p1" || s_ok=false
	if [ -n "$p2" ]; then
		printf '%s\n' "$out" | grep -qF -- "$p2" || s_ok=false
	fi
	if $s_ok; then
		record_pass
	else
		record_fail "$name(signal)"
	fi
}
if [ -z "$filter" ] || [[ "signal" == *"$filter"* ]]; then
	mkdir -p .tmp
	cat > .tmp/signal_robot.sql <<'SQLEOF'
CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (
	name  String,
	chunk String
);
INSERT INTO Daemon (name, chunk) VALUES ('d0', 'lib/daemon_probe.lua');
SQLEOF
	{ echo '@Action'; echo 't0.delay;'; for _ in $(seq 1 60); do echo '   200;'; done; echo '@'; } > .tmp/signal_long.mt
	run_one_signal 'signal_halt' 0 'Stop requested, halting action dispatch' 'joined, total publishes=' \
		"$bin" --robot=.tmp/signal_robot.sql --input=.tmp/signal_long.mt
	run_one_signal 'signal_accept' 1 'TCP 等待客户端连接被停止信号中断' '' \
		"$bin" --robot=robots/relay-test.sql --tcp $((18000 + RANDOM % 1000))
fi


# ======================================================================
# fixture 孤儿检测（F3/N-32）：robots/test/*.sql 与 scripts/test/*.mt 里没被
# 任何用例表/脚本引用的夹具会静默失效——新写了回归夹具却忘了接进用例表，
# 守门全绿而该夹具从未跑过一次。判据：文件 stem（去扩展名）是否作为**子串**
# 出现在用例表正文 / scripts/*.sh / CMakeLists.txt 中。
# 用子串而非整词：同一器件的多个变体（ads1115.sql 实机版 / ads1115_virt.sql
# 离线版）共享前缀，注册了其一即视为该名字空间已被守门认知；只有全新的、
# 没人提过的名字才会被判为孤儿（这正是「漏登记」的形态）。
# 实机夹具（需 /dev/i2c-*、/dev/spidev*、/dev/gpiochip*、/dev/pwmchip*，
# 离线守门跑不了）在下方白名单显式登记，每个都对应文件头注释的「实机测试」；
# 白名单外出现孤儿即判失败——或接进用例表，或在此声明为实机夹具。
# 与「用例表引用的文件必须存在」互补：这里管漏登记，那里管空引用。
# 只在无过滤器的全量跑里执行（与常量自检同理由）。
# ======================================================================
orphan_whitelist="ad9833 ads1115_diff gpio hal_i2c jit_rows mcp4725 mpu_filter pwm tpl0501"
run_fixture_orphans() {
	echo "----- fixture-orphans (夹具孤儿检测) -----"
	local f stem n=0 orphans=""
	for f in robots/test/*.sql scripts/test/*.mt; do
		stem="$(basename "$f")"; stem="${stem%.*}"
		case " $orphan_whitelist " in *" $stem "*) continue ;; esac
		# 登记来源只看用例表正文（e2e_cases/parse_cases）+ 其它守门脚本 + CMake 目标。
		# 刻意不 grep 本文件全文：注释里顺带提一句名字并不等于接进了表。
		sed -n '/^e2e_cases=(/,/^)/p; /^parse_cases=(/,/^)/p' "$SELF" | grep -qF "$stem" && continue
		grep -qF "$stem" scripts/*.sh CMakeLists.txt 2>/dev/null && continue
		orphans="$orphans $f"
		n=$((n + 1))
	done
	if [ "$n" -eq 0 ]; then
		echo "[PASS] 无孤儿夹具（实机白名单 $(echo $orphan_whitelist | wc -w) 个）"
		record_pass
	else
		echo "[FAIL] $n 个夹具未被任何用例表引用（接进用例表，或加入 orphan_whitelist 声明为实机夹具）："
		for f in $orphans; do echo "       $f"; done
		record_fail "fixture-orphans"
	fi
}

# ======================================================================
# 系统常量自检（F-24）：HAL.lua 硬编码 ioctl/termios 编号与系统头的
# 一致性已在编译期断言（构建失败即漂移）；此处执行期再跑一遍作为
# 双保险。二进制与 mybot_tests 同目录构建（BOT_TESTS 指向 sanitizer
# 构建时同样适用）。只在无过滤器的全量跑里执行——过滤器只匹配用例名，
# 常量自检不受过滤语义管辖，且不能让「拼错过滤器」的空匹配防线失效。
# ======================================================================
run_const_checks() {
	echo "----- const-check (系统常量自检) -----"
	local const_dir="$1" ok=true out
	for prog in test_ioctl_nums test_uart_consts test_v4l2_consts test_curl_consts; do
		local binp="$const_dir/$prog"
		if [ ! -x "$binp" ]; then
			# test_curl_consts 是唯一可选构建项：libcurl 开发头文件不参与
			# mybot 构建（运行期只要 libcurl.so），头文件缺失时该自检不产出
			# 二进制——按跳过计（与缺 python3 同一口径），不判失败；
			# 其余三个无外部依赖，缺产物仍按「构建产物缺失」硬失败。
			if [ "$prog" = "test_curl_consts" ]; then
				echo "[SKIP] $prog 未构建（构建机缺 libcurl 开发头文件）——CURLOPT 常量漂移自检未执行"
				record_skip "const-curl(缺libcurl头文件)"
				continue
			fi
			artifact_missing "系统常量自检" "$prog" "$binp" "$tests_explicit"
			return 2
		fi
		out="$("$binp" 2>&1)"
		if [ $? -ne 0 ]; then
			ok=false
			echo "[FAIL] $prog 执行失败：$out"
		else
			echo "$out"
		fi
	done
	if $ok; then
		record_pass
	else
		record_fail "const-check"
	fi
}

# ======================================================================
# C++ 层单元测试（test/cpp/）：解析器与值语义工具的白盒覆盖，零第三方框架。
# 守门契约与 Lua/E2E 段完全一致：退出码 0 + [DONE] 完成标记 + [PASS] 计数非零
# （三者缺一即判失败，防「用例没跑起来却报绿」）。
# 测试二进制缺省 ./build/mybot_tests，可用 BOT_TESTS=xxx 覆盖。
# 命令行过滤器同样传给 C++ 单测；未命中任何用例时按 SKIP 处理
# （过滤器多为 Lua 用例名，不应因此判红；全仓库无匹配由末尾的空匹配防线兜底）。
# ======================================================================
run_cpp_unit() {
	echo "----- cpp-unit (C++ 单测) -----"
	if [ ! -x "$tests_bin" ]; then
		artifact_missing "C++ 单测" "全部用例" "$tests_bin" "$tests_explicit"
		return 2
	fi
	local out rc pass_n
	if [ -n "$timeout_cmd" ]; then
		out="$($timeout_cmd "$tests_bin" $filter 2>&1)"
	else
		out="$("$tests_bin" $filter 2>&1)"
	fi
	rc=$?
	echo "$out"
	if [ $rc -eq 124 ] && [ -n "$timeout_cmd" ]; then
		echo "[FAIL] C++ 单测超时（>${TEST_TIMEOUT}s，疑似死循环）"
		record_fail "cpp-unit(C++)"
		return
	fi
	if echo "$out" | grep -qF "无匹配用例"; then
		echo "[SKIP] 过滤器 '${filter}' 未命中任何 C++ 用例"
		record_skip "cpp-unit(过滤器未命中)"
		return 2
	fi
	pass_n="$(printf '%s\n' "$out" | grep -cF '[PASS]')"
	if [ "$pass_n" -eq 0 ]; then
		echo "[FAIL] 假绿嫌疑：[PASS] 计数为 0（早退/零断言）"
		record_fail "cpp-unit(C++)"
	elif [ $rc -ne 0 ] || echo "$out" | grep -qF "[FAIL]" || ! echo "$out" | grep -qF "[DONE]"; then
		record_fail "cpp-unit(C++)"
	else
		record_pass
	fi
}
if [ -z "$filter" ]; then
	run_fixture_orphans
	run_const_checks "$(dirname "$tests_bin")"
fi
run_cpp_unit

# 空匹配防线：过滤器拼错时 0 通过 0 失败不得假绿
if [ $((pass + fail)) -eq 0 ]; then
	echo "汇总: 无匹配用例（过滤器 '${filter}' 未命中任何 test/test_*.lua、mybot E2E、无硬件契约回归、TCP E2E、agent E2E、输入边界加固或 C++ 单测，请检查拼写）"
	exit 1
fi
echo "汇总: $pass 通过, $fail 失败, $skip 跳过"
if [ "$skip" -gt 0 ]; then
	echo "未执行:$skipped_list"
fi
if [ $fail -ne 0 ]; then
	echo "失败用例:$failed"
	exit 1
fi
