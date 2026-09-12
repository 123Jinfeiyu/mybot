-- INA219.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_ina219.lua（INA219 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效——先注桩会被 HAL 的真函数覆盖
-- （test_pca9685_channel/float.lua 的旧桩模式即踩此坑，两个文件已同步修正）。
-- 桩内模拟器件语义：写 config=0x8000 触发软复位（读回配置默认值 0x399F），
-- 使 build 的「软复位 -> 读配置甄别 -> 写配置 -> 等首拍」流程可离线走通。
-- INA219 无 ID 寄存器（0xFE/0xFF 不存在），甄别靠复位后配置默认值 0x399F。
--
-- 关键换算预期值锁定 Linux 内核 drivers/iio/adc/ina2xx-adc.c 与
-- TI datasheet SBOSA33：
--   分流寄存器 16 位完整有符号、无移位，LSB 10µV（PGA=/8 量程 ±320mV）
--   总线寄存器 13 位有效、左移 3，读后 >>3，LSB 4mV
--   配置默认值 0x399F = BRNG(1<<13) | PGA(3<<11) | BADC(3<<7) | SADC(3<<3) | MODE_CONT
--   工作配置 = 0x399F | avg_code<<(BADC|SADC 各 4 位)
--   build 等待 ceil(2 * 532µs * avg_n / 1000) + 2 ms

dofile("modules/INA219.lua")

-- ==================== 桩与器件模型 ====================
-- 寄存器按 16 位单元建模（真实器件如此）：0x01/0x02 相邻寄存器各自独立
local regs16 = {}			-- 寄存器地址 -> 16 位值（未实现的寄存器读作 0x0000）
local last_work_cfg = nil	-- 最近一次写 config 的工作配置（16 位）
local sleep_ms = nil		-- 最近一次 milli_sleep
local fail_read = false		-- 置 true 模拟总线读失败
local set_addr_ok = true	-- 置 false 模拟 set_addr 失败

-- 复位后读回的默认配置（用例可改写以模拟错挂芯片，如 INA226 的 0x4127）
local cfg_after_reset = 0x399F

local function poke16(reg, v)
	regs16[reg] = v
end

local function reset_device()
	regs16 = {}
	poke16(0x00, cfg_after_reset)
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return set_addr_ok end

function i2c_write_multi(fd, reg, ...)
	local b = { ... }
	-- 软复位命令 0x8000：不落盘，恢复复位默认配置
	if reg == 0x00 and b[1] == 0x80 and b[2] == 0x00 then
		reset_device()
		return 0
	end
	regs16[reg] = b[1] * 256 + b[2]
	if reg == 0x00 then last_work_cfg = regs16[reg] end
	return 0
end

function i2c_read_multi(fd, reg, n)
	if fail_read then return -1 end
	local v = regs16[reg] or 0
	if n == 2 then return v end
	return v % (256 ^ n)	-- 其他宽度取低 n 字节（本驱动只用 n=2）
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
cfg_after_reset = 0x399F
fail_read, set_addr_ok = false, true

expect_ok("build 默认参数 (avg/rshunt 缺省)", function() return build({ fd = 3, address = "0x40" }) end)
check(last_work_cfg == 0x3E67,
	"缺省工作配置 = 0x3E67 (bit13|PGA3|avg16code12|cont)", string.format("0x%04X", last_work_cfg or 0))
check(sleep_ms ~= nil and sleep_ms >= math.ceil(2 * 532 * 16 / 1000),
	"build 等待 >= 一个转换循环 (>=17ms)", tostring(sleep_ms))

-- 字符串 fd/address（框架元数据侧全为字符串）
expect_ok("build 字符串 fd='3'", function() return build({ fd = "3", address = "0x40" }) end)

-- ==================== build：avg 降档 ====================
expect_ok("build avg='3'", function() return build({ fd = 3, address = "0x40", avg = "3" }) end)
check(last_work_cfg == 0x3CCF, "avg3->2 code9 => 0x3CCF", string.format("0x%04X", last_work_cfg or 0))

expect_ok("build avg='32'", function() return build({ fd = 3, address = "0x40", avg = "32" }) end)
check(last_work_cfg == 0x3EEF, "avg32 code13 => 0x3EEF", string.format("0x%04X", last_work_cfg or 0))

expect_ok("build avg='2000'", function() return build({ fd = 3, address = "0x40", avg = "2000" }) end)
check(last_work_cfg == 0x3FFF, "avg2000->128 code15 => 0x3FFF", string.format("0x%04X", last_work_cfg or 0))

expect_ok("build avg='-'", function() return build({ fd = 3, address = "0x40", avg = "-" }) end)
check(last_work_cfg == 0x3E67, "'-' 缺省档 => 0x3E67", string.format("0x%04X", last_work_cfg or 0))

-- ==================== build：甄别与错误通道 ====================
cfg_after_reset = 0x4127
expect_err("build 复位配置不符（INA226 误挂）", function() return build({ fd = 3, address = "0x40" }) end, "Config after reset")
cfg_after_reset = 0x399F

fail_read = true
expect_err("build 总线读失败", function() return build({ fd = 3, address = "0x40" }) end, "Failed to read")
fail_read = false

set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x40" }) end, "set_addr failed")
set_addr_ok = true

expect_err("build 缺 fd", function() return build({ address = "0x40" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")

-- ==================== call：未 build 守卫 ====================
-- 先 reset_device 后 build 使 rshunt 已置位（避免守卫误报），
-- 再直接验证 call 在 rshunt 已置位时的正常路径
expect_ok("build rshunt='0.05'", function() return build({ fd = 3, address = "0x40", rshunt = "0.05" }) end)
poke16(0x01, 4000)		-- 分流 4000 LSB x 10µV = 40mV
poke16(0x02, 16384)		-- 总线 (16384>>3) x 4mV = 20480mV = 20.48V
local res = expect_ok("call 正常读值", function() return call({ fd = 3, address = "0x40" }) end)
if res then
	check(near(res.V, 8.192), "V = 8.192V", tostring(res.V))
	check(near(res.I, 0.8), "I = 40mV/0.05R = 0.8A", tostring(res.I))
	check(near(res.P, 6.5536), "P = 6.5536W", tostring(res.P))
end

-- 反流：shunt=0xFC00 (-1024 LSB -> -10.24mV)
poke16(0x01, 0xFC00)
res = expect_ok("call 反流为负", function() return call({ fd = 3, address = "0x40" }) end)
if res then
	check(near(res.I, -0.2048), "I = -0.2048A", tostring(res.I))
	check(near(res.P, -1.6777216), "P = -1.6777216W", tostring(res.P))
end

-- rshunt='-' 归缺省 0.1Ω
expect_ok("build rshunt='-'", function() return build({ fd = 3, address = "0x40", rshunt = "-" }) end)
poke16(0x01, 4000)
res = expect_ok("call 缺省 rshunt", function() return call({ fd = 3, address = "0x40" }) end)
if res then
	check(near(res.I, 0.4), "I = 40mV/0.1R = 0.4A", tostring(res.I))
end

-- 错误通道
expect_err("call 缺 fd", function() return call({ address = "0x40" }) end, "fd or address")
fail_read = true
expect_err("call 分流读失败", function() return call({ fd = 3, address = "0x40" }) end, "shunt voltage")
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
