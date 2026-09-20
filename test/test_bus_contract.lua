-- test_bus_contract.lua - 数值归一化契约回归（2026-09-20 审计 C2）
--
-- 背景：byte_of / uint_of 是全仓唯一的字节/非负整数归一化点（HAL 真机路径与
-- virtbus 虚拟总线共用同一份），注释承诺「非法返回 nil」。但旧实现只写
-- `n = math.floor(n); if n < 0 or n > 255 then return nil end`，而 LuaJIT 下
--    math.floor(NaN) = NaN，且 NaN < 0、NaN > 255 均为 false
-- —— NaN（含字符串 "nan"/"inf"，tonumber 亦返回 NaN/inf）被当作**合法值**返回，
-- 一路走到 ffi.new("uint8_t", NaN) 变成 0x00：
--   HAL.i2c_write_reg(fd, reg, NaN) 返回成功但线上写的是 0x00；
--   PCA9685 的 NaN 角度穿透 0..180 夹取后写成 ON=0/OFF=0（无脉冲）却返回 OK。
-- 这是「静默错写真实硬件」这一类里最坏的一种：既不报错，也看不出值被改过。
--
-- 用法（项目根目录）: luajit test/test_bus_contract.lua
local tester = dofile("test/lib/tester.lua")

local bus = dofile("modules/lib/bus_contract.lua")
dofile("modules/lib/drv_common.lua")		-- 定义 int_of / num_or（纯 Lua，无 HAL 依赖）

local nan = 0 / 0
local inf = math.huge

-- ---- byte_of：非有限值必须被拒 ----

tester.check("byte_of(NaN) 返回 nil", bus.byte_of(nan) == nil)
tester.check("byte_of(inf) 返回 nil", bus.byte_of(inf) == nil)
tester.check("byte_of(-inf) 返回 nil", bus.byte_of(-inf) == nil)
tester.check("byte_of('nan') 返回 nil", bus.byte_of("nan") == nil)
tester.check("byte_of('inf') 返回 nil", bus.byte_of("inf") == nil)
tester.check("byte_of('1e400') 返回 nil（strtod 溢出为 inf）", bus.byte_of("1e400") == nil)
tester.check("byte_of(nil) 返回 nil", bus.byte_of(nil) == nil)
tester.check("byte_of(负数) 返回 nil", bus.byte_of(-1) == nil)
tester.check("byte_of(256) 返回 nil", bus.byte_of(256) == nil)

-- ---- byte_of：合法输入行为逐字不变（NaN 防线不得误伤）----

tester.check("byte_of(0) == 0", bus.byte_of(0) == 0)
tester.check("byte_of(255) == 255", bus.byte_of(255) == 255)
tester.check("byte_of(1.9) == 1（向下取整）", bus.byte_of(1.9) == 1)
tester.check("byte_of('7') == 7（字符串数字）", bus.byte_of("7") == 7)
tester.check("byte_of(0x0F) == 15", bus.byte_of(15) == 15)

-- ---- uint_of：非有限值必须被拒（+inf 旧实现会原样返回）----

tester.check("uint_of(NaN) 返回 nil", bus.uint_of(nan) == nil)
tester.check("uint_of(inf) 返回 nil", bus.uint_of(inf) == nil)
tester.check("uint_of(-inf) 返回 nil", bus.uint_of(-inf) == nil)
tester.check("uint_of('nan') 返回 nil", bus.uint_of("nan") == nil)
tester.check("uint_of(-1) 返回 nil", bus.uint_of(-1) == nil)

tester.check("uint_of(0) == 0", bus.uint_of(0) == 0)
tester.check("uint_of(3.7) == 3", bus.uint_of(3.7) == 3)
tester.check("uint_of(1000000) == 1000000", bus.uint_of(1000000) == 1000000)

-- ---- drv_common.int_of：钳制语义，但非有限值必须回落缺省 ----

tester.check("int_of(NaN, 9, 0, 100) == 9", int_of(nan, 9, 0, 100) == 9)
tester.check("int_of(inf, 9, 0, 100) == 9", int_of(inf, 9, 0, 100) == 9)
tester.check("int_of(-inf, 9, 0, 100) == 9", int_of(-inf, 9, 0, 100) == 9)
tester.check("int_of('nan', 9, 0, 100) == 9", int_of("nan", 9, 0, 100) == 9)
tester.check("int_of('-' , 9, 0, 100) == 9（占位符）", int_of("-", 9, 0, 100) == 9)
tester.check("int_of(1e9, 0, 0, 100) == 100（钳上界不变）", int_of(1e9, 0, 0, 100) == 100)
tester.check("int_of(-5, 0, 0, 100) == 0（钳下界不变）", int_of(-5, 0, 0, 100) == 0)
tester.check("int_of(42, 0, 0, 100) == 42", int_of(42, 0, 0, 100) == 42)

-- ---- drv_common.num_or：非有限值必须回落缺省 ----

tester.check("num_or(NaN, 3) == 3", num_or(nan, 3) == 3)
tester.check("num_or(inf, 3) == 3", num_or(inf, 3) == 3)
tester.check("num_or('nan', 3) == 3", num_or("nan", 3) == 3)
tester.check("num_or(nil, 3) == 3", num_or(nil, 3) == 3)
tester.check("num_or('2.5', 3) == 2.5", num_or("2.5", 3) == 2.5)
tester.check("num_or(0, 3) == 0（0 是合法值，不得被 or 吞）", num_or(0, 3) == 0)

tester.summary("test_bus_contract")
return 0
