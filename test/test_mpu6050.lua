-- MPU6050.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_mpu6050.lua（MPU6050 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 驱动在 build/call 时才解析全局名，后注桩生效（test_pca9685_channel 踩过的坑，见
-- test_ms5611.lua 头注释）。
--
-- 覆盖点（对照 modules/MPU6050.lua 当前实现）：
--   1. build：参数校验、默认地址 0x68、寄存器配置序列（PWR_MGMT_1=0x6B 退睡眠、
--      陀螺 0x1B=±500°/s、加速度 0x1C=±8g）、100ms 稳定等待、错误通道；
--   2. call：六轴 16 位补码符号扩展（正/负/零/极值），返回键名与量程无换算；
--   3. destroy：参数校验与成功返回；
--   4. 字符串 fd/address（框架元数据侧全为字符串）兼容。

dofile("modules/MPU6050.lua")

-- ==================== 桩与器件模型 ====================
-- 后注桩（先定义会被 HAL 真函数覆盖）。regs 模拟「寄存器 -> 字节」。
local regs = {}
local writes = {}        -- 写寄存器序列 {reg, val}
local set_addr_log = {}  -- set_addr 收到的地址序列
local sleep_total = 0
local fail_write = false
local fail_read = false
local set_addr_ok = true

function i2c_set_addr(fd, addr)
	set_addr_log[#set_addr_log + 1] = addr
	return set_addr_ok
end

function i2c_write_reg(fd, reg, val)
	if fail_write then return -1 end
	writes[#writes + 1] = { reg = reg, val = val }
	return 0
end

function i2c_read_reg(fd, reg)
	if fail_read then return -1 end
	return regs[reg] or 0
end

function milli_sleep(ms) sleep_total = sleep_total + ms end

-- ==================== 寄存器常量（与驱动一致） ====================
local PWR_MGMT_1     = 0x6B
local PWR_NOSLEEP    = 0x00
local GYRO_CONF_REG  = 0x1B
local GYRO_FS_500DPS = 0x08
local ACCEL_CONF_REG = 0x1C
local ACCEL_FS_8g    = 0x10

local A_XH, A_XL = 0x3B, 0x3C
local A_YH, A_YL = 0x3D, 0x3E
local A_ZH, A_ZL = 0x3F, 0x40
local G_XH, G_XL = 0x43, 0x44
local G_YH, G_YL = 0x45, 0x46
local G_ZH, G_ZL = 0x47, 0x48

-- 用 16 位补码设定一个轴的高/低字节
local function set_word(hi_reg, lo_reg, word)
	word = word % 65536
	regs[hi_reg] = math.floor(word / 256)
	regs[lo_reg] = word % 256
end

local base = { fd = 3, address = "0x68" }

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

-- 断言某轴读取的符号扩展结果
local function expect_axis(label, data, axis, expect)
	local ok, res = pcall(call, data)
	local got = ok and type(res) == "table" and res[axis] or nil
	check(ok and got ~= nil and got == expect, label .. " " .. axis .. "=" .. tostring(expect),
		"got=" .. tostring(got) .. " result=" .. result_of(ok, res))
end

-- ==================== build：寄存器配置 ====================
writes = {}
sleep_total = 0
set_addr_log = {}
regs = {}
expect_ok("build 默认参数 (fd=3, address='0x68')", function() return build(base) end)

check(#writes == 3, "build 恰好写 3 个配置寄存器", tostring(#writes))
check(writes[1].reg == PWR_MGMT_1 and writes[1].val == PWR_NOSLEEP,
	"build 写 PWR_MGMT_1 退睡眠 (0x6B<-0x00)",
	string.format("reg=0x%02X val=0x%02X", writes[1] and writes[1].reg or -1, writes[1] and writes[1].val or -1))
check(writes[2].reg == GYRO_CONF_REG and writes[2].val == GYRO_FS_500DPS,
	"build 写陀螺量程 ±500°/s (0x1B<-0x08)",
	string.format("reg=0x%02X val=0x%02X", writes[2] and writes[2].reg or -1, writes[2] and writes[2].val or -1))
check(writes[3].reg == ACCEL_CONF_REG and writes[3].val == ACCEL_FS_8g,
	"build 写加速度量程 ±8g (0x1C<-0x10)",
	string.format("reg=0x%02X val=0x%02X", writes[3] and writes[3].reg or -1, writes[3] and writes[3].val or -1))
check(sleep_total == 101, "build 含 1ms 起步 + 100ms 稳定等待 (累计 101ms)", tostring(sleep_total))
check(#set_addr_log > 0 and set_addr_log[#set_addr_log] == 0x68,
	"set_addr 收到 0x68=104", tostring(set_addr_log[#set_addr_log]))

-- 非默认地址 0x69
set_addr_log = {}
expect_ok("build 地址 '0x69'", function() return build({ fd = 3, address = "0x69" }) end)
check(set_addr_log[#set_addr_log] == 0x69, "set_addr 收到 0x69=105", tostring(set_addr_log[#set_addr_log]))

-- ==================== build：错误通道 ====================
-- NOTE: 当前驱动不检查 i2c_write_reg 返回值，I/O 写失败不会触发 ERR。
-- fd/address 统一走 drv_common.i2c_pair（消息 "Invalid Argument: fd or address"）。
expect_err("build 缺 fd", function() return build({ address = "0x68" }) end, "Invalid Argument: fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "Invalid Argument: fd or address")
expect_err("build 参数非表", function() return build(nil) end, "Invalid Arguments Type")

-- ==================== call：符号扩展与返回契约 ====================
-- 全零
regs = {}
expect_ok("call 全零返回 OK", function() return call(base) end)
expect_axis("call 全零", base, "ACCEL_X", 0)
expect_axis("call 全零", base, "GYRO_Z",  0)

-- 正值
regs = {}
set_word(A_XH, A_XL, 0x1234)
set_word(G_ZH, G_ZL, 0x7FFF)
expect_axis("call 正加速度 x=0x1234", base, "ACCEL_X", 0x1234)
expect_axis("call 最大正角速度 z=0x7FFF", base, "GYRO_Z", 0x7FFF)

-- 负值（补码还原）
regs = {}
set_word(A_YH, A_YL, 0xFFFF)  -- -1
set_word(G_XH, G_XL, 0x8000)  -- -32768
expect_axis("call 负加速度 y=-1", base, "ACCEL_Y", -1)
expect_axis("call 最小负角速度 x=-32768", base, "GYRO_X", -32768)

-- 中间负值
regs = {}
set_word(G_YH, G_YL, 0xFB2E)  -- -1234
set_word(A_ZH, A_ZL, 0x8001)  -- -32767
expect_axis("call 负角速度 y=-1234", base, "GYRO_Y", -1234)
expect_axis("call 负加速度 z=-32767", base, "ACCEL_Z", -32767)

-- 六轴全部同时读到（一次 call 填满 12 个寄存器）
regs = {}
set_word(A_XH, A_XL, 0x0001)
set_word(A_YH, A_YL, 0x0002)
set_word(A_ZH, A_ZL, 0x0003)
set_word(G_XH, G_XL, 0xFFFC)  -- -4
set_word(G_YH, G_YL, 0xFFFD)  -- -3
set_word(G_ZH, G_ZL, 0xFFFE)  -- -2
local ok, res = pcall(call, base)
check(ok and res.ACCEL_X == 1 and res.ACCEL_Y == 2 and res.ACCEL_Z == 3
	and res.GYRO_X == -4 and res.GYRO_Y == -3 and res.GYRO_Z == -2 and res.result == "OK",
	"六轴同时正确（1,2,3,-4,-3,-2）",
	ok and table.concat({ res.ACCEL_X, res.ACCEL_Y, res.ACCEL_Z, res.GYRO_X, res.GYRO_Y, res.GYRO_Z }, ",") or result_of(ok, res))

-- 量程无换算：直接返回寄存器原始值（不做 16384/65.5 除法），键名用驱动输出名
local raw_ok, raw = pcall(call, base)
check(raw_ok and raw and raw.result == "OK" and raw.ACCEL_X ~= nil and raw.GYRO_X ~= nil,
	"call 返回键名 ACCEL_*/GYRO_* 与 result 契约", raw_ok and tostring(raw and raw.result) or result_of(raw_ok, raw))

-- ==================== call：错误通道 ====================
-- NOTE: 当前驱动不检查 i2c_read_reg 返回值，I/O 读失败仅得到 0，不会触发 ERR。
-- fd/address 统一走 drv_common.i2c_pair（消息 "Invalid Argument: fd or address"），
-- 非表参数经 arg_table（消息 "Invalid Arguments Type: table"）。
expect_err("call 缺 fd", function() return call({ address = "0x68" }) end, "Invalid Argument: fd or address")
expect_err("call 缺 address", function() return call({ fd = 3 }) end, "Invalid Argument: fd or address")
expect_err("call 参数非表", function() return call("x") end, "Invalid Arguments Type")

-- ==================== destroy ====================
expect_ok("destroy 正常", function() return destroy(base) end)
expect_err("destroy 缺 fd", function() return destroy({ address = "0x68" }) end, "Invalid Argument")
expect_err("destroy 参数非表", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
