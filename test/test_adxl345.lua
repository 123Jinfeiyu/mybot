-- ADXL345.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_adxl345.lua（ADXL345 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ina226.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。寄存器模型覆盖 DEVID 甄别、FULL_RES 数据格式、
-- 速率码、测量启动与小端数据区。

dofile("modules/ADXL345.lua")

local bitc = dofile("modules/bitcompat.lua")

-- ==================== 寄存器模型 ====================
local regs = {}
local devid = 0xE5
local rd8_log = {}
local wr_log = {}
local sleeps = {}
local nak = false
local read_fail = false
local set_addr_fail = false

local function reset_device()
	regs = {
		[0x00] = devid,
		-- 数据区（小端）：X=0x1000(4096) Y=0xFFFF(65535) Z=0xFC00(64512,-1024 原码)
		[0x32] = 0x00, [0x33] = 0x10,
		[0x34] = 0xFF, [0x35] = 0xFF,
		[0x36] = 0x00, [0x37] = 0xFC,
	}
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return not set_addr_fail end

function i2c_write_reg(fd, reg, val)
	if nak then return false end
	val = math.floor(val) % 256
	wr_log[#wr_log + 1] = { reg = reg, val = val }
	regs[reg] = val
	return true
end

function i2c_read_reg(fd, reg)
	if read_fail or nak then return -1 end
	rd8_log[#rd8_log + 1] = reg
	return regs[reg] or 0
end

function i2c_read_multi(fd, reg, n)
	if read_fail or nak then return -1 end
	local v = 0
	for i = 0, n - 1 do v = v * 256 + (regs[reg + i] or 0) end
	return v
end

function milli_sleep(ms) sleeps[#sleeps + 1] = ms end

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

local function wr_of(reg)
	local v
	for _, w in ipairs(wr_log) do
		if w.reg == reg then v = w.val end
	end
	return v
end

local function has_sleep(ms)
	for _, s in ipairs(sleeps) do
		if s == ms then return true end
	end
	return false
end

local FD, ADDR = 3, 0x53

-- ==================== build：默认参数 ====================
reset_device()
wr_log, rd8_log, sleeps = {}, {}, {}
expect_ok("build 默认（±8g/100Hz）", function() return build({ fd = FD, address = "0x53" }) end)
check(rd8_log[1] == 0x00, "首笔读 = DEVID", string.format("0x%02X", rd8_log[1] or 0))
check(wr_of(0x31) == 0x0A, "DATA_FORMAT = FULL_RES|±8g (0x0A)",
	string.format("0x%02X", wr_of(0x31) or 0))
check(wr_of(0x2C) == 0x0A, "BW_RATE = 100Hz (0x0A)", string.format("0x%02X", wr_of(0x2C) or 0))
check(wr_of(0x2D) == 0x08, "POWER_CTL = Measure (0x08)", string.format("0x%02X", wr_of(0x2D) or 0))
check(has_sleep(22), "等两个测量周期 (22ms @100Hz)")

-- ==================== build：列取档 ====================
reset_device()
wr_log = {}
expect_ok("build range='3' odr='70'", function()
	return build({ fd = FD, address = "0x53", range = "3", odr = "70" })
end)
check(wr_of(0x31) == 0x08, "range 3 -> ±2g (FULL_RES|0)", string.format("0x%02X", wr_of(0x31) or 0))
check(wr_of(0x2C) == 0x09, "odr 70 -> 50Hz (0x09)", string.format("0x%02X", wr_of(0x2C) or 0))

reset_device()
wr_log = {}
expect_ok("build range='600' odr='5000'", function()
	return build({ fd = FD, address = "0x53", range = "600", odr = "5000" })
end)
check(wr_of(0x31) == 0x0B, "range 600 -> ±16g (FULL_RES|3)", string.format("0x%02X", wr_of(0x31) or 0))
check(wr_of(0x2C) == 0x0F, "odr 5000 -> 3200Hz (0x0F)", string.format("0x%02X", wr_of(0x2C) or 0))

-- ==================== build：错误通道 ====================
devid = 0xE7
reset_device()
expect_err("build DEVID 不符", function() return build({ fd = FD, address = "0x53" }) end, "DEVID")
devid = 0xE5

nak = true
reset_device()
expect_err("build 器件无应答", function() return build({ fd = FD, address = "0x53" }) end, "Failed")
nak = false

expect_err("build 缺 fd", function() return build({ address = "0x53" }) end, "fd or address")

set_addr_fail = true
expect_err("build set_addr 失败", function() return build({ fd = FD, address = "0x53" }) end, "set_addr failed")
set_addr_fail = false

-- ==================== call：三轴小端拼字 ====================
reset_device()
wr_log = {}
expect_ok("build（call 前置）", function() return build({ fd = FD, address = "0x53" }) end)

local res = expect_ok("call 三轴读值", function() return call({ fd = FD, address = "0x53" }) end)
if res then
	check(res.ACCEL_X == 4096, "ACCEL_X = 0x1000 (LSB 在前)", tostring(res.ACCEL_X))
	check(res.ACCEL_Y == 65535, "ACCEL_Y = 0xFFFF（原始码不转负）", tostring(res.ACCEL_Y))
	check(res.ACCEL_Z == 64512, "ACCEL_Z = 0xFC00 原始码（-1024 补码语义）", tostring(res.ACCEL_Z))
end

read_fail = true
expect_err("call 读失败", function() return call({ fd = FD, address = "0x53" }) end, "Failed to read")
read_fail = false

expect_err("call 缺 fd", function() return call({ address = "0x53" }) end, "fd or address")

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
