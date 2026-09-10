-- MPU6050.lua 16 位补码符号扩展离线测试（桩替身，不触碰真实 I2C）
-- 背景：六轴加速度/角速度为有符号补码，原实现无符号拼字节会把负值
--       读成 65536 附近的大正数；修复后经 s16() 还原为有符号原始值
-- 用法: luajit test_mpu6050_sign.lua（MPU6050 依赖 HAL，需 LuaJIT 的 ffi）

-- 桩：按寄存器地址返回可控字节。六轴寄存器 12 个高低字节地址已知，
-- 每个用例先设定一份「寄存器 → 字节」映射。
local regs = {}

dofile("modules/MPU6050.lua")

-- 驱动 dofile HAL 后再注桩（HAL 真函数会被覆盖，参考 test_pca9685_channel.lua）
local writes
function i2c_set_addr(fd, addr) return true end
-- burst 读桩：reg 起连续 n 字节（1 基表），与 HAL i2c_read_multi_bytes 契约一致
function i2c_read_multi_bytes(fd, reg, n)
	local t = {}
	for i = 0, n - 1 do t[i + 1] = regs[reg + i] or 0 end
	return t
end
function i2c_write_reg(fd, reg, val) end
function milli_sleep(ms) end

-- MPU6050 寄存器地址（与 modules/MPU6050.lua 常量一致）
local A_XH, A_XL = 0x3B, 0x3C
local A_YH, A_YL = 0x3D, 0x3E
local A_ZH, A_ZL = 0x3F, 0x40
local G_XH, G_XL = 0x43, 0x44
local G_YH, G_YL = 0x45, 0x46
local G_ZH, G_ZL = 0x47, 0x48

local fails = 0

local function result_of(ok, res)
	if ok and type(res) == "table" then return tostring(res.result) end
	return tostring(res)
end

-- 用一个轴的 16 位补码设定寄存器的字节高低
local function set_word(hi_reg, lo_reg, word)
	word = word % 65536
	regs[hi_reg] = math.floor(word / 256)
	regs[lo_reg] = word % 256
end

-- 断言某轴读取值与期望有符号值一致
local function expect_sign(label, data, axis, expect)
	local ok, res = pcall(call, data)
	local got = ok and type(res) == "table" and res[axis] or nil
	if ok and got ~= nil and got == expect then
		print(string.format("[PASS] %-28s %s=%d", label, axis, got))
	else
		fails = fails + 1
		print(string.format("[FAIL] %-28s %s=%s (expect %d) result=%s", label,
			axis, tostring(got), expect, result_of(ok, res)))
	end
end

-- 一个用例可以同时设多个轴进行检查
local base = {fd = 3, address = "0x68"}

-- 全零
regs = {}
expect_sign("all zeros", base, "ACCEL_X", 0)
expect_sign("all zeros", base, "GYRO_Z",  0)

-- 正值
regs = {}
set_word(A_XH, A_XL, 0x1234)
set_word(G_ZH, G_ZL, 0x7FFF)
expect_sign("positive accel_x", base, "ACCEL_X", 0x1234)
expect_sign("max positive gyro_z", base, "GYRO_Z", 0x7FFF)

-- 负值（补码）
regs = {}
set_word(A_YH, A_YL, 0xFFFF)  -- -1
set_word(G_XH, G_XL, 0x8000)  -- -32768
expect_sign("negative accel_y=-1", base, "ACCEL_Y", -1)
expect_sign("min negative gyro_x", base, "GYRO_X", -32768)

-- 中间负值
regs = {}
set_word(G_YH, G_YL, 0xFB2E)  -- -1234
set_word(A_ZH, A_ZL, 0x8001)  -- -32767
expect_sign("negative gyro_y=-1234", base, "GYRO_Y", -1234)
expect_sign("negative accel_z=-32767", base, "ACCEL_Z", -32767)

if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end