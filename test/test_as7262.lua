-- AS7262.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_as7262.lua（AS7262 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效——先注桩会被 HAL 的真函数覆盖
-- （test_pca9685_channel/float.lua 的旧桩模式即踩此坑，两个文件已同步修正）。
--
-- 桩内模拟【间接访问状态机】：I2C 只见 状态0x00/写口0x01/读口0x02 三个硬件
-- 寄存器，芯片侧维护 TX_VALID/RX_VALID 握手与"地址写 -> 数据写"状态序；
-- 写口值不按 bit7 区分命令/数据（驱动写数据字节也可能 >=0x80，如 RST 位），
-- 而按状态机当前位置解释——与真实芯片一致。RST 位写入触发模型复位；
-- 连续模式下 DATA_RDY 清零后立即回置（never_ready 时除外）。

dofile("modules/AS7262.lua")
local bitc = dofile("modules/bitcompat.lua")

-- ==================== 桩与器件模型 ====================
local virtual = {}				-- 虚拟寄存器
local tx_valid, rx_valid = false, false
local waddr, wstate = nil, nil	-- 写命令地址与状态机位置
local raddr = nil				-- 读命令地址
local got_rst = false
local never_ready = false
local stuck_tx = false			-- 置 true 模拟握手挂死（TX_VALID 恒 1）
local fail_write = false
local fail_read = false
local fail_02_after = nil		-- 成功读 0x02 达该次数后置故障（nil = 不启用）
local read02_count = 0
local set_addr_ok = true
local set_addr_log = {}
local sleep_total = 0
local device_type = 0x3E		-- 0x3D=AS7261 / 0x3F=AS7263，用例可改

local function model_reset()
	virtual = {
		[0x00] = device_type,
		[0x01] = 0x40,			-- HW version 占位
		[0x04] = 0x08,			-- 上电默认：BANK=2 连续全通道，GAIN=0，DATA_RDY=0
		[0x05] = 0x00,
		[0x06] = 24,			-- 片上温度占位
		[0x07] = 0x00,
	}
	for a = 0x08, 0x13 do virtual[a] = 0 end
	tx_valid, rx_valid = false, false
	waddr, wstate, raddr = nil, nil, nil
	got_rst = true
end

local function reset_model()
	model_reset()
	never_ready, stuck_tx = false, false
	fail_write, fail_read, set_addr_ok = false, false, true
	fail_02_after, read02_count = nil, 0
	set_addr_log = {}
	sleep_total = 0
	device_type = 0x3E
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr)
	set_addr_log[#set_addr_log + 1] = addr
	return set_addr_ok
end

function i2c_read_reg(fd, reg)
	if fail_read then return -1 end
	if reg == 0x00 then
		if stuck_tx then return 2 end
		return (tx_valid and 2 or 0) + (rx_valid and 1 or 0)
	end
	if reg == 0x02 then
		if fail_02_after and read02_count >= fail_02_after then return -1 end
		read02_count = read02_count + 1
		rx_valid = false
		return virtual[raddr] or 0
	end
	return -1
end

function i2c_write_reg(fd, reg, val)
	if fail_write then return false end
	if reg == 0x01 then
		if wstate == "addr" then
			-- 数据字节（按状态机解释，与 bit7 无关——数据可能 >=0x80）
			wstate = nil
			tx_valid = false
			virtual[waddr] = val % 256
			if waddr == 0x04 then
				if bitc.band(val, 0x80) ~= 0 then
					model_reset()		-- RST 位：软复位
				elseif not never_ready then
					-- 连续模式：DATA_RDY 清零后立即回置（模拟转换完成）
					virtual[0x04] = bitc.bor(virtual[0x04] % 256, 0x02)
				end
			end
		elseif val >= 0x80 then
			waddr = val - 0x80			-- 写命令地址（芯片锁存后即就绪，TX_VALID 不置位）
			wstate = "addr"
		else
			raddr = val				-- 读命令地址，数据立即就绪
			rx_valid = true
		end
	end
	return true
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

local function vreg_is(addr, val, label)
	local extra = string.format("0x%02X=0x%02X", addr, virtual[addr] or 0)
	check(virtual[addr] == val, label .. string.format(" (0x%02X=0x%02X)", addr, val), extra)
end

-- ==================== build：默认参数 ====================
reset_model()
expect_ok("build 默认参数 (gain/integ/timeout 缺省)", function() return build({ fd = 3, address = "0x49" }) end)
check(got_rst, "build 发送软复位 (RST 位)")
vreg_is(0x05, 50, "缺省积分时间 INT_T=50")
check(bitc.band(virtual[0x04], 0x30) == 0x30, "缺省增益 64x ([5:4]=3)",
	string.format("0x%02X", virtual[0x04]))
check(bitc.band(virtual[0x04], 0x0C) == 0x08, "模式 2 连续全通道 ([3:2]=2)",
	string.format("0x%02X", virtual[0x04]))

-- 字符串 fd/address（框架元数据侧全为字符串）
reset_model()
expect_ok("build 字符串 fd='3' address='0x49'", function() return build({ fd = "3", address = "0x49" }) end)
check(#set_addr_log > 0 and set_addr_log[#set_addr_log] == 73,
	"set_addr 收到 0x49=73", tostring(set_addr_log[#set_addr_log]))

-- ==================== build：gain/integ 列 ====================
reset_model()
expect_ok("build gain='16'", function() return build({ fd = 3, address = "0x49", gain = "16" }) end)
check(bitc.band(virtual[0x04], 0x30) == 0x20, "gain=16 -> 代码 2", string.format("0x%02X", virtual[0x04]))

reset_model()
expect_ok("build gain='3.7'", function() return build({ fd = 3, address = "0x49", gain = "3.7" }) end)
check(bitc.band(virtual[0x04], 0x30) == 0x10, "gain=3.7 -> 代码 1", string.format("0x%02X", virtual[0x04]))

reset_model()
expect_ok("build gain='1'", function() return build({ fd = 3, address = "0x49", gain = "1" }) end)
check(bitc.band(virtual[0x04], 0x30) == 0x00, "gain=1 -> 代码 0", string.format("0x%02X", virtual[0x04]))

reset_model()
expect_ok("build gain='20'（就近吸附到 16）", function() return build({ fd = 3, address = "0x49", gain = "20" }) end)
check(bitc.band(virtual[0x04], 0x30) == 0x20, "gain=20 -> 吸附代码 2", string.format("0x%02X", virtual[0x04]))

reset_model()
expect_ok("build integ='10'", function() return build({ fd = 3, address = "0x49", integ = "10" }) end)
vreg_is(0x05, 10, "积分时间 INT_T=10")

reset_model()
expect_ok("build integ='300'（钳到 255）", function() return build({ fd = 3, address = "0x49", integ = "300" }) end)
vreg_is(0x05, 255, "积分时间钳到 255")

-- ==================== build：错误通道 ====================
reset_model()
device_type = 0x3D
expect_err("build 器件甄别不符 (AS7261)", function() return build({ fd = 3, address = "0x49" }) end, "expect 0x3E AS7262")
device_type = 0x3E

reset_model()
stuck_tx = true
expect_err("build 握手挂死", function() return build({ fd = 3, address = "0x49" }) end, "Failed to read AS7262 CONTROL_SETUP")
stuck_tx = false

reset_model()
set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x49" }) end, "set_addr failed")
set_addr_ok = true

expect_err("build 缺 fd", function() return build({ address = "0x49" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")
expect_err("build 参数非表", function() return build(nil) end, "Invalid Arguments Type")

-- ==================== call：六通道读取与温度 ====================
reset_model()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x49" }) end)
-- 注入六通道数据（MSB 偶地址 / LSB 奇地址）与温度
virtual[0x08] = 0x12; virtual[0x09] = 0x34		-- V = 0x1234
virtual[0x0A] = 0x56; virtual[0x0B] = 0x78		-- B = 0x5678
virtual[0x0C] = 0x9A; virtual[0x0D] = 0xBC		-- G = 0x9ABC
virtual[0x0E] = 0xDE; virtual[0x0F] = 0xF0		-- Y = 0xDEF0
virtual[0x10] = 0x11; virtual[0x11] = 0x11		-- O = 0x1111
virtual[0x12] = 0x22; virtual[0x13] = 0x22		-- R = 0x2222
virtual[0x06] = 0x18								-- 温度 +24°C

local res = expect_ok("call 六通道 + 温度", function() return call({ fd = 3, address = "0x49" }) end)
if res then
	check(res.V == 0x1234 and res.B == 0x5678 and res.G == 0x9ABC, "V/B/G 通道拼装", tostring(res.V))
	check(res.Y == 0xDEF0 and res.O == 0x1111 and res.R == 0x2222, "Y/O/R 通道拼装", tostring(res.Y))
	check(res.TEMP_C == 24, "TEMP_C = 24", tostring(res.TEMP_C))
end

-- 二次 call：连续模式下 DATA_RDY 已回置，可继续采样
res = expect_ok("call 连续采样第二拍", function() return call({ fd = 3, address = "0x49" }) end)

-- 负温度（二进制补码）
reset_model()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x49" }) end)
virtual[0x06] = 0xE8			-- -24°C
res = expect_ok("call 负温度", function() return call({ fd = 3, address = "0x49" }) end)
if res then check(res.TEMP_C == -24, "TEMP_C = -24（补码还原）", tostring(res.TEMP_C)) end

-- ==================== call：错误通道 ====================
reset_model()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x49" }) end)

-- 永不就绪：DATA_RDY 清零后不回置
never_ready = true
expect_ok("call 第一拍（存量就绪）", function() return call({ fd = 3, address = "0x49" }) end)
expect_err("call data-ready 超时", function() return call({ fd = 3, address = "0x49" }) end, "data-ready timeout")
never_ready = false

-- 握手挂死
stuck_tx = true
expect_err("call 握手挂死", function() return call({ fd = 3, address = "0x49" }) end, "Failed to poll")
stuck_tx = false

-- 总线失败
fail_read = true
expect_err("call 总线读失败", function() return call({ fd = 3, address = "0x49" }) end, "Failed to poll")
fail_read = false

-- 通道数据读失败：第 1 次 0x02 读（轮询 CONTROL_SETUP）放行，第 2 次（V 通道 MSB）置故障
reset_model()
expect_ok("build 正常", function() return build({ fd = 3, address = "0x49" }) end)
fail_02_after = 1
expect_err("call 读通道失败", function() return call({ fd = 3, address = "0x49" }) end, "Failed to read AS7262 channel V")
fail_02_after = nil

expect_err("call 缺 fd", function() return call({ address = "0x49" }) end, "fd or address")
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
