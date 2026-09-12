-- LTC2944.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test/test_ltc2944.lua（LTC2944 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 寄存器按字节建模；驱动 build 写 CONTROL 走 i2c_write_reg（单字节）、
-- read_reg16 用 i2c_read_multi(fd, reg, 2)。
-- 桩忠实模拟器件寄存器指针自增：CONTROL(0x01) 是 8 位寄存器，若驱动回归为
-- 3 字节写 [0x01,0x00,ctrl]，charge_msb_clobbered 检查会红（真实器件会被
-- 写进 Sleep 模式 + 电荷 MSB 被毁）——守护 2026-09 审计 S1 缺陷。
--
-- 换算锁定（依 ADI Linduino 与内核 ltc2941-battery-gauge.c 双源核对）：
--   电压 raw/65535*70.8；电流 (raw-0x7FFF)/0x7FFF*0.064/rsense（偏移二进制，中点 0A）；
--   温度 raw/65535*510-273.15；电荷 code*0.34e-3*(M*0.05)/(rsense*4096)*1000 mAh
--   控制字 = 0xC0（自动连续）| 预分频码<<3；预分频码 = log4(M)
--   presc_code_of：M>=4096->码6、>=1024->5、>=256->4、>=64->3、>=16->2、>=4->1、>=1->0

dofile("modules/LTC2944.lua")
local bitc = dofile("modules/bitcompat.lua")

-- ==================== 桩与器件模型 ====================
local regs16 = { [0x00] = 0x0000 }	-- STATUS 默认 0（存在性检查）
local last_control = nil			-- 最近一次写 control 的值
local charge_msb_clobbered = false	-- 多字节写越过 CONTROL 踩进 CHG_MSB（回归哨兵）
local read_fail = false			-- 模拟总线读失败
local write_fail = false			-- 模拟写 control 失败
local set_addr_ok = true			-- 模拟 set_addr 成功

function i2c_set_addr(fd, addr) return set_addr_ok end

-- 忠实模拟指针自增：多字节写从 reg 起逐字节落位（16 位读合并由 i2c_read_multi 完成）
function i2c_write_multi(fd, reg, ...)
	local b = { ... }
	if write_fail and reg == 0x01 then return -1 end
	for i, byte in ipairs(b) do
		local r = reg + i - 1
		if r > 0x01 and reg == 0x01 then charge_msb_clobbered = true end
		regs16[r] = byte
	end
	return 0
end

-- HAL 单字节写：CONTROL(0x01) 是 8 位寄存器，必须走这里
function i2c_write_reg(fd, reg, val)
	if write_fail and reg == 0x01 then return false end
	regs16[reg] = val
	if reg == 0x01 then last_control = val end
	return true
end

function i2c_read_multi(fd, reg, n)
	if read_fail then return -1 end
	local v = regs16[reg] or 0
	if n == 2 then return v end
	return v % (256 ^ n)
end

function milli_sleep(ms) end

-- 便捷：预置一组寄存器原始值
local function poke(chg, v, iraw, traw)
	regs16[0x02] = chg	-- CHG_MSB/LSB（16 位累计电荷）
	regs16[0x08] = v	-- V_MSB/LSB
	regs16[0x0E] = iraw	-- I_MSB/LSB
	regs16[0x14] = traw	-- T_MSB/LSB
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

-- ==================== call-before-build ====================
-- rsense/presc_m 仅 build 赋值，未 build 先 call 须按契约返回 ERR（而非 nil 算术崩溃）
expect_err("call 未 build 先调", function() return call({ fd = 3, address = "0x64" }) end, "not initialized")

-- ==================== build：默认参数 ====================
-- rsense 缺省 0.05，prescaler 缺省 4096 -> 码 6 -> control = 0xC0 | 6<<3 = 0xE0
-- 控制字 = 0xC0（自动连续）| (tab索引-1)<<3；presc_tab={1,4,16,64,256,1024,4096}
-- 缺省 M4096(索引7) -> 码6 -> 0xC0|0x30=0xF0；M16(索引3) -> 码2 -> 0xC0|0x10=0xD0；M1(索引1) -> 码0 -> 0xC0
expect_ok("build 默认参数", function() return build({ fd = 3, address = "0x64" }) end)
check(last_control == 0xF0,
	"缺省控制字 = 0xF0 (自动连续 + M4096码6)", string.format("0x%04X", last_control or 0))
check(not charge_msb_clobbered,
	"build 不越过 CONTROL 踩写 CHG_MSB（8 位寄存器单字节写）")

-- 字符串 fd/address（框架元数据侧全为字符串）
expect_ok("build 字符串 fd='3'", function() return build({ fd = "3", address = "0x64" }) end)

-- ==================== build：预分频码映射 ====================
expect_ok("build prescaler='1'", function() return build({ fd = 3, address = "0x64", prescaler = "1" }) end)
check(last_control == 0xC0, "M1 码0 => 0xC0", string.format("0x%04X", last_control or 0))

expect_ok("build prescaler='16'", function() return build({ fd = 3, address = "0x64", prescaler = "16" }) end)
check(last_control == 0xD0, "M16 码2 => 0xD0", string.format("0x%04X", last_control or 0))

expect_ok("build prescaler='-'（非法归 4096）", function() return build({ fd = 3, address = "0x64", prescaler = "-" }) end)
check(last_control == 0xF0, "'-' 归 4096 码6 => 0xF0", string.format("0x%04X", last_control or 0))

expect_ok("build prescaler='999999'（超大归 4096）", function() return build({ fd = 3, address = "0x64", prescaler = "999999" }) end)
check(last_control == 0xF0, "超大值归 4096 => 0xF0", string.format("0x%04X", last_control or 0))

-- ==================== build：错误通道 ====================
expect_err("build 缺 fd", function() return build({ address = "0x64" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")

set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x64" }) end, "set_addr failed")
set_addr_ok = true

read_fail = true
expect_err("build 存在性检查读失败", function() return build({ fd = 3, address = "0x64" }) end, "Failed to read")
read_fail = false

write_fail = true
expect_err("build 写 control 失败", function() return build({ fd = 3, address = "0x64" }) end, "Failed to write")
write_fail = false

-- ==================== call：换算 ====================
-- 先 build 设置 rsense=0.05（默认）
build({ fd = 3, address = "0x64", rsense = "0.05", prescaler = "4096" })

-- 电压：raw=32768 -> 32768/65535*70.8 = 35.4V
-- 电流：raw=0x7FFF -> 0A（中点）
-- 温度：raw=32768 -> 32768/65535*510-273.15 = -1.05°C
-- 电荷：raw=1000 -> 1000*0.34e-3*(4096*0.05)/(0.05*4096)*1000 = 1000*0.34e-3*1000 = 340 mAh
poke(1000, 32768, 0x7FFF, 32768)
local res = expect_ok("call 正常读值", function() return call({ fd = 3, address = "0x64" }) end)
if res then
	check(near(res.V, 32768 / 65535 * 70.8), "V = 32768/65535*70.8V", tostring(res.V))
	check(near(res.I, 0), "I = 0A (中点)", tostring(res.I))
	check(near(res.T, 32768 / 65535 * 510 - 273.15), "T = 换算", tostring(res.T))
	check(near(res.CHARGE, 1000 * 0.34e-3 * (4096 * 0.05) / (0.05 * 4096)),
		"CHARGE = 0.34", tostring(res.CHARGE))
end

-- 电流满量程：raw=0xFFFF -> (65535-32767)/32767*0.064/0.05 = +0.635A
poke(0, 0, 0xFFFF, 0)
res = expect_ok("call 电流满量程", function() return call({ fd = 3, address = "0x64" }) end)
if res then
	check(near(res.I, (0xFFFF - 0x7FFF) / 0x7FFF * 0.064 / 0.05), "I ≈ +0.635A", tostring(res.I))
end

-- 电流负向（放电）：raw=0 -> (0-32767)/32767*0.064/0.05 = -0.635A
poke(0, 0, 0, 0)
res = expect_ok("call 电流放电为负", function() return call({ fd = 3, address = "0x64" }) end)
if res then
	check(near(res.I, (0 - 0x7FFF) / 0x7FFF * 0.064 / 0.05), "I ≈ -0.635A", tostring(res.I))
end

-- rsense='-' 归缺省 0.05
expect_ok("build rsense='-'", function() return build({ fd = 3, address = "0x64" }) end)
poke(0, 0, 0x7FFF, 0)
res = expect_ok("call 缺省 rsense", function() return call({ fd = 3, address = "0x64" }) end)
if res then
	check(near(res.I, 0), "I = 0A (缺省 rsense)", tostring(res.I))
end

-- 错误通道
expect_err("call 缺 fd", function() return call({ address = "0x64" }) end, "fd or address")
read_fail = true
expect_err("call 读失败", function() return call({ fd = 3, address = "0x64" }) end, "Failed to read")
read_fail = false

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
