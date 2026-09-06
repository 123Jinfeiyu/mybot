-- PCF8574.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_pcf8574.lua（PCF8574 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_sgp30.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按准双向语义建模：
--   - 输出锁存（POR 0xFF）+ 外部拉低掩码：引脚读值 = 锁存 & ~拉低
--     （外部低电平恒胜弱上拉；写 0 强驱动亦然——模型同式）
--   - 写帧断言：i2c_write_multi 必须是单字节（无寄存器地址）
--   - 读帧断言：i2c_read 纯读 1 字节

dofile("modules/PCF8574.lua")

local bitc = dofile("modules/bitcompat.lua")

-- ==================== 器件模型 ====================
local latch = 0xFF				-- 输出锁存（POR 默认全 1）
local pull_low = 0				-- 外部拉低掩码（模拟开关按下/开漏设备）
local wr_nak = false
local rd_nak = false
local wr_frames = {}			-- 写帧记录（字节数 + 值）
local rd_frames = 0

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

function i2c_write_multi(fd, val, ...)
	if wr_nak then return -1 end
	local extra = { ... }
	assert(#extra == 0, "PCF8574 has no register address: single-byte write expected")
	local v = math.floor(tonumber(val) or -1)
	assert(v >= 0 and v <= 255, "latch byte out of range")
	wr_frames[#wr_frames + 1] = v
	latch = v
	return 0
end

function i2c_read(fd, n)
	if rd_nak then return -1 end
	assert(n == 1, "pure 1-byte read expected")
	rd_frames = rd_frames + 1
	-- 准双向语义：引脚读值 = 锁存 & ~外部拉低掩码（外部低电平恒胜弱上拉）
	-- bitcompat 无 bnot：8 位反码用 bxor(0xFF) 取
	return bitc.band(latch % 256, bitc.bxor(pull_low % 256, 0xFF))
end

function milli_sleep(ms) end

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

local FD, ADDR = 3, 0x20
local function reset()
	latch = 0xFF
	pull_low = 0
	wr_nak = false
	rd_nak = false
	wr_frames = {}
	rd_frames = 0
end

-- ==================== build ====================
reset()
expect_ok("build 默认（0x20，锁存 0xFF）", function() return build({ fd = FD, address = "0x20" }) end)
check(#wr_frames == 1 and wr_frames[1] == 0xFF, "初始锁存写 0xFF", tostring(#wr_frames))
check(rd_frames == 1, "在位探测读一次", tostring(rd_frames))

reset()
expect_ok("build PCF8574A 地址族（0x38）", function() return build({ fd = FD, address = "0x38" }) end)
reset()
expect_ok("build 地址族上界（0x27）", function() return build({ fd = FD, address = "0x27" }) end)

reset()
expect_ok("build 初始锁存 0x0F", function() return build({ fd = FD, address = "0x20", out = "15" }) end)
check(wr_frames[1] == 0x0F, "锁存写 0x0F", string.format("0x%02X", wr_frames[1] or 0))

reset()
expect_err("build 地址越族（0x30）", function() return build({ fd = FD, address = "0x30" }) end, "invalid address")
expect_err("build 地址越族（0x48）", function() return build({ fd = FD, address = "0x48" }) end, "invalid address")
expect_err("build 初始锁存越界", function() return build({ fd = FD, address = "0x20", out = "256" }) end, "out")
expect_err("build 缺 fd", function() return build({ address = "0x20" }) end, "fd or address")
expect_err("build 非表参数", function() return build(7) end, "Invalid Arguments Type")

reset()
wr_nak = true
expect_err("build 锁存写无应答", function() return build({ fd = FD, address = "0x20" }) end, "not responding")
wr_nak = false
rd_nak = true
expect_err("build 在位读无应答", function() return build({ fd = FD, address = "0x20" }) end, "not responding")
rd_nak = false

-- ==================== call：输出与回读 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x20" }) end)
local res = expect_ok("call 写 0xAA", function() return call({ fd = FD, address = "0x20", out = "170" }) end)
if res then
	check(res.OUT == 0xAA, "OUT=0xAA", string.format("0x%02X", res.OUT))
	check(res.PINS == 0xAA, "PINS=0xAA（无外部负载回读锁存）", string.format("0x%02X", res.PINS))
	check(wr_frames[2] == 0xAA, "写帧单字节 0xAA", string.format("0x%02X", wr_frames[2]))
end

res = expect_ok("call 纯读引脚", function() return call({ fd = FD, address = "0x20" }) end)
if res then
	check(res.PINS == 0xAA and res.OUT == 0xAA, "不写时回读保持", string.format("0x%02X/0x%02X", res.PINS, res.OUT))
end

-- ==================== call：准双向语义 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x20" }) end)
pull_low = 0xF0					-- 高 4 位被外部拉低（开关按下）
res = expect_ok("call 弱上拉被外部拉低", function() return call({ fd = FD, address = "0x20", out = "255" }) end)
if res then
	check(res.OUT == 0xFF, "OUT=0xFF", string.format("0x%02X", res.OUT))
	check(res.PINS == 0x0F, "PINS=0x0F（外部低电平胜弱上拉）", string.format("0x%02X", res.PINS))
end

res = expect_ok("call 强驱动低胜外部", function() return call({ fd = FD, address = "0x20", out = "0" }) end)
if res then
	check(res.PINS == 0x00, "PINS=0x00（写 0 强驱动）", string.format("0x%02X", res.PINS))
end

-- ==================== call：错误路径 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x20" }) end)
expect_err("call out 越界", function() return call({ fd = FD, address = "0x20", out = "256" }) end, "out")
expect_err("call out 负值", function() return call({ fd = FD, address = "0x20", out = "-1" }) end, "out")
expect_err("call 非表参数", function() return call(2) end, "Invalid Arguments")
expect_err("call 缺 address", function() return call({ fd = FD }) end, "fd or address")

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x20" }) end)
wr_nak = true
expect_err("call 写无应答", function() return call({ fd = FD, address = "0x20", out = "1" }) end, "not responding")
wr_nak = false
rd_nak = true
expect_err("call 读无应答", function() return call({ fd = FD, address = "0x20" }) end, "not responding")
rd_nak = false

-- ==================== destroy ====================
reset()
expect_ok("destroy 无关断寄存器", function() return destroy({ fd = FD, address = "0x20" }) end)
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
