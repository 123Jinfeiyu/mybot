-- MAX17043.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test/test_max17043.lua（MAX17043 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 寄存器 16 位单元建模；驱动 read_reg16 用 i2c_read_multi(fd, reg, 2)。
-- 纯只读驱动：build 不写任何寄存器，仅读版本号（0x08）做存在性检查。
--
-- 换算锁定（依内核 max17040_battery.c 钉死）：
--   VCELL(0x02)：12 位左移 4，读后 >>4，LSB 1.25mV
--   SOC(0x04)：16 位定点 1/256% 增量，raw/256 = 百分比
--   无 ID/POR/STATUS 寄存器（那是 MAX17048 的 0x1A）

dofile("modules/MAX17043.lua")
local bitc = dofile("modules/bitcompat.lua")

-- ==================== 桩与器件模型 ====================
local regs16 = { [0x08] = 0x0100 }	-- 版本号（任意值，存在性检查只要求可读）
local read_fail = false			-- 模拟总线读失败
local set_addr_ok = true			-- 模拟 set_addr 成功

function i2c_set_addr(fd, addr) return set_addr_ok end

function i2c_read_multi(fd, reg, n)
	if read_fail then return -1 end
	local v = regs16[reg] or 0
	if n == 2 then return v end
	return v % (256 ^ n)
end

function milli_sleep(ms) end

-- 便捷：预置 VCELL/SOC 原始值
local function poke(vcell, soc)
	regs16[0x02] = vcell
	regs16[0x04] = soc
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

-- ==================== build：纯只读存在性检查 ====================
-- 版本号可读即通过
expect_ok("build 正常（读版本号）", function() return build({ fd = 3, address = "0x36" }) end)

-- 字符串 fd/address
expect_ok("build 字符串 fd='3'", function() return build({ fd = "3", address = "0x36" }) end)

-- ==================== build：错误通道 ====================
expect_err("build 缺 fd", function() return build({ address = "0x36" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")

set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x36" }) end, "set_addr failed")
set_addr_ok = true

read_fail = true
expect_err("build 读版本号失败", function() return build({ fd = 3, address = "0x36" }) end, "Failed to read")
read_fail = false

-- ==================== call：换算 ====================
-- VCELL：raw 12 位左移 4，读后 >>4
-- 例：raw=0x0800 (=2048) -> >>4 = 128 -> 128*1.25mV = 160mV = 0.16V
-- SOC：raw=32768 -> 32768/256 = 128%（超 100%，器件饱和）
poke(0x0800, 32768)
local res = expect_ok("call VCELL/SOC 换算", function() return call({ fd = 3, address = "0x36" }) end)
if res then
	-- VCELL：LuaJIT bit.rshift(0x0800,4) = 128 -> 128*0.00125 = 0.16V
	check(near(res.VCELL, bitc.rshift(0x0800, 4) * 0.00125), "VCELL = >>4 x 1.25mV", tostring(res.VCELL))
	-- SOC：32768/256 = 128
	check(near(res.SOC, 128), "SOC = 128%", tostring(res.SOC))
end

-- 满量程电池：VCELL raw=0x7FF0 (=32784) -> >>4 = 2049 -> 2049*1.25mV = 2.56125V
-- SOC raw=0xFFFF (=65535) -> 65535/256 = 255.99%
poke(0x7FF0, 0xFFFF)
res = expect_ok("call 满量程", function() return call({ fd = 3, address = "0x36" }) end)
if res then
	check(near(res.VCELL, bitc.rshift(0x7FF0, 4) * 0.00125), "VCELL 满量程", tostring(res.VCELL))
	check(near(res.SOC, 65535 / 256), "SOC 满量程 ≈ 255.99%", tostring(res.SOC))
end

-- 空电池：VCELL raw=0 -> 0V；SOC raw=0 -> 0%
poke(0, 0)
res = expect_ok("call 空电池", function() return call({ fd = 3, address = "0x36" }) end)
if res then
	check(res.VCELL == 0, "VCELL = 0V", tostring(res.VCELL))
	check(res.SOC == 0, "SOC = 0%", tostring(res.SOC))
end

-- 典型：锂电池 4.1V 满充 -> VCELL raw = 4.1/0.00125*16 = 52480 (0xCD80)
-- SOC 85% -> raw = 85*256 = 21760
poke(52480, 21760)
res = expect_ok("call 典型 4.1V/85%", function() return call({ fd = 3, address = "0x36" }) end)
if res then
	check(near(res.VCELL, 4.1, 0.002), "VCELL ≈ 4.1V", tostring(res.VCELL))
	check(near(res.SOC, 85), "SOC = 85%", tostring(res.SOC))
end

-- 错误通道
expect_err("call 缺 fd", function() return call({ address = "0x36" }) end, "fd or address")
expect_err("call 缺 address", function() return call({ fd = 3 }) end, "fd or address")

set_addr_ok = false
expect_err("call set_addr 失败", function() return call({ fd = 3, address = "0x36" }) end, "set_addr failed")
set_addr_ok = true

read_fail = true
expect_err("call 读 VCELL 失败", function() return call({ fd = 3, address = "0x36" }) end, "Failed to read")
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
