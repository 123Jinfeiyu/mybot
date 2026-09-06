-- AMG8833.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_amg8833.lua（AMG8833 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ina226.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。像素区按器件位域反向编码（12 位补码小端），聚合断言
-- 覆盖负温/热点定位/均值；块读指针序列断言 8 次 16 字节对齐。

dofile("modules/AMG8833.lua")

-- ==================== 寄存器模型 ====================
local regs = {}
local rd_bytes_log = {}		-- read_multi_bytes 记录 {reg, n}
local addr_log = {}			-- set_addr 记录
local wr_log = {}
local sleeps = {}
local nak = false
local read_fail = false
local set_addr_fail = false

-- 12 位补码小端编码：像素 v（-2048..2047 码 -> 0.25°C/LSB）
local function enc12(v)
	local raw = v % 4096
	return raw % 256, math.floor(raw / 256)
end

local function reset_device(pixels, therm)
	regs = { [0x0E] = therm % 256, [0x0F] = math.floor(therm / 256) }
	for i = 0, 63 do
		local lsb, msb = enc12(pixels[i])
		regs[0x80 + i * 2] = lsb
		regs[0x81 + i * 2] = msb
	end
end

local function default_pixels()
	local p = {}
	for i = 0, 63 do p[i] = 80 end		-- 80 码 = 20°C
	p[0] = -20							-- -20 码 = -5°C（负温像素，角落）
	p[27] = 171							-- 171 码 = 42.75°C（热点，行 3 列 3）
	p[63] = 40							-- 40 码 = 10°C（另一角）
	return p
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr)
	if set_addr_fail then return false end
	addr_log[#addr_log + 1] = addr
	return true
end

function i2c_write_reg(fd, reg, val)
	if nak then return false end
	wr_log[#wr_log + 1] = { reg = reg, val = math.floor(val) % 256 }
	return true
end

function i2c_read_multi_bytes(fd, reg, n)
	if read_fail or nak then return nil end
	rd_bytes_log[#rd_bytes_log + 1] = { reg = reg, n = n }
	local t = {}
	for i = 0, n - 1 do t[i + 1] = regs[reg + i] or 0 end
	return t
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

local FD = 7

-- ==================== build：默认参数 ====================
reset_device(default_pixels(), 400)		-- 热敏 400 码 = 25°C
wr_log, rd_bytes_log, addr_log, sleeps = {}, {}, {}, {}
expect_ok("build 默认（0x69/10Hz）", function() return build({ fd = FD }) end)
check(wr_of(0x00) == 0x00, "PCTL = normal", string.format("0x%02X", wr_of(0x00) or 0))
check(wr_of(0x01) == 0x3F, "RST = 初始复位 0x3F", string.format("0x%02X", wr_of(0x01) or 0))
check(wr_of(0x03) == 0x00, "INTC = 中断关闭", string.format("0x%02X", wr_of(0x03) or 0))
check(wr_of(0x02) == 0x00, "FPSC = 10Hz", string.format("0x%02X", wr_of(0x02) or 0))
check(has_sleep(120), "等一个帧周期 (120ms @10Hz)")
check(addr_log[1] == 0x69, "地址列缺省 0x69", string.format("0x%02X", addr_log[1] or 0))

-- ==================== build：fps='1' 与显式地址 ====================
reset_device(default_pixels(), 400)
wr_log, sleeps, addr_log = {}, {}, {}
expect_ok("build fps='1' addr='0x68'", function()
	return build({ fd = FD, address = "0x68", fps = "1" })
end)
check(wr_of(0x02) == 0x01, "FPSC = 1Hz (0x01)", string.format("0x%02X", wr_of(0x02) or 0))
check(has_sleep(1020), "等一个帧周期 (1020ms @1Hz)")
check(addr_log[1] == 0x68, "显式地址 0x68", string.format("0x%02X", addr_log[1] or 0))

-- ==================== call：像素聚合与热点定位 ====================
reset_device(default_pixels(), 400)
wr_log = {}
rd_bytes_log = {}
local res = expect_ok("call 像素聚合", function() return call({ fd = FD }) end)
if res then
	check(near(res.MAXT, 42.75), "MAXT = 42.75°C（171 码 x 0.25）", tostring(res.MAXT))
	check(near(res.MINT, -5.0), "MINT = -5°C（12 位补码负温）", tostring(res.MINT))
	check(near(res.AVGT, 5071 * 0.25 / 64), "AVGT = 5071 码 x 0.25 / 64 = 19.80859375°C", tostring(res.AVGT))
	check(res.MAXR == 3 and res.MAXC == 3, "热点定位 [3][3]（像素 27）",
		string.format("%d,%d", res.MAXR, res.MAXC))
	check(near(res.THERM, 25.0), "THERM = 25°C（400 码 x 0.0625）", tostring(res.THERM))
	check(#rd_bytes_log == 9, "读事务 9 次（热敏 1 + 像素 8）", tostring(#rd_bytes_log))
	check(rd_bytes_log[1].reg == 0x0E and rd_bytes_log[1].n == 2, "首笔 = 热敏电阻 2 字节")
	check(rd_bytes_log[2].reg == 0x80 and rd_bytes_log[2].n == 16, "像素块 1 指针 0x80 x 16 字节")
	check(rd_bytes_log[9].reg == 0xF0 and rd_bytes_log[9].n == 16, "像素块 8 指针 0xF0 x 16 字节")
end

-- ==================== call：全负温阵列（极值交换） ====================
local cold = {}
for i = 0, 63 do cold[i] = -100 end		-- 全 -25°C
cold[40] = 20								-- 行 5 列 0 唯一正温 5°C（20 码）
reset_device(cold, 300)
rd_bytes_log = {}
res = expect_ok("call 全负温阵列", function() return call({ fd = FD }) end)
if res then
	check(near(res.MAXT, 5.0), "MAXT = 5°C（像素 40）", tostring(res.MAXT))
	check(near(res.MINT, -25.0), "MINT = -25°C", tostring(res.MINT))
	check(res.MAXR == 5 and res.MAXC == 0, "热点定位 [5][0]",
		string.format("%d,%d", res.MAXR, res.MAXC))
end

-- ==================== build/call：错误通道 ====================
reset_device(default_pixels(), 400)
nak = true
expect_err("build 器件无应答", function() return build({ fd = FD }) end, "Failed to write")
nak = false

reset_device(default_pixels(), 400)
read_fail = true
expect_err("call 热敏读失败", function() return call({ fd = FD }) end, "thermistor")
read_fail = false

expect_err("build 缺 fd", function() return build({ address = "0x69" }) end, "Invalid Argument: fd")

set_addr_fail = true
expect_err("build set_addr 失败", function() return build({ fd = FD }) end, "set_addr failed")
set_addr_fail = false

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
