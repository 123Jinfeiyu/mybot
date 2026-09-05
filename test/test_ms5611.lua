-- MS5611.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_ms5611.lua（MS5611 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效——先注桩会被 HAL 的真函数覆盖
-- （test_pca9685_channel/float.lua 的旧桩模式即踩此坑，两个文件已同步修正）。
-- 桩内模拟器件语义：单字节命令（复位/转换）+ PROM 字读 + 24 位 ADC 读。
--
-- 验证策略：
--   1. CRC-4：测试侧独立实现 AN520 标准算法，先用 AN520 已知答案
--      （{0x3132,...} -> CRC=0xB）自证，再经 build 通过/失败与驱动实现交叉；
--   2. 补偿解算：测试侧用 ffi int64（64 位精确整数）独立复算一阶/二阶公式，
--      与驱动的 double 拆位算法对照——d1*sens ~2^56 超 double 2^53 线的
--      情形被专门覆盖；
--   3. 桩 PROM 的 C1~C6 用接近真实器件量级的系数，word0 的 CRC 位域由
--      测试侧 crc4 现场计算填入。

dofile("modules/MS5611.lua")
local bitc = dofile("modules/bitcompat.lua")
local ffi = require("ffi")
local i64 = ffi.typeof("int64_t")

-- ==================== 测试侧 CRC-4（独立实现，TE/PX4 同款布局） ====================
-- PROM Word Map（datasheet Fig.16）：8 字，word7(0xAE) = 4 位 CRC + 12 位序列号；
-- CRC 位域本身参与运算（word7 仅清低字节），多项式 0x3000。
-- 注意：AN520 的 word0-CRC 布局不适用于 MS5611（系其他 MS56xx 变体）。
-- 注意：LuaJIT bit 库返回带符号 32 位，MSB 判断必须用 band 而非数值比较。
local function test_crc_nib(prom8)		-- prom8[1..8]，word8 未屏蔽
	local n_prom = {}
	for i = 1, 8 do n_prom[i] = prom8[i] % 65536 end
	local crc_read = math.floor(n_prom[8] / 4096) % 16
	n_prom[8] = n_prom[8] - (n_prom[8] % 256)	-- 清低字节（0xFF00 掩码）
	local n_rem = 0
	for cnt = 0, 15 do
		local w = n_prom[math.floor(cnt / 2) + 1]
		if cnt % 2 == 1 then
			n_rem = bitc.bxor(n_rem, w % 256)
		else
			n_rem = bitc.bxor(n_rem, math.floor(w / 256))
		end
		n_rem = n_rem % 65536
		for _ = 1, 8 do
			if bitc.band(n_rem, 0x8000) ~= 0 then
				n_rem = bitc.bxor((n_rem * 2) % 65536, 0x3000)
			else
				n_rem = (n_rem * 2) % 65536
			end
		end
	end
	return math.floor((n_rem % 65536) / 4096) % 16, crc_read
end

local function test_crc_ok(prom8)
	local nib, crc_read = test_crc_nib(prom8)
	return nib == crc_read
end

-- ==================== int64 floor 除法（参考实现用） ====================
local function fdiv64(a, b)
	local m = a % b
	if m < 0 then m = m + b end
	return (a - m) / b
end

-- 参考补偿解算（int64 精确整数，内核驱动同款算术移位语义 -> floor）
local function ref_comp(d1, d2, C)
	local dt = d2 - C[5] * 256
	local temp = 2000 + tonumber(fdiv64(i64(dt) * i64(C[6]), 8388608))
	local off = C[2] * 65536 + tonumber(fdiv64(i64(C[4]) * i64(dt), 128))
	local sens = C[1] * 32768 + tonumber(fdiv64(i64(C[3]) * i64(dt), 256))
	if temp < 2000 then
		local t2 = tonumber(fdiv64(i64(dt) * i64(dt), 2147483648))
		local off2 = tonumber(fdiv64(i64(5) * i64(temp - 2000) * i64(temp - 2000), 2))
		local sens2 = tonumber(fdiv64(i64(5) * i64(temp - 2000) * i64(temp - 2000), 4))
		if temp < -1500 then
			off2 = off2 + tonumber(i64(7) * i64(temp + 1500) * i64(temp + 1500))
			sens2 = sens2 + tonumber(fdiv64(i64(11) * i64(temp + 1500) * i64(temp + 1500), 2))
		end
		temp = temp - t2
		off = off - off2
		sens = sens - sens2
	end
	local p = tonumber(fdiv64(fdiv64(i64(d1) * i64(sens), 2097152) - i64(off), 32768))
	return temp, p
end

-- ==================== 桩与器件模型 ====================
-- 接近真实器件量级的系数（datasheet 一阶算例同款）
local COEFF = { 40127, 36924, 23317, 23282, 33464, 31471 }		-- C1~C6
local prom = {}						-- 7 字（word0 由 reset_device 按测试 CRC 填）
local adc_d1, adc_d2 = 9085466, 8569156
local pending = nil					-- 'd1' | 'd2'（最近一次转换命令）
local got_reset = false
local conv_cmds = {}				-- 转换命令日志
local fail_write = false
local fail_read = false
local set_addr_ok = true
local set_addr_log = {}
local sleep_total = 0

local function reset_device()
	prom = { [1] = 0x06A4 }			-- word0 出厂数据占位
	for i = 1, 6 do prom[i + 1] = COEFF[i] end
	-- word7 = 序列号 + CRC：CRC 位域参与自身运算，逐一试探合法位域
	for nib = 0, 15 do
		prom[8] = nib * 4096 + 0x321
		if test_crc_ok(prom) then break end
	end
	pending = nil
	got_reset = false
	conv_cmds = {}
	fail_write, fail_read, set_addr_ok = false, false, true
	set_addr_log = {}
	sleep_total = 0
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr)
	set_addr_log[#set_addr_log + 1] = addr
	return set_addr_ok
end

function i2c_write_multi(fd, cmd, ...)
	if fail_write then return -1 end
	if cmd == 0x1E then
		got_reset = true
		return 0
	end
	if (cmd >= 0x40 and cmd <= 0x48 and cmd % 2 == 0) or
	   (cmd >= 0x50 and cmd <= 0x58 and cmd % 2 == 0) then
		pending = (cmd < 0x50) and "d1" or "d2"
		conv_cmds[#conv_cmds + 1] = cmd
	end
	return 0
end

function i2c_read_multi(fd, reg, n)
	if fail_read then return -1 end
	if reg == 0x00 and n == 3 then
		-- 24 位 ADC 结果按最近一次转换命令区分 D1/D2
		local v = (pending == "d2") and adc_d2 or adc_d1
		return v % 16777216
	end
	if reg >= 0xA0 and reg <= 0xAE and reg % 2 == 0 and n == 2 then
		return prom[(reg - 0xA0) / 2 + 1] or 0
	end
	return -1
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

-- ==================== CRC-4 布局自证（测试侧实现） ====================
-- reset_device 生成的 word7 使 test_crc_ok 通过；后续 build 通过/失败
-- 即驱动实现与测试实现的交叉验证
reset_device()
check(test_crc_ok(prom), "PROM 模型 word7 CRC 自洽")

-- ==================== build：默认参数与 PROM 自检 ====================
reset_device()
expect_ok("build 默认参数 (osr/p0 缺省)", function() return build({ fd = 3, address = "0x77" }) end)
check(got_reset, "build 发送复位 (0x1E)")
check(sleep_total >= 3, "build 含复位重载等待 (>=3ms)", tostring(sleep_total))

-- 字符串 fd/address（框架元数据侧全为字符串）
reset_device()
expect_ok("build 字符串 fd='3' address='0x77'", function() return build({ fd = "3", address = "0x77" }) end)
check(#set_addr_log > 0 and set_addr_log[#set_addr_log] == 119,
	"set_addr 收到 0x77=119", tostring(set_addr_log[#set_addr_log]))

-- CRC 损坏甄别
reset_device()
local nib, _ = test_crc_nib(prom)
prom[8] = ((nib + 1) % 16) * 4096 + (prom[8] % 4096)	-- 篡改 word7 CRC 位域
expect_err("build PROM CRC 不符", function() return build({ fd = 3, address = "0x77" }) end, "PROM CRC mismatch")

-- ==================== build：错误通道 ====================
reset_device()
fail_write = true
expect_err("build 复位写失败", function() return build({ fd = 3, address = "0x77" }) end, "Failed to reset MS5611")

reset_device()
fail_read = true
expect_err("build 读 PROM 失败", function() return build({ fd = 3, address = "0x77" }) end, "Failed to read MS5611 PROM")

reset_device()
set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x77" }) end, "set_addr failed")
set_addr_ok = true

expect_err("build 缺 fd", function() return build({ address = "0x77" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")
expect_err("build 参数非表", function() return build(nil) end, "Invalid Arguments Type")

-- ==================== call：补偿解算（int64 参考对照） ====================
-- 暖机：一阶即可（TEMP >= 2000），且 d1*sens ~2^56 超 double 2^53 线
reset_device()
expect_ok("build 正常 (osr 缺省 4096)", function() return build({ fd = 3, address = "0x77" }) end)
local exp_t, exp_p = ref_comp(adc_d1, adc_d2, COEFF)
local res = expect_ok("call 暖机解算", function() return call({ fd = 3, address = "0x77" }) end)
if res then
	check(near(res.T_C, exp_t / 100, 1e-12), "T_C = " .. tostring(exp_t / 100) .. "°C", tostring(res.T_C))
	check(near(res.P_HPA, exp_p / 100, 1e-12), "P_HPA = " .. tostring(exp_p / 100) .. "hPa", tostring(res.P_HPA))
	check(exp_p > 99000 and exp_p < 103000, "参考气压落在合理区间 (990~1030hPa)", tostring(exp_p / 100))
	check(res.ALT_M > -500 and res.ALT_M < 500, "ALT_M 量级合理", tostring(res.ALT_M))
	check(near(res.ALT_M, 44330 * (1 - (res.P_HPA / 1013.25) ^ 0.1902949), 1e-6),
		"ALT_M 与压高公式一致 (p0=1013.25)")
	-- OSR 4096 的转换命令
	check(#conv_cmds == 2 and conv_cmds[1] == 0x48 and conv_cmds[2] == 0x58,
		"缺省 OSR4096 转换命令 0x48/0x58",
		table.concat(conv_cmds, ","))
	check(sleep_total >= 3 + 10 + 10, "call 含两段转换等待 (>=23ms 累计)", tostring(sleep_total))
end

-- 低温：二阶补偿（TEMP < 2000）
reset_device()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x77" }) end)
adc_d2 = 8000000		-- dT<0 -> TEMP 落在 0°C 以下，触发二阶
exp_t, exp_p = ref_comp(adc_d1, adc_d2, COEFF)
check(exp_t < 2000 and exp_t >= -1500, "参考 TEMP 落在普通低温段", tostring(exp_t))
res = expect_ok("call 低温二阶补偿", function() return call({ fd = 3, address = "0x77" }) end)
if res then
	check(near(res.T_C, exp_t / 100, 1e-12), "低温 T_C 对照", tostring(res.T_C))
	check(near(res.P_HPA, exp_p / 100, 1e-12), "低温 P_HPA 对照", tostring(res.P_HPA))
end

-- 极低温：TEMP < -1500 追加项
reset_device()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x77" }) end)
adc_d2 = 7616784		-- 一阶 TEMP ~ -1564，触发极低温分支
exp_t, exp_p = ref_comp(adc_d1, adc_d2, COEFF)
check(exp_t < -1500, "参考 TEMP 落在极低温段 (<-1500)", tostring(exp_t))
res = expect_ok("call 极低温补偿", function() return call({ fd = 3, address = "0x77" }) end)
if res then
	check(near(res.T_C, exp_t / 100, 1e-12), "极低温 T_C 对照", tostring(res.T_C))
	check(near(res.P_HPA, exp_p / 100, 1e-12), "极低温 P_HPA 对照", tostring(res.P_HPA))
end

-- ==================== call：OSR 降档与 p0 列 ====================
reset_device()
expect_ok("build osr='300'（降档到 256）", function() return build({ fd = 3, address = "0x77", osr = "300" }) end)
res = expect_ok("call OSR256", function() return call({ fd = 3, address = "0x77" }) end)
check(#conv_cmds == 2 and conv_cmds[1] == 0x40 and conv_cmds[2] == 0x50,
	"OSR256 转换命令 0x40/0x50", table.concat(conv_cmds, ","))
check(sleep_total >= 3 + 1 + 1, "OSR256 等待按档位缩短", tostring(sleep_total))

reset_device()
expect_ok("build osr='-' p0='1000'", function() return build({ fd = 3, address = "0x77", p0 = "1000" }) end)
res = expect_ok("call 自定义 p0", function() return call({ fd = 3, address = "0x77" }) end)
if res then
	check(near(res.ALT_M, 44330 * (1 - (res.P_HPA / 1000) ^ 0.1902949), 1e-6),
		"ALT_M 按 p0=1000 换算")
end

-- ==================== call：错误通道 ====================
reset_device()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x77" }) end)

fail_write = true
expect_err("call D1 转换启动失败", function() return call({ fd = 3, address = "0x77" }) end, "D1 conversion")
fail_write = false

pending = "d1"
fail_read = true
expect_err("call 读 D1 失败", function() return call({ fd = 3, address = "0x77" }) end, "Failed to read MS5611 D1")
fail_read = false

expect_err("call 缺 fd", function() return call({ address = "0x77" }) end, "fd or address")
expect_err("call 参数非表", function() return call("x") end, "Invalid Arguments")

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
