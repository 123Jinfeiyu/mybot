-- MCP4725.lua 命令帧离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_mcp4725.lua（MCP4725 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ina226.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。i2c_write_multi 桩逐字节捕获命令帧（快速写 2 字节 /
-- EEPROM 写 3 字节），i2c_read 桩按脚本回读（读回 5 字节 / RDY 轮询 1 字节）。

dofile("modules/MCP4725.lua")

-- ==================== I2C 桩 ====================
local frames = {}			-- 写帧记录（字节表）
local read_script = {}		-- i2c_read 按序回吐的合并值
local read_feed_i = 0
local wr_fail = false
local read_fail = false
local sleeps = {}

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

function i2c_write_multi(fd, reg, ...)
	if wr_fail then return -1 end
	local f = { reg, ... }
	frames[#frames + 1] = f
	return 0
end

function i2c_read(fd, n)
	if read_fail then return -1 end
	read_feed_i = read_feed_i + 1
	local v = read_script[read_feed_i]
	if v == nil then return -1 end
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

local function near(a, b, eps)
	return math.abs(a - b) <= (eps or 1e-9)
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

local function frame_hex(f)
	local t = {}
	for i, b in ipairs(f or {}) do t[i] = string.format("0x%02X", b) end
	return table.concat(t, ",")
end

local function seq_is(label, want)
	local same = #frames == #want
	for i = 1, math.min(#frames, #want) do
		if #frames[i] ~= #want[i] then same = false; break end
		for j = 1, #want[i] do
			if frames[i][j] ~= want[i][j] then same = false; break end
		end
	end
	check(same, label, frames and frame_hex(frames[1]) or "")
end

local FD = 7

-- ==================== build：读回在位验证 ====================
-- 读回：b0=0x00 b1=0x80 b2=0x00（DAC=2048）+ b3=0x08 b4=0x00（EEPROM=2048）
-- 合并值 = b0<<40 | b3<<24 | b1<<8 | b2（b4=0）
read_script = { 0x00 * 1099511627776 + 0x08 * 16777216 + 0x80 * 256 + 0x00 }
expect_ok("build 读回在位验证", function() return build({ fd = FD }) end)
check(#frames == 0, "build 只读不写", tostring(#frames))

read_fail = true
expect_err("build 读失败", function() return build({ fd = FD }) end, "Failed to read")
read_fail = false

expect_err("build 缺 fd", function() return build({ address = "0x62" }) end, "Invalid Argument: fd")

-- ==================== call：快速写 ====================
frames = {}
local res = expect_ok("call percent='50'（快速写）", function()
	return call({ fd = FD, percent = "50" })
end)
if res then
	check(res.level == 2048 and res.eeprom == 0, "code = floor(2047.5+0.5) = 2048",
		tostring(res.level))
end
seq_is("快速写 2 字节帧 [0x08, 0x00]", { { 0x08, 0x00 } })

frames = {}
res = expect_ok("call volt='2.5'（vref=5）", function()
	return call({ fd = FD, volt = "2.5" })
end)
if res then check(res.level == 2048, "volt 换算 = 4096/2 = 2048", tostring(res.level)) end

frames = {}
res = expect_ok("call volt='5.0'（钳满码）", function()
	return call({ fd = FD, volt = "5.0" })
end)
if res then check(res.level == 4095, "volt=vref 钳 4095", tostring(res.level)) end
seq_is("满码快速写 [0x0F, 0xFF]", { { 0x0F, 0xFF } })

frames = {}
res = expect_ok("call level='-100'（负值钳 0）", function()
	return call({ fd = FD, level = "-100" })
end)
if res then check(res.level == 0, "level 钳 0", tostring(res.level)) end
seq_is("零码快速写 [0x00, 0x00]", { { 0x00, 0x00 } })

-- ==================== call：EEPROM 写 + RDY 轮询 ====================
-- mock 读脚本：1 次 RDY 忙（b0=0x00）+ 1 次 RDY 完成（b0=0x80）
frames = {}
read_feed_i = 0
read_script = { 0x00, 0x80 }
res = expect_ok("call percent='50' eep='1'", function()
	return call({ fd = FD, percent = "50", eep = "1" })
end)
if res then
	check(res.eeprom == 1, "eeprom=1 回显", tostring(res.eeprom))
end
seq_is("EEPROM 写 3 字节帧 [0x60, 0x80, 0x00]", { { 0x60, 0x80, 0x00 } })
check(#sleeps >= 1, "RDY 轮询带 50ms 步进", tostring(#sleeps))

-- 超时：永远忙
frames = {}
read_feed_i = 0
read_script = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
expect_err("call EEPROM 写超时", function()
	return call({ fd = FD, percent = "50", eep = "1" })
end, "timeout")

-- ==================== call：无参读回 ====================
-- b0=0x00（RDY=1? bit7=0 -> rdy=0；pd=0）b1=0x80 b2=0x00（DAC=2048）
-- b3=0x08 b4=0x00（EEPROM=2048）
frames = {}
read_feed_i = 0
read_script = { 0x00 * 256 ^ 4 + 0x80 * 256 ^ 3 + 0x08 * 256 }
res = expect_ok("call 无参读回", function() return call({ fd = FD }) end)
if res then
	check(res.level == 2048, "读回 DAC = 2048", tostring(res.level))
	check(res.eeprom == 2048, "读回 EEPROM = 2048（b3&0xF<<8|b4）", tostring(res.eeprom))
	check(res.pd == 0, "掉电位 = 0", tostring(res.pd))
	check(near(res.volt, 2048 * 5.0 / 4096), "volt 换算 = 2.5V", tostring(res.volt))
	check(#frames == 0, "读回不写字", tostring(#frames))
end

-- ==================== call：错误通道 ====================
wr_fail = true
frames = {}
expect_err("call 快速写失败", function() return call({ fd = FD, percent = "50" }) end, "Failed to write")
wr_fail = false

expect_err("call 缺 fd", function() return call({ percent = "50" }) end, "Invalid Argument: fd")

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
