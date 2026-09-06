-- VL53L0X.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_vl53l0x.lua（VL53L0X 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效——先注桩会被 HAL 的真函数覆盖
-- （test_pca9685_channel/float.lua 的旧桩模式即踩此坑，两个文件已同步修正）。
--
-- 桩内模拟【8 位寄存器 + 分页器件】：0xFF 是页选择（0/1/6/7 四页参与序列），
-- 模型只对 page-0 的 0x00 模式写做状态机响应（0x41/0x01 = 参考校准启动即就绪、
-- 0x02 = 背靠背连续启动、0x00 = 停），0x0B=0x01 清中断并让连续模式重新计时；
-- page-7 的 0x83 轮询与 0x92 SPAD 信息按 getSpadInfo 时序模拟。可注入：
-- 错误器件 ID、spad/校准挂死、总线读写失败、永不就绪、任意结果块。
--
-- 关键序列预期值锁定三方参考实现（Pololu VL53L0X.cpp / ESPHome
-- vl53l0x_sensor.cpp 逐条一致，状态映射自 ST vl53l0x_api.cpp）：
--   sig=0.25 (Q9.7 截断) -> 0x44 = 0x0020；long=1 缺省 sig=0.1 -> 0x00 0x0C
--   spad92=0x18 (24 SPAD 非光圈) -> 使能图 {FF,FF,FF,00,00,00}
--   spad92=0x8C (12 SPAD 光圈)   -> 使能图 {00,F0,FF,00,00,00}（跳前 12）
--   结果块状态字节 0x58（内部码 11）-> STATUS=0；0x00->255；0x08->5；
--   0x20->2；0x30->4；0x40->3（ST get_pal_range_status 查表）

dofile("modules/VL53L0X.lua")

-- ==================== 桩与器件模型 ====================
local regs = {}					-- 8 位寄存器平铺存储
local page = 0					-- 0xFF 页选择
local model_id = 0xEE
local stop_var = 0x2B
local spad92 = 0x18				-- SPAD 数 24，非光圈
local continuous_active = false
local countdown = 0				-- 连续模式就绪倒计时（sleep 驱动）
local int_status = 0
local spad_pending = false		-- 0x83=0x00 (page7) 后进入等待
local never_ready = false
local spad_stuck = false
local refcal_stuck = false
local fail_write = false
local fail_read = false
local fail_read_regs = {}
local set_addr_ok = true
local sleep_total = 0
-- 日志
local writes = {}				-- {reg=, vals={...}, multi=}
local sig_writes = {}			-- 0x44 门限 multi 写 {hi, lo}
local map_writes = {}			-- 0xB0 使能图 multi 写
local clear_count = 0
local sysrange = {}				-- page-0 下写 0x00 的模式值序列
local reads_91 = 0
local reads_83_pages = {}		-- 0x83 每次读取所在的页（RMW 必须在 page 6）
local set_addr_log = {}

local function reset_model()
	regs = {}
	regs[0x89] = 0x00
	regs[0x84] = 0x12				-- 极性位预置 1，验证 RMW 清除
	for r = 0xB0, 0xB5 do regs[r] = 0xFF end	-- 上电参考 SPAD 使能图全 1
	page = 0
	model_id = 0xEE
	stop_var = 0x2B
	spad92 = 0x18
	continuous_active = false
	countdown = 0
	int_status = 0
	spad_pending = false
	never_ready = false
	spad_stuck = false
	refcal_stuck = false
	fail_write = false
	fail_read = false
	fail_read_regs = {}
	set_addr_ok = true
	sleep_total = 0
	writes = {}
	sig_writes = {}
	map_writes = {}
	clear_count = 0
	sysrange = {}
	reads_91 = 0
	reads_83_pages = {}
	set_addr_log = {}
end

-- 注入 12 字节结果块（0x14~0x1F）：st=状态字节 spad8.8 sig/amb=9.7 dist=mm
local function inject_result(st, spad_hi, spad_lo, sig_hi, sig_lo, amb_hi, amb_lo, d_hi, d_lo)
	regs[0x14] = st
	regs[0x15] = 0x00
	regs[0x16] = spad_hi; regs[0x17] = spad_lo
	regs[0x18] = 0x00;    regs[0x19] = 0x00
	regs[0x1A] = sig_hi;  regs[0x1B] = sig_lo
	regs[0x1C] = amb_hi;  regs[0x1D] = amb_lo
	regs[0x1E] = d_hi;    regs[0x1F] = d_lo
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr)
	set_addr_log[#set_addr_log + 1] = addr
	return set_addr_ok
end

local function model_write(reg, val, multi)
	if reg == 0xFF then page = val end
	if not multi then
		regs[reg] = val
	else
		for i, v in ipairs(val) do regs[reg + i - 1] = v end
	end
	if reg == 0x00 and page == 0 then
		if val == 0x02 then
			continuous_active = true
			int_status = 0
			countdown = 5
		elseif val == 0x00 then
			continuous_active = false
		elseif val == 0x01 or val == 0x41 then
			if not refcal_stuck then int_status = 0x07 end
		end
		sysrange[#sysrange + 1] = val
	end
	if reg == 0x0B and val == 0x01 then
		clear_count = clear_count + 1
		int_status = 0
		if continuous_active then countdown = 5 end
	end
	if reg == 0x83 and page == 7 then
		if val == 0x00 then spad_pending = true end
		if val == 0x01 then spad_pending = false end
	end
end

function i2c_write_reg(fd, reg, val)
	if fail_write then return false end
	writes[#writes + 1] = { reg = reg, vals = { val }, multi = false }
	model_write(reg, val, false)
	return true
end

function i2c_write_multi(fd, reg, ...)
	if fail_write then return -1 end
	local vs = { ... }
	writes[#writes + 1] = { reg = reg, vals = vs, multi = true }
	if reg == 0x44 then sig_writes[#sig_writes + 1] = { vs[1], vs[2] } end
	if reg == 0xB0 then map_writes[#map_writes + 1] = vs end
	model_write(reg, vs, true)
	return 0
end

function i2c_read_reg(fd, reg)
	if fail_read or fail_read_regs[reg] then return -1 end
	if reg == 0xFF then return page end
	if reg == 0xC0 then return model_id end
	if reg == 0x91 and page == 1 then reads_91 = reads_91 + 1 return stop_var end
	if reg == 0x83 then reads_83_pages[#reads_83_pages + 1] = page end
	if reg == 0x92 and page == 7 then return spad92 end		-- getSpadInfo 在 page 7 读 0x92
	if reg == 0x83 and page == 7 and spad_pending then
		return spad_stuck and 0 or 1
	end
	if reg == 0x13 then
		if never_ready then return 0 end
		if continuous_active and countdown <= 0 then int_status = 0x07 end
		return int_status
	end
	return regs[reg] or 0
end

function i2c_read_multi_bytes(fd, reg, n)
	if fail_read or fail_read_regs[reg] then return nil end
	local t = {}
	for i = 0, n - 1 do t[i + 1] = regs[reg + i] or 0 end
	return t
end

function milli_sleep(ms)
	sleep_total = sleep_total + ms
	if continuous_active then countdown = countdown - ms end
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

local function reg_is(reg, val, label)
	check(regs[reg] == val, label .. string.format(" (0x%02X=0x%02X)", reg, val),
		string.format("0x%02X=0x%02X", reg, regs[reg] or 0))
end

-- 单字节写序列中某寄存器的值序列
local function write_values(reg)
	local t = {}
	for _, w in ipairs(writes) do
		if w.reg == reg and not w.multi then t[#t + 1] = w.vals[1] end
	end
	return t
end

-- ==================== build：默认参数 ====================
reset_model()
expect_ok("build 默认参数", function() return build({ fd = 3, address = "0x29" }) end)
check(write_values(0x89)[1] == 0x01, "VHV EXTSUP_HV RMW 置 bit0", tostring(write_values(0x89)[1]))
reg_is(0x88, 0x00, "I2C standard mode")
check(reads_91 == 1, "舞步读 0x91 取 stop_variable 一次", tostring(reads_91))
check(#reads_83_pages >= 3 and reads_83_pages[1] == 6 and reads_83_pages[#reads_83_pages] == 6,
	"0x83 首末 RMW 读均在 page 6（中间为 page 7 轮询）",
	table.concat(reads_83_pages, ","))
check(write_values(0x60)[1] == 0x12, "MSRC 关限值检查 RMW", tostring(write_values(0x60)[1]))
check(#sig_writes == 1 and sig_writes[1][1] == 0x00 and sig_writes[1][2] == 0x20,
	"信号率门限 0.25 -> Q9.7 0x0020",
	sig_writes[1] and string.format("0x%02X%02X", sig_writes[1][1], sig_writes[1][2]))
local seq01 = write_values(0x01)
check(table.concat(seq01, ",") == "255,248,232,1,2,232",
	"SEQ_CONFIG 序列 0xFF,(tuning 0xF8),0xE8,0x01,0x02,0xE8", table.concat(seq01, ","))
check(#map_writes == 1, "SPAD 使能图单次块写", tostring(#map_writes))
if map_writes[1] then
	check(table.concat(map_writes[1], ",") == "255,255,255,0,0,0",
		"24 SPAD 非光圈 -> {FF,FF,FF,00,00,00}", table.concat(map_writes[1], ","))
end
reg_is(0x30, 0x09, "tuning 0x30 (标准)")
reg_is(0x57, 0x30, "tuning 0x57 (标准)")
reg_is(0x48, 0x28, "tuning 0x48 (标准)")
reg_is(0x71, 0x01, "tuning final timeout HI")
reg_is(0x72, 0xFE, "tuning final timeout LO")
reg_is(0x0A, 0x04, "GPIO 中断条件 = 新样本就绪")
reg_is(0x84, 0x02, "GPIO 极性 RMW 清 bit4")
check(clear_count >= 1, "build 清中断 (GPIO 配置)", tostring(clear_count))
local sr = table.concat(sysrange, ",")
check(sysrange[1] == 0x41 and sysrange[3] == 0x01,
	"参考校准两轮启动 0x41/0x01", sr)
check(sysrange[#sysrange] == 0x02, "末次启动 = 背靠背连续 0x02", sr)
local w91 = {}
for _, w in ipairs(writes) do
	if w.reg == 0x91 and not w.multi then w91[#w91 + 1] = w.vals[1] end
end
check(#w91 == 1 and w91[1] == 0x2B, "启动舞步回写 stop_variable", table.concat(w91, ","))

-- 字符串 fd/address（框架元数据侧全为字符串）
reset_model()
expect_ok("build 字符串 fd='3' address='0x29'", function() return build({ fd = "3", address = "0x29" }) end)
check(#set_addr_log > 0 and set_addr_log[#set_addr_log] == 41,
	"set_addr 收到 0x29=41", tostring(set_addr_log[#set_addr_log]))

-- ==================== build：sig/long 列 ====================
reset_model()
expect_ok("build long='1'（长距模式）", function() return build({ fd = 3, address = "0x29", long = "1" }) end)
reg_is(0x30, 0x07, "长距覆写 0x30")
reg_is(0x57, 0x50, "长距覆写 0x57")
reg_is(0x48, 0x48, "长距覆写 0x48")
check(#sig_writes == 1 and sig_writes[1][1] == 0x00 and sig_writes[1][2] == 0x0C,
	"长距缺省 sig=0.1 -> Q9.7 0x000C",
	sig_writes[1] and string.format("0x%02X%02X", sig_writes[1][1], sig_writes[1][2]))

reset_model()
expect_ok("build sig='1.5'", function() return build({ fd = 3, address = "0x29", sig = "1.5" }) end)
check(sig_writes[1][1] == 0x00 and sig_writes[1][2] == 0xC0,
	"sig=1.5 -> Q9.7 0x00C0",
	string.format("0x%02X%02X", sig_writes[1][1], sig_writes[1][2]))

reset_model()
expect_ok("build sig='-5'（钳 0）", function() return build({ fd = 3, address = "0x29", sig = "-5" }) end)
check(sig_writes[1][1] == 0x00 and sig_writes[1][2] == 0x00,
	"sig<0 钳 0 -> 0x0000", string.format("0x%02X%02X", sig_writes[1][1], sig_writes[1][2]))

reset_model()
expect_ok("build spad92=0x8C（光圈型）", function()
	spad92 = 0x8C
	return build({ fd = 3, address = "0x29" })
end)
if map_writes[1] then
	check(table.concat(map_writes[1], ",") == "0,240,255,0,0,0",
		"12 SPAD 光圈 -> 跳前 12 位 {00,F0,FF,00,00,00}", table.concat(map_writes[1], ","))
end

-- ==================== build：错误通道 ====================
reset_model()
model_id = 0x43
expect_err("build 错误器件 ID", function() return build({ fd = 3, address = "0x29" }) end, "Device type")

reset_model()
fail_read_regs = { [0xC0] = true }
expect_err("build 读 model ID 总线失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to read VL53L0X model ID")

reset_model()
fail_write = true
expect_err("build 写 VHV 失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to write VL53L0X VHV config")

reset_model()
spad_stuck = true
expect_err("build spad info 轮询超时", function() return build({ fd = 3, address = "0x29" }) end, "spad info timeout")
spad_stuck = false

reset_model()
fail_read_regs = { [0x92] = true }
expect_err("build 读 spad info 失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to read VL53L0X spad info")

reset_model()
fail_read_regs = { [0xB0] = true }
expect_err("build 读 SPAD 使能图失败", function() return build({ fd = 3, address = "0x29" }) end, "Failed to read VL53L0X spad enable map")

reset_model()
refcal_stuck = true
expect_err("build 参考校准超时", function() return build({ fd = 3, address = "0x29" }) end, "ref calibration timeout")
refcal_stuck = false

reset_model()
set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x29" }) end, "set_addr failed")
set_addr_ok = true

expect_err("build 缺 fd", function() return build({ address = "0x29" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")
expect_err("build 参数非表", function() return build(nil) end, "Invalid Arguments Type")

-- ==================== call：结果块解析与状态映射 ====================
reset_model()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x29" }) end)
inject_result(0x58, 0x02, 0x00, 0x0C, 0x80, 0x03, 0x00, 0x01, 0x2C)
local res = expect_ok("call 有效测量", function() return call({ fd = 3, address = "0x29" }) end)
if res then
	check(res.DIST_MM == 300, "DIST_MM = 300", tostring(res.DIST_MM))
	check(res.STATUS == 0 and res.VALID == 1, "STATUS=0 VALID=1（内部码 11）", tostring(res.STATUS))
	check(near(res.SPAD_NB, 2.0), "SPAD_NB = 2.0 (8.8 定点)", tostring(res.SPAD_NB))
	check(near(res.SIGNAL_MCPS, 25.0) and near(res.AMBIENT_MCPS, 6.0),
		"SIGNAL=25 AMBIENT=6 MCPS (9.7 定点)",
		tostring(res.SIGNAL_MCPS) .. "/" .. tostring(res.AMBIENT_MCPS))
end

-- 清中断后连续模式重新计时：sleep 5ms 后再次就绪
res = expect_ok("call 连续采样第二拍", function() return call({ fd = 3, address = "0x29" }) end)
if res then check(res.DIST_MM == 300, "第二拍同一注入值", tostring(res.DIST_MM)) end

-- 状态映射：内部码 -> ST 码
local cases = {
	{ 0x00, 255 }, { 0x08, 5 }, { 0x20, 2 }, { 0x30, 4 }, { 0x40, 3 }, { 0x58, 0 },
}
for _, c in ipairs(cases) do
	inject_result(c[1], 0x02, 0x00, 0x0C, 0x80, 0x03, 0x00, 0x01, 0x2C)
	res = expect_ok(string.format("call 状态字节 0x%02X", c[1]), function() return call({ fd = 3, address = "0x29" }) end)
	if res then
		check(res.STATUS == c[2],
			string.format("内部码 %d -> STATUS=%d", math.floor(c[1] / 8), c[2]),
			tostring(res.STATUS))
	end
end

-- 越界判据：DIST >= 8190 -> VALID=0（ESPHome 同款）
inject_result(0x58, 0x02, 0x00, 0x0C, 0x80, 0x03, 0x00, 0x1F, 0xFE)
res = expect_ok("call 距离 8190", function() return call({ fd = 3, address = "0x29" }) end)
if res then check(res.VALID == 0, "8190 -> VALID=0", tostring(res.VALID)) end

-- ==================== call：错误通道 ====================
never_ready = true
expect_err("call data-ready 超时", function() return call({ fd = 3, address = "0x29" }) end, "data-ready timeout")
never_ready = false

fail_read_regs = { [0x13] = true }
expect_err("call 轮询总线失败", function() return call({ fd = 3, address = "0x29" }) end, "Failed to poll")
fail_read_regs = { [0x14] = true }
expect_err("call 读结果块失败", function() return call({ fd = 3, address = "0x29" }) end, "range results")
fail_read_regs = {}

local cc_before = clear_count
fail_write = true
expect_err("call 清中断失败", function() return call({ fd = 3, address = "0x29" }) end, "Failed to clear")
fail_write = false
check(clear_count == cc_before, "失败路径未清中断", tostring(clear_count - cc_before))

expect_err("call 缺 fd", function() return call({ address = "0x29" }) end, "fd or address")
expect_err("call 参数非表", function() return call("x") end, "Invalid Arguments")

-- ==================== destroy ====================
local sr_before = #sysrange
expect_ok("destroy 停止测距", function() return destroy({ fd = 3, address = "0x29" }) end)
check(#sysrange == sr_before + 1 and sysrange[#sysrange] == 0x00,
	"destroy 写入 SYSRANGE_START=0x00", tostring(sysrange[#sysrange]))
expect_ok("destroy 缺 fd 安全返回", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
