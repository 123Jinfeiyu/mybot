-- INA3221.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_ina3221.lua（INA3221 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效——先注桩会被 HAL 的真函数覆盖
-- （test_pca9685_channel/float.lua 的旧桩模式即踩此坑，两个文件已同步修正）。
-- 桩内模拟器件语义：写 config=0x8000 触发软复位（恢复 ID 寄存器 0xFE/0xFF），
-- 使 build 的「校验 ID -> 软复位 -> 写配置 -> 等首拍」流程可离线走通。
--
-- 关键换算预期值锁定 Linux 内核 drivers/hwmon/ina3221.c 与 TI datasheet：
--   分流/总线寄存器同为 13 位有效、左移 3，读取后 >>3
--   分流有符号（补码），LSB 40µV；总线无符号，LSB 8mV
--   0xFE 厂商 ID 恒 0x5449、0xFF 芯片 ID 恒 0x3220
--   配置默认 CT=4 (1.1ms)，三通道使能 0x7<<12，MODE_CONT=0x7
--   缺省工作配置 = 0x7000 | avg_code<<9 | 4<<6 | 4<<3 | 0x7 = 0x73F7（avg16 时）

dofile("modules/INA3221.lua")

-- ==================== 桩与器件模型 ====================
local regs16 = {}			-- 寄存器地址 -> 16 位值
local last_work_cfg = nil	-- 最近一次写 config 的工作配置（16 位）
local sleep_ms = nil		-- 最近一次 milli_sleep
local fail_read = false		-- 置 true 模拟总线读失败
local set_addr_ok = true	-- 置 false 模拟 set_addr 失败

-- 器件身份（用例可改写以模拟错挂芯片）
local mfg_id, die_id = 0x5449, 0x3220

local function poke16(reg, v)
	regs16[reg] = v
end

local function reset_device()
	regs16 = {}
	poke16(0xFE, mfg_id)
	poke16(0xFF, die_id)
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return set_addr_ok end

function i2c_write_multi(fd, reg, ...)
	local b = { ... }
	-- 软复位命令 0x8000：不落盘，恢复 ID
	if reg == 0x00 and b[1] == 0x80 and b[2] == 0x00 then
		reset_device()
		return 0
	end
	regs16[reg] = b[1] * 256 + b[2]
	if reg == 0x00 then last_work_cfg = regs16[reg] end
	return 0
end

-- INA3221 的 build 先校验 ID 再软复位，所以预置桩前必须确保 0xFE/0xFF 已注入
-- 桩：i2c_read_multi 命中 0xFE/0xFF 时直接返回 mfg_id/die_id，不查 regs16
function i2c_read_multi(fd, reg, n)
	if fail_read then return -1 end
	if reg == 0xFE then return mfg_id end
	if reg == 0xFF then return die_id end
	local v = regs16[reg] or 0
	if n == 2 then return v end
	return v % (256 ^ n)
end

function milli_sleep(ms) sleep_ms = ms end

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

-- ==================== build：默认参数 ====================
mfg_id, die_id = 0x5449, 0x3220
fail_read, set_addr_ok = false, true

expect_ok("build 默认参数 (avg/rshunt 缺省)", function() return build({ fd = 3, address = "0x40" }) end)
check(last_work_cfg == 0x7527,
	"缺省工作配置 = 0x7527 (3ch|avg16code2|CT4|cont)", string.format("0x%04X", last_work_cfg or 0))
check(sleep_ms ~= nil and sleep_ms >= math.ceil(3 * (1.1 + 1.1) * 16) + 2,
	"build 等待 >= 一个完整转换循环", tostring(sleep_ms))

-- 字符串 fd/address（框架元数据侧全为字符串）
expect_ok("build 字符串 fd='3'", function() return build({ fd = "3", address = "0x40" }) end)

-- ==================== build：avg 降档 ====================
expect_ok("build avg='3'", function() return build({ fd = 3, address = "0x40", avg = "3" }) end)
check(last_work_cfg == 0x7127, "avg3->1 code0 => 0x7127", string.format("0x%04X", last_work_cfg or 0))

expect_ok("build avg='32'", function() return build({ fd = 3, address = "0x40", avg = "32" }) end)
check(last_work_cfg == 0x7527, "avg32->16 code2 => 0x7527", string.format("0x%04X", last_work_cfg or 0))

expect_ok("build avg='2000'", function() return build({ fd = 3, address = "0x40", avg = "2000" }) end)
check(last_work_cfg == 0x7F27, "avg2000->1024 code7 => 0x7F27", string.format("0x%04X", last_work_cfg or 0))

expect_ok("build avg='-'", function() return build({ fd = 3, address = "0x40", avg = "-" }) end)
check(last_work_cfg == 0x7527, "'-' 缺省档 => 0x7527", string.format("0x%04X", last_work_cfg or 0))

-- ==================== build：甄别与错误通道 ====================
mfg_id = 0x1234
expect_err("build 厂商 ID 不符", function() return build({ fd = 3, address = "0x40" }) end, "Unexpected IDs")
mfg_id = 0x5449

die_id = 0x3221
expect_err("build 芯片 ID 不符", function() return build({ fd = 3, address = "0x40" }) end, "Unexpected IDs")
die_id = 0x3220

fail_read = true
expect_err("build 总线读失败", function() return build({ fd = 3, address = "0x40" }) end, "Failed to read")
fail_read = false

set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x40" }) end, "set_addr failed")
set_addr_ok = true

expect_err("build 缺 fd", function() return build({ address = "0x40" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")

-- ==================== call：换算 ====================
-- build rshunt1/2/3='0.1' 后预设三通道寄存器
-- 分流寄存器 13 位有符号左移 3：值 10000 (0x2710) -> >>3 = 1250 -> x40µV = 50mV
-- 总线寄存器 13 位无符号左移 3：值 10000 (0x2710) -> >>3 = 1250 -> x8mV = 10V
expect_ok("build rshunt='0.1/0.05/0.01'", function()
	return build({ fd = 3, address = "0x40", rshunt1 = "0.1", rshunt2 = "0.05", rshunt3 = "0.01" })
end)

-- 通道 1：shunt=10000 (50mV), bus=10000 (10V), rshunt=0.1
poke16(0x01, 10000)		-- SHUNT1
poke16(0x02, 10000)		-- BUS1

-- 通道 2：shunt=5000 (25mV), bus=5000 (5V), rshunt=0.05
poke16(0x03, 5000)		-- SHUNT2
poke16(0x04, 5000)		-- BUS2

-- 通道 3：shunt=2500 (12.5mV), bus=2500 (2.5V), rshunt=0.01
poke16(0x05, 2500)		-- SHUNT3
poke16(0x06, 2500)		-- BUS3

local res = expect_ok("call 三通道读值", function() return call({ fd = 3, address = "0x40" }) end)
if res then
	-- ch1: V=10V, I=50mV/0.1R=0.5A
	check(near(res.V1, 10.0), "V1 = 10.0V", tostring(res.V1))
	check(near(res.I1, 0.5), "I1 = 0.5A", tostring(res.I1))
	-- ch2: V=5V, I=25mV/0.05R=0.5A
	check(near(res.V2, 5.0), "V2 = 5.0V", tostring(res.V2))
	check(near(res.I2, 0.5), "I2 = 0.5A", tostring(res.I2))
	-- ch3: V=2.496V, I=12.48mV/0.01R=1.248A
	check(near(res.V3, 2.496), "V3 = 2.496V", tostring(res.V3))
	check(near(res.I3, 1.248), "I3 = 1.248A", tostring(res.I3))
end

-- 反流：shunt=0xFC00 (LuaJIT bit.rshift 无符号：0xFC00=64512, >>3=8064, s13(8064)=8064-8192=-128)
-- -128 x 40µV = -5.12mV, /0.1R = -0.0512A
poke16(0x01, 0xFC00)
res = expect_ok("call 反流为负", function() return call({ fd = 3, address = "0x40" }) end)
if res then
	check(near(res.I1, -0.0512), "I1 = -0.0512A", tostring(res.I1))
end

-- rshunt='-' 归缺省 0.01Ω
expect_ok("build rshunt='-'", function() return build({ fd = 3, address = "0x40" }) end)
poke16(0x01, 10000)
res = expect_ok("call 缺省 rshunt", function() return call({ fd = 3, address = "0x40" }) end)
if res then
	check(near(res.I1, 5.0), "I1 = 50mV/0.01R = 5.0A", tostring(res.I1))
end

-- 错误通道
expect_err("call 缺 fd", function() return call({ address = "0x40" }) end, "fd or address")
fail_read = true
expect_err("call 寄存器读失败", function() return call({ fd = 3, address = "0x40" }) end, "Failed to read")
fail_read = false

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
