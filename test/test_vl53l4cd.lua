-- VL53L4CD.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_vl53l4cd.lua（VL53L4CD 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效——先注桩会被 HAL 的真函数覆盖
-- （test_pca9685_channel/float.lua 的旧桩模式即踩此坑，两个文件已同步修正）。
-- 桩内模拟器件语义：16 位寄存器字节模型 + 指针自增流写；0x0000 软复位
-- （上电默认 + 振荡器/固件状态）；data-ready 按 0x0030 bit4 极性与 0x0031
-- bit0 的真实判定逻辑模拟，可注入翻转/失败/永不就绪。
--
-- SetRangeTiming 编码预期值为手算常量（osc=10000 时 macro=3865464），
-- 用于锁定对 ST ULD 源码的移植不被无意改动：
--   budget=50/inter=0  -> A=0x00C8 B=0x0185（B 的 267 归一化为 exp1/ls133）
--   budget=200         -> A=0x02D1 B=0x038B（1115 归一化为 exp3/ls139）
--   budget=10          -> A=0x001F B=0x0029
--   budget=50/inter=100（clock_pll=512）-> INTER=54016(0xD300) A=0x0060 B=0x0080
--   （1.055 系数：C 端 float32 与 Lua double 在该输入下恰好同为 54016）

dofile("modules/VL53L4CD.lua")

-- ==================== 桩与器件模型 ====================
local regs = {}					-- 16 位寄存器地址 -> 字节值
local got_boot = false			-- 收到软复位释放（0x0000=0x01）
local writes = {}				-- 复位后的写事务日志 {reg=, n=}
local start_writes = {}			-- 写入 0x0087 的值序列
local clear_count = 0			-- 0x0086 清中断次数
local fail_write = false
local fail_read = false
local fail_regs = {}
local set_addr_ok = true
local set_addr_log = {}
local sleep_total = 0
-- 用例可控的器件状态
local boot_status = 3			-- 0x00E5 固件启动状态
local never_ready = false		-- 置 true 强制 data-ready 恒为未就绪

local function reset_device()
	regs = {}
	regs[0x0006] = 0x27; regs[0x0007] = 0x10	-- 振荡器频率 word = 10000
	regs[0x00DE] = 0x02; regs[0x00DF] = 0x00	-- PLL 校准 word = 0x0200
	regs[0x00E5] = boot_status
	writes = {}
	start_writes = {}
	clear_count = 0
end

local function reset_model()
	reset_device()
	got_boot = false
	fail_write, fail_read, set_addr_ok = false, false, true
	fail_regs = {}
	set_addr_log = {}
	sleep_total = 0
	boot_status = 3
	never_ready = false
end

-- data-ready 模型：0x0030 bit4 -> 有效极性；0x0031 bit0 == 极性 -> 就绪
-- never_ready=true 时由 i2c_read_reg16 桩强制 0x0031 读值的 bit0 恒不等于极性

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr)
	set_addr_log[#set_addr_log + 1] = addr
	return set_addr_ok
end

function i2c_write_reg16(fd, reg16, ...)
	if fail_write then return -1 end
	local b = { ... }
	if reg16 == 0x0000 and b[1] == 0x00 then return 0 end
	if reg16 == 0x0000 and b[1] == 0x01 then
		got_boot = true
		reset_device()
		return 0
	end
	for i, v in ipairs(b) do regs[reg16 + i - 1] = v end	-- 指针自增流写
	writes[#writes + 1] = { reg = reg16, n = #b }
	if reg16 == 0x0087 then start_writes[#start_writes + 1] = b[1] end
	if reg16 == 0x0086 then clear_count = clear_count + 1 end
	return 0
end

function i2c_read_reg16(fd, reg16, n)
	if fail_read or fail_regs[reg16] then return -1 end
	if never_ready and reg16 == 0x0031 then
		local mux = regs[0x0030] or 0
		local int_pol = (math.floor(mux / 16) % 2 == 1) and 0 or 1
		return (int_pol == 0) and 0x03 or 0x02	-- bit0 恒 != 有效极性
	end
	local v = 0
	for i = 0, n - 1 do v = v * 256 + (regs[reg16 + i] or 0) end
	return v
end

function milli_sleep(ms) sleep_total = sleep_total + ms end

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

local function reg_is(reg, val, label)
	local extra = string.format("0x%04X=0x%02X", reg, regs[reg] or 0)
	check(regs[reg] == val, label .. string.format(" (0x%04X=0x%02X)", reg, val), extra)
end

-- 找复位后配置流写事务（0x2D 起始 91 字节）
local function config_stream_n()
	for _, w in ipairs(writes) do
		if w.reg == 0x2D then return w.n end
	end
	return nil
end

-- ==================== build：默认参数 ====================
reset_model()
expect_ok("build 默认参数 (budget/inter/timeout 缺省)", function() return build({ fd = 3, address = "0x29" }) end)
check(got_boot, "build 发送软复位释放 (0x0000=0x01)")
check(config_stream_n() == 91, "默认配置流 91 字节单事务", tostring(config_stream_n()))
check(#start_writes == 3 and start_writes[1] == 0x40 and start_writes[2] == 0x00 and start_writes[3] == 0x21,
	"启动序列 VHV(0x40)->停(0x00)->连续测距(0x21)",
	table.concat(start_writes, ",", 1, #start_writes))
check(clear_count == 2, "清中断 2 次（VHV + 首测量）", tostring(clear_count))
reg_is(0x0008, 0x09, "VHV_LOOP_BOUND")
reg_is(0x000B, 0x00, "0x000B")
reg_is(0x0024, 0x05, "0x0024 word 高字节")
reg_is(0x0025, 0x00, "0x0024 word 低字节")
reg_is(0x005E, 0x00, "RANGE_CONFIG_A 高字节 (budget=50 -> 0x00C8)")
reg_is(0x005F, 0xC8, "RANGE_CONFIG_A 低字节")
reg_is(0x0061, 0x01, "RANGE_CONFIG_B 高字节 (budget=50 -> 0x0185)")
reg_is(0x0062, 0x85, "RANGE_CONFIG_B 低字节")
reg_is(0x006C, 0x00, "INTERMEASUREMENT dword=0 (连续模式)")

-- 字符串 fd/address（框架元数据侧全为字符串）
reset_model()
expect_ok("build 字符串 fd='3' address='0x29'", function() return build({ fd = "3", address = "0x29" }) end)
check(#set_addr_log > 0 and set_addr_log[#set_addr_log] == 41,
	"set_addr 收到 0x29=41", tostring(set_addr_log[#set_addr_log]))

-- ==================== build：budget/inter ====================
reset_model()
expect_ok("build budget='200'", function() return build({ fd = 3, address = "0x29", budget = "200" }) end)
reg_is(0x005E, 0x02, "budget=200 A 高字节 (0x02D1)")
reg_is(0x005F, 0xD1, "budget=200 A 低字节")
reg_is(0x0061, 0x03, "budget=200 B 高字节 (0x038B)")
reg_is(0x0062, 0x8B, "budget=200 B 低字节")

reset_model()
expect_ok("build budget='5'（钳到 10）", function() return build({ fd = 3, address = "0x29", budget = "5" }) end)
reg_is(0x005E, 0x00, "budget=10 A 高字节 (0x001F)")
reg_is(0x005F, 0x1F, "budget=10 A 低字节")
reg_is(0x0061, 0x00, "budget=10 B 高字节 (0x0029)")
reg_is(0x0062, 0x29, "budget=10 B 低字节")

reset_model()
expect_ok("build inter='100'（自主模式）", function() return build({ fd = 3, address = "0x29", inter = "100" }) end)
check(regs[0x006C] == 0x00 and regs[0x006D] == 0x00 and regs[0x006E] == 0xD3 and regs[0x006F] == 0x00,
	"INTERMEASUREMENT = 54016 (0x0000D300, 1.055 x 100 x 512 截断)",
	string.format("0x%02X%02X%02X%02X", regs[0x006C] or 0, regs[0x006D] or 0, regs[0x006E] or 0, regs[0x006F] or 0))
reg_is(0x005E, 0x00, "自主模式 A 高字节 (0x0060)")
reg_is(0x005F, 0x60, "自主模式 A 低字节")
reg_is(0x0061, 0x00, "自主模式 B 高字节 (0x0080)")
reg_is(0x0062, 0x80, "自主模式 B 低字节")
check(start_writes[#start_writes] == 0x40, "自主模式启动值 0x40", tostring(start_writes[#start_writes]))

expect_err("build inter='30' (<=budget 非法)", function() return build({ fd = 3, address = "0x29", inter = "30" }) end, "Invalid inter-measurement")

-- ==================== build：错误通道 ====================
reset_model()
fail_write = true
expect_err("build 软复位写失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to reset VL53L4CD")

reset_model()
boot_status = 0
expect_err("build 固件启动超时", function() return build({ fd = 3, address = "0x29" }) end, "boot timeout")

reset_model()
fail_regs = { [0x00E5] = true }
expect_err("build 读固件状态失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to read firmware boot status")

reset_model()
fail_regs = { [0x0031] = true }
expect_err("build 轮询 data-ready 总线失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to poll VL53L4CD data-ready")

reset_model()
never_ready = true
expect_err("build VHV 测量超时", function() return build({ fd = 3, address = "0x29" }) end, "VHV measurement ready timeout")
never_ready = false

reset_model()
fail_regs = { [0x0006] = true }
expect_err("build 读振荡器频率失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to read oscillator frequency")

reset_model()
set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x29" }) end, "set_addr failed")
set_addr_ok = true

expect_err("build 缺 fd", function() return build({ address = "0x29" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")
expect_err("build 参数非表", function() return build(nil) end, "Invalid Arguments Type")

-- ==================== call：结果读取与状态映射 ====================
reset_model()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x29" }) end)

-- 有效测量：raw 状态 0x09 -> 映射 0；注入 1234mm / 800kcps / 400kcps / sigma 2.0 / 2 SPAD
regs[0x0089] = 0x09
regs[0x008C] = 0x02; regs[0x008D] = 0x00		-- spad word 512 -> 2
regs[0x008E] = 0x00; regs[0x008F] = 0x64		-- signal word 100 -> 800kcps
regs[0x0090] = 0x00; regs[0x0091] = 0x32		-- ambient word 50 -> 400kcps
regs[0x0092] = 0x00; regs[0x0093] = 0x08		-- sigma word 8 -> 2.0mm
regs[0x0096] = 0x04; regs[0x0097] = 0xD2		-- distance word 1234
local res = expect_ok("call 有效测量", function() return call({ fd = 3, address = "0x29" }) end)
if res then
	check(res.DIST_MM == 1234, "DIST_MM = 1234", tostring(res.DIST_MM))
	check(res.STATUS == 0 and res.VALID == 1, "STATUS=0 VALID=1", tostring(res.STATUS))
	check(res.SPAD_NB == 2, "SPAD_NB = 2", tostring(res.SPAD_NB))
	check(res.SIGNAL_KCPS == 800 and res.AMBIENT_KCPS == 400, "SIGNAL=800 AMBIENT=400 kcps",
		tostring(res.SIGNAL_KCPS) .. "/" .. tostring(res.AMBIENT_KCPS))
	check(near(res.SIGMA_MM, 2.0), "SIGMA_MM = 2.0", tostring(res.SIGMA_MM))
end

-- 原始状态映射：0x00 -> 255；0x04 -> 2；有效 SPAD=0 -> 255
regs[0x0089] = 0x00
res = expect_ok("call raw 状态 0x00", function() return call({ fd = 3, address = "0x29" }) end)
if res then check(res.STATUS == 255 and res.VALID == 0, "raw 0 -> STATUS=255 VALID=0", tostring(res.STATUS)) end

regs[0x0089] = 0x04
res = expect_ok("call raw 状态 0x04", function() return call({ fd = 3, address = "0x29" }) end)
if res then check(res.STATUS == 2, "raw 4 -> STATUS=2（映射表）", tostring(res.STATUS)) end

regs[0x0089] = 0x09
regs[0x008C] = 0x00; regs[0x008D] = 0x00		-- SPAD=0 守卫
res = expect_ok("call SPAD=0", function() return call({ fd = 3, address = "0x29" }) end)
if res then check(res.STATUS == 255 and res.VALID == 0, "SPAD=0 -> STATUS=255（ST 同款守卫）", tostring(res.STATUS)) end

-- ==================== call：data-ready 极性与错误通道 ====================
-- 极性翻转：mux bit4=0 -> 有效极性=1，tio bit0=1 -> 就绪（验证判定链）
regs[0x0030] = 0x00; regs[0x0031] = 0x03; regs[0x0089] = 0x09; regs[0x008C] = 0x02
res = expect_ok("call 极性翻转后仍就绪", function() return call({ fd = 3, address = "0x29" }) end)
if res then check(res.DIST_MM == 1234, "翻转极性下读到同一注入值", tostring(res.DIST_MM)) end

-- 未就绪超时：mux bit4=0（极性=1），tio bit0=0 -> 永不就绪
regs[0x0030] = 0x00; regs[0x0031] = 0x02
expect_err("call data-ready 超时", function() return call({ fd = 3, address = "0x29" }) end, "data-ready timeout")

-- 恢复默认就绪态（mux=0x11 极性=0，tio=0x02 bit0=0 -> 就绪）再测总线失败路径
regs[0x0030] = 0x11; regs[0x0031] = 0x02
fail_regs = { [0x0031] = true }
expect_err("call 轮询总线失败", function() return call({ fd = 3, address = "0x29" }) end, "Failed to poll")
fail_regs = { [0x0096] = true }
expect_err("call 读距离失败", function() return call({ fd = 3, address = "0x29" }) end, "range results")
fail_regs = {}

expect_err("call 缺 fd", function() return call({ address = "0x29" }) end, "fd or address")
expect_err("call 参数非表", function() return call("x") end, "Invalid Arguments")

-- ==================== destroy ====================
local sw_before = #start_writes
expect_ok("destroy 停止测距", function() return destroy({ fd = 3, address = "0x29" }) end)
check(#start_writes == sw_before + 1 and start_writes[#start_writes] == 0x00,
	"destroy 写入 SYSTEM_START=0x00", tostring(start_writes[#start_writes]))
expect_ok("destroy 缺 fd 安全返回", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
