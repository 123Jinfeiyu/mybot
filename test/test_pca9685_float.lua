-- PCA9685.lua 浮点角度兼容性离线测试（桩替身，不触碰真实 I2C）
-- 用法: lua5.4 test_pca9685_float.lua

local writes = {}

-- 桩：模拟框架注册的 C 函数
function i2c_set_addr(fd, addr) end
function i2c_write_reg(fd, reg, val) writes[#writes + 1] = {reg = reg, val = val} end
function i2c_read_reg(fd, reg) return 0 end
function milli_sleep(ms) end

dofile("modules/PCA9685.lua")

local function try(label, data)
	writes = {}
	local ok, res = pcall(call, data)
	if ok then
		-- 取出 OFF 寄存器低两字节（reg+2, reg+3）合成 offval
		local offval = nil
		for _, w in ipairs(writes) do
			if w.reg == 0x06 + 4 * 6 + 2 then offval = w.val end
			if w.reg == 0x06 + 4 * 6 + 3 and offval then offval = offval + w.val * 256 end
		end
		print(string.format("[PASS] %-28s writes=%d offval=%s result=%s",
			label, #writes, tostring(offval), tostring(res and res.result)))
	else
		print(string.format("[FAIL] %-28s error: %s", label, tostring(res)))
	end
end

-- 1) 当前 Cangjie 路径：整数角度（JsonInt → lua integer）
try("integer angle=45",        {fd = 3, address = "0x40", channel = 6, angle = 45})

-- 2) 迁移后实际路径：Servo.lua 返回经 call_lua 字符串化 → 字符串角度
try("string angle='45'",       {fd = 3, address = "0x40", channel = "6", angle = "45"})
try("string angle='44.7'",     {fd = 3, address = "0x40", channel = "6", angle = "44.7"})

-- 3) 浮点数字角度
try("float angle=44.7",        {fd = 3, address = "0x40", channel = 6, angle = 44.7})

-- 4) 越界角度（驱动边界应限幅到 0~180）
try("string angle='250'",      {fd = 3, address = "0x40", channel = "6", angle = "250"})
try("string angle='-30'",      {fd = 3, address = "0x40", channel = "6", angle = "-30"})
