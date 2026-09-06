-- LPS22HB.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_lps22hb.lua（LPS22HB 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_sgp30.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按寄存器文件 + 单次转换状态机建模：
--   - WHO_AM_I 可注入错值；SWRESET 写后自清零（立即）
--   - CTRL_REG2 ONE_SHOT 写入 -> busy 至精确 3ms（fake 时钟），完成后
--     ONE_SHOT 自清零并按数据手册场格式（小端）填充 0x28~0x2C
--   - 气压/温度原始码可注入（含负值补码），期望 P/T 独立换算交叉验证

dofile("modules/LPS22HB.lua")

-- ==================== 器件模型 ====================
local regs = {}
local id_reg = 0xB1
local pres_raw, temp_raw = 4149248, 2345		-- 1013.0 hPa / 23.45 °C
local busy_until = nil
local stuck = false
local now_ms = 0
local wr_nak = false
local rd_nak = false

local function inject()
	regs = { [0x0F] = id_reg, [0x10] = 0x00, [0x11] = 0x00 }
end

local function fill_field()
	-- 小端：0x28=XL, 0x29=L, 0x2A=H；0x2B=TEMP_L, 0x2C=TEMP_H
	local p = pres_raw % 0x1000000
	regs[0x28] = p % 256
	regs[0x29] = math.floor(p / 256) % 256
	regs[0x2A] = math.floor(p / 65536) % 256
	local t = temp_raw % 0x10000
	regs[0x2B] = t % 256
	regs[0x2C] = math.floor(t / 256) % 256
end

local function advance()
	if busy_until ~= nil and not stuck and now_ms >= busy_until then
		busy_until = nil
		fill_field()
		regs[0x11] = regs[0x11] - regs[0x11] % 2		-- ONE_SHOT 自清零
	end
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

function i2c_write_reg(fd, reg, val)
	if wr_nak then return false end		-- HAL 返回布尔，桩必须一致
	val = math.floor(val) % 256
	if reg == 0x11 and math.floor(val / 4) % 2 == 1 then
		regs[0x11] = 0					-- SWRESET：位自清零（全寄存器回 POR）
		regs[0x10] = 0
	elseif reg == 0x11 and val % 2 == 1 then
		regs[0x11] = val					-- ONE_SHOT 置位
		busy_until = now_ms + 3
	else
		regs[reg] = val
	end
	return true
end

function i2c_read_reg(fd, reg)
	if rd_nak then return -1 end
	advance()
	return regs[reg] or 0
end

function i2c_read_multi(fd, reg, n)
	if rd_nak then return -1 end
	assert(n == 5, "burst read of 5 bytes expected")
	assert(reg == 0xA8, "auto-increment address 0x28|0x80 expected, got " .. tostring(reg))
	advance()
	local v = 0
	for i = 0, n - 1 do v = v * 256 + (regs[0x28 + i] or 0) end
	return v
end

function milli_sleep(ms) now_ms = now_ms + ms end

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

local FD, ADDR = 3, 0x5D
local function reset()
	id_reg = 0xB1
	pres_raw, temp_raw = 4149248, 2345
	busy_until = nil
	stuck = false
	now_ms = 0
	wr_nak = false
	rd_nak = false
	inject()
end

-- ==================== build ====================
reset()
expect_ok("build 默认", function() return build({ fd = FD, address = "0x5D" }) end)
check(regs[0x10] == 0x02, "CTRL_REG1=0x02（BDU + 单次模式）", string.format("0x%02X", regs[0x10]))
check(math.floor(regs[0x11] / 4) % 2 == 0, "SWRESET 已自清零", tostring(regs[0x11]))

reset()
id_reg = 0xBD
inject()
expect_err("build LPS25HB 拦下", function() return build({ fd = FD, address = "0x5D" }) end, "0xBD")
id_reg = 0x99
inject()
expect_err("build WHO_AM_I 错误", function() return build({ fd = FD, address = "0x5D" }) end, "unexpected WHO_AM_I")
id_reg = 0xB1

reset()
wr_nak = true
expect_err("build 写无应答", function() return build({ fd = FD, address = "0x5D" }) end, "not responding")
wr_nak = false
rd_nak = true
expect_err("build 读无应答", function() return build({ fd = FD, address = "0x5D" }) end, "not responding")
rd_nak = false
expect_err("build 缺 fd", function() return build({ address = "0x5D" }) end, "fd or address")
expect_err("build 非表参数", function() return build(8) end, "Invalid Arguments Type")

-- ==================== call：换算（独立算式交叉验证） ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x5D" }) end)
local res = expect_ok("call 标准大气压", function() return call({ fd = FD, address = "0x5D" }) end)
if res then
	check(near(res.P, 4149248 / 4096.0), "P=1013.0 hPa", tostring(res.P))
	check(near(res.T, 2345 / 100.0), "T=23.45 °C", tostring(res.T))
	check(now_ms >= 3, "等待单次转换完成", tostring(now_ms))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x5D" }) end)
pres_raw, temp_raw = 1066496, 65436		-- 260.375 hPa / 负温 -1.00 °C（16 位补码 65436）
res = expect_ok("call 负温度补码", function() return call({ fd = FD, address = "0x5D" }) end)
if res then
	check(near(res.T, -1.0), "T=-1.00 °C（16 位补码）", tostring(res.T))
	check(near(res.P, 1066496 / 4096.0), "P=260.375 hPa", tostring(res.P))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x5D" }) end)
pres_raw = 0xFFF000						-- 负气压 -1 hPa（补码路径，物理上仅测试解码）
temp_raw = 0
res = expect_ok("call 气压补码路径", function() return call({ fd = FD, address = "0x5D" }) end)
if res then
	check(near(res.P, -1.0), "P=-1 hPa（24 位补码）", tostring(res.P))
end

-- ==================== call：超时与错误路径 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x5D" }) end)
stuck = true
expect_err("call 转换超时", function() return call({ fd = FD, address = "0x5D" }) end, "not ready")
check(now_ms >= 50, "超时前轮询 50ms", tostring(now_ms))
stuck = false

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x5D" }) end)
wr_nak = true
expect_err("call 触发写无应答", function() return call({ fd = FD, address = "0x5D" }) end, "not responding")
wr_nak = false
rd_nak = true
expect_err("call 读无应答", function() return call({ fd = FD, address = "0x5D" }) end, "not responding")
rd_nak = false
expect_err("call 非表参数", function() return call(3) end, "Invalid Arguments")
expect_err("call 缺 address", function() return call({ fd = FD }) end, "fd or address")

-- ==================== destroy ====================
reset()
expect_ok("destroy 无关断需求（单次即断电）", function() return destroy({ fd = FD, address = "0x5D" }) end)
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
