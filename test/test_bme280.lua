-- BME280.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_bme280.lua（BME280 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_bme688.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按寄存器文件 + 转换状态机建模：
--   - 校准注入用 datasheet 附录 8.3 示例系数组（T=25.08°C / P≈100653Pa 档），
--     测试内直接用系数常量做参考补偿输出，与驱动的字节装配结果交叉验证
--     （专抓端序/nibble 装配转录笔误）
--   - 桩侧 i2c_read_multi 镜像真 HAL 契约：n>6 显式 -1——守护「合并读越界」
--     回归（BME280 湿度校准块 7 字节曾因此 build 恒 ERR，驱动不可用）
--   - 写 CTRL_MEAS 模式位=1 触发单次转换：busy 6ms 后填充数据区

dofile("modules/BME280.lua")

-- ==================== 校准系数（datasheet 附录 8.3 示例组） ====================
local CAL = {
	T1 = 27504, T2 = 26435, T3 = -1000,
	P1 = 36477, P2 = -10685, P3 = 3024, P4 = 2855, P5 = 140,
	P6 = -7, P7 = 15500, P8 = -14600, P9 = 6000,
	H1 = 260, H2 = 382, H3 = 0, H4 = 350, H5 = 0, H6 = 30,
}

-- ==================== 器件模型 ====================
local regs = {}
local adc_T, adc_P, adc_H = 519888, 415148, 30000
local busy_until = nil
local nvm_stuck = false
local meas_stuck = false
local now_ms = 0
local wr_nak, rd_nak = false, false

local function put16le(reg, v)
	v = v % 65536
	regs[reg] = v % 256
	regs[reg + 1] = math.floor(v / 256)
end

local function inject_regs()
	regs[0xD0] = 0x60
	put16le(0x88, CAL.T1); put16le(0x8A, CAL.T2); put16le(0x8C, CAL.T3)
	put16le(0x8E, CAL.P1); put16le(0x90, CAL.P2); put16le(0x92, CAL.P3)
	put16le(0x94, CAL.P4); put16le(0x96, CAL.P5); put16le(0x98, CAL.P6)
	put16le(0x9A, CAL.P7); put16le(0x9C, CAL.P8); put16le(0x9E, CAL.P9)
	regs[0xA1] = CAL.H1
	put16le(0xE1, CAL.H2)
	regs[0xE3] = CAL.H3
	-- H4=s12(E4<<4|E5低4位) H5=s12(E6<<4|E5高4位)：按位场逆向装配 E4/E5/E6
	regs[0xE4] = math.floor(CAL.H4 / 16) % 256
	regs[0xE6] = math.floor(CAL.H5 / 16) % 256
	regs[0xE5] = (CAL.H5 % 16) * 16 + CAL.H4 % 16
	regs[0xE7] = CAL.H6 % 256
end

local function fill_data()
	-- 数据区 0xF7~0xFE：P(20bit) T(20bit) H(16bit)，MSB 对齐
	regs[0xF7] = math.floor(adc_P / 4096) % 256
	regs[0xF8] = math.floor(adc_P / 16) % 256
	regs[0xF9] = (adc_P % 16) * 16
	regs[0xFA] = math.floor(adc_T / 4096) % 256
	regs[0xFB] = math.floor(adc_T / 16) % 256
	regs[0xFC] = (adc_T % 16) * 16
	regs[0xFD] = math.floor(adc_H / 256) % 256
	regs[0xFE] = adc_H % 256
end

local function advance()
	if busy_until ~= nil and now_ms >= busy_until then
		busy_until = nil
		fill_data()
	end
end

-- 状态寄存器 0xF3 按模型状态合成：bit0=NVM 拷贝中，bit3=测量中
local function read_reg(reg)
	if reg == 0xF3 then
		local st = 0
		if nvm_stuck then st = st + 0x01 end
		if busy_until ~= nil then st = st + 0x08 end
		return st
	end
	return regs[reg] or 0
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

function i2c_write_reg(fd, reg, val)
	if wr_nak then return false end
	val = math.floor(val) % 256
	regs[reg] = val
	if reg == 0xF4 and val % 4 == 1 then			-- forced 模式触发单次测量
		busy_until = meas_stuck and math.huge or (now_ms + 6)
	end
	return true
end

-- 镜像真 HAL 契约：合并读上限 6（2^53 精度），越界显式 -1（回归守护点）
function i2c_read_multi(fd, reg, n)
	if rd_nak then return -1 end
	if n == nil or n < 1 or n > 6 then return -1 end
	advance()
	local v = 0
	for i = 0, n - 1 do v = v * 256 + read_reg(reg + i) end
	return v
end

function i2c_read_multi_bytes(fd, reg, n)
	if rd_nak then return nil end
	if n == nil then return nil end
	n = math.max(1, math.min(16, n))
	advance()
	local t = {}
	for i = 0, n - 1 do t[i + 1] = read_reg(reg + i) end
	return t
end

function milli_sleep(ms) now_ms = now_ms + ms end

-- ==================== 参考补偿（测试内独立转录 datasheet 附录 8.3 浮点版） ====================
local function ref_temperature(adc)
	local var1 = (adc / 16384.0 - CAL.T1 / 1024.0) * CAL.T2
	local var2 = (adc / 131072.0 - CAL.T1 / 8192.0) ^ 2 * CAL.T3
	local t_fine = var1 + var2
	return t_fine / 5120.0, t_fine
end

local function ref_pressure(adc, t_fine)
	local var1 = t_fine / 2.0 - 64000.0
	local var2 = var1 * var1 * CAL.P6 / 32768.0
	var2 = var2 + var1 * CAL.P5 * 2.0
	var2 = var2 / 4.0 + CAL.P4 * 65536.0
	var1 = (CAL.P3 * var1 * var1 / 524288.0 + CAL.P2 * var1) / 524288.0
	var1 = (1.0 + var1 / 32768.0) * CAL.P1
	local p = 1048576.0 - adc
	if var1 ~= 0 then
		p = (p - var2 / 4096.0) * 6250.0 / var1
		local w1 = CAL.P9 * p * p / 2147483648.0
		local w2 = p * CAL.P8 / 32768.0
		p = p + (w1 + w2 + CAL.P7) / 16.0
	end
	return p
end

local function ref_humidity(adc, t_fine)
	local h = t_fine - 76800.0
	h = (adc - (CAL.H4 * 64.0 + CAL.H5 / 16384.0 * h))
		* (CAL.H2 / 65536.0 * (1.0 + CAL.H6 / 67108864.0 * h
			* (1.0 + CAL.H3 / 67108864.0 * h)))
	h = h * (1.0 - CAL.H1 * h / 524288.0)
	return math.max(0.0, math.min(100.0, h))
end

-- ==================== 断言辅助 ====================
local fails = 0
local function check(cond, label, extra)
	if cond then
		print(string.format("[PASS] %s", label))
	else
		fails = fails + 1
		print(string.format("[FAIL] %s%s", label, extra and (" (" .. tostring(extra) .. ")") or ""))
	end
end

local function near(a, b, rel)
	return math.abs(a - b) <= (rel or 1e-6) * math.max(1.0, math.abs(a), math.abs(b))
end

local function result_of(ok, res)
	if ok and type(res) == "table" then return tostring(res.result) end
	return "EXC:" .. tostring(res)
end

local function expect_ok(label, fn)
	local ok, res = pcall(fn)
	check(result_of(ok, res) == "OK", label, result_of(ok, res))
	return ok and res or nil
end

local function expect_err(label, fn, msgpat)
	local ok, res = pcall(fn)
	local msg = type(res) == "table" and tostring(res.message) or tostring(res)
	check(result_of(ok, res) == "ERR" and msg:find(msgpat, 1, true) ~= nil,
		label, msg)
end

local FD, ADDR = 3, 0x76
local function reg(a) return regs[a] end

local function reset()
	regs = {}
	adc_T, adc_P, adc_H = 519888, 415148, 30000
	busy_until = nil
	nvm_stuck = false
	meas_stuck = false
	now_ms = 0
	wr_nak, rd_nak = false, false
	inject_regs()
end

-- ==================== call 未 build ====================
reset()
expect_err("call 未 build", function() return call({ fd = FD, address = "0x76" }) end, "not initialized")

-- ==================== build：配置与寄存器 ====================
reset()
expect_ok("build 默认", function() return build({ fd = FD, address = "0x76" }) end)
check(reg(0xF2) == 0x01, "CTRL_HUM=0x01（湿度 x1）", string.format("0x%02X", reg(0xF2) or 0))
check(reg(0xF4) == 0x24, "CTRL_MEAS=0x24（T/P x1，睡眠）", string.format("0x%02X", reg(0xF4) or 0))
check(reg(0xF5) == 0x00, "CONFIG=0x00（滤波关闭）", string.format("0x%02X", reg(0xF5) or 0))

-- ==================== build：错误路径 ====================
reset()
regs[0xD0] = 0x58
expect_err("build chip id 错误（BMP280?）", function() return build({ fd = FD, address = "0x76" }) end, "chip id")
reset()
nvm_stuck = true
expect_err("build NVM 拷贝超时", function() return build({ fd = FD, address = "0x76" }) end, "copy timeout")
check(now_ms >= 50, "超时前轮询 50ms", tostring(now_ms))
reset()
wr_nak = true
expect_err("build 写无应答（回读配置不一致）", function() return build({ fd = FD, address = "0x76" }) end, "configure")
wr_nak = false
rd_nak = true
expect_err("build 读无应答", function() return build({ fd = FD, address = "0x76" }) end, "chip id")
rd_nak = false
expect_err("build 缺 fd", function() return build({ address = "0x76" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = FD }) end, "fd or address")
expect_err("build 非表参数", function() return build(0) end, "Invalid Arguments Type")

-- ==================== call：T/P/H 与独立公式交叉验证 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x76" }) end)
local res = expect_ok("call 三通道补偿", function() return call({ fd = FD, address = "0x76" }) end)
if res then
	local refT, t_fine = ref_temperature(adc_T)
	local refP = ref_pressure(adc_P, t_fine)
	local refH = ref_humidity(adc_H, t_fine)
	check(refT > 20 and refT < 30 and refP > 95000 and refP < 105000
		and refH > 0 and refH < 100, "参考值落在未钳位合理区间",
		string.format("T=%s P=%s H=%s", tostring(refT), tostring(refP), tostring(refH)))
	check(near(res.T, refT), "T 与独立公式一致", string.format("%s vs %s", tostring(res.T), tostring(refT)))
	check(near(res.P, refP), "P 与独立公式一致", string.format("%s vs %s", tostring(res.P), tostring(refP)))
	check(near(res.H, refH), "H 与独立公式一致", string.format("%s vs %s", tostring(res.H), tostring(refH)))
end

-- ==================== call：无效读数与超时 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x76" }) end)
adc_T = 0x80000
expect_err("call 无效温度读数（0x80000）", function() return call({ fd = FD, address = "0x76" }) end, "temperature")
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x76" }) end)
adc_H = 0x8000
expect_err("call 无效湿度读数（0x8000）", function() return call({ fd = FD, address = "0x76" }) end, "humidity")
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x76" }) end)
meas_stuck = true
expect_err("call 测量超时", function() return call({ fd = FD, address = "0x76" }) end, "timeout")
check(now_ms >= 105, "超时前轮询 105ms（5ms 首等 + 100ms）", tostring(now_ms))
expect_err("call 非表参数", function() return call(3) end, "Invalid Arguments")
expect_err("call 缺 address", function() return call({ fd = FD }) end, "fd or address")

-- ==================== destroy ====================
reset()
expect_ok("destroy 前 build", function() return build({ fd = FD, address = "0x76" }) end)
expect_ok("destroy 置睡眠", function() return destroy({ fd = FD, address = "0x76" }) end)
check(reg(0xF4) % 4 == 0, "CTRL_MEAS 模式位清零（睡眠）", tostring(reg(0xF4)))

reset()
expect_ok("destroy 缺 fd 安全返回", function() return destroy({}) end)
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
