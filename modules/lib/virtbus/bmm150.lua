-- modules/lib/virtbus/bmm150.lua —— BMM150 三轴地磁传感器虚拟器件模型
-- bot4cj 虚拟总线器件库（无 BUS 字段 = I2C，attach 动态委托 dev.write/dev.read）
--
-- 定位：【8 位字节指针 + 上电挂起态电源门 + 三段工厂 trim NVM + XY/Z 异构
-- 13/15 位数据块】样板（Bosch BMM150，I2C 地址族 0x10/0x11/0x12/0x13 由
-- CSB/SDO 引脚四组合决定，模块常见 0x10/0x11；纯闭包、纯算术无 bitcompat）。
-- 寄存器全按字节寻址（i2c_read_multi_bytes = 写 1B 指针 + 连续读 nB），
-- 读指针随字节自增、0xFF 后回绕 0x00，与 amg8833.lua 同族但五处个性：
--
--   1) 上电序列与常规范式相反（datasheet §4.2.2 挂起态）：上电复位后芯片处
--      suspend，**仅 0x4B 可访问**，其余寄存器读回 0x00（datasheet 原文：
--      挂起态 CHIP_ID(0x40) 读 "0x00"（I2C）/ high-Z（SPI））、写 ACK 丢弃；
--      必须写 0x4B bit0=1（Power Control）进 sleep/active，驱动随后实睡
--      3ms（BMM150_START_UP_TIME）才读 CHIP_ID 与 trim。模型以 powered
--      upvalue 做读/写门（不建 3ms 计时门：接口可访问性只看电源位，3ms
--      是模拟域唤醒，驱动必睡、测试另账断言 sleep_total=3）。
--   2) 0x4B bit7=软复位（自清不落盘）：触发后全部易失寄存器回 POR（数据/
--      配置丢失），同帧低 7 位照落（bit0 决定复位后进 suspend 还是 sleep；
--      Bosch API 的 0x83 = 复位+上电）；trim 是 NVM，软复位后重新装载、
--      不丢失；CHIP_ID 为 ROM 固定 0x32，who() 覆写跨软复位保留。
--   3) 工厂 trim 16 字节三段分散布局（0x5Dx2 / 0x62x4 / 0x68x10），拼装含
--      s8/s16/u16 与 **0x6D 高位掩码**（dig_xyz1 高字节在 0x68+5、bit7
--      保留读恒 0，模型在该字节读回时 %0x80）；补偿管线在驱动侧，模型只
--      存字节，绝不抄 Bosch compensate_x/y/z 公式（避免模型-驱动互证假绿）。
--   4) 数据 0x42 起 8 字节小端位域异构：X/Y = 13 位有符号（LSB bit[7:3]
--      >>3 + MSB s8×32，范围 -4096..4095，-4096=溢出哨兵）；Z = 15 位
--      有符号（LSB bit[7:1] >>1 + MSB s8×128，范围 -16384..16383，
--      -16384=溢出哨兵）；RHALL = 14 位无符号（LSB bit[7:2] >>2 +
--      MSB×64，0..16383）。测量区只读：写 ACK 丢弃，码只能由注入 API 改。
--   5) normal 模式连续出数、无 data-ready 握手（驱动不轮询状态位），故无
--      转换门/无 advance；virt_tick 直接装数据块预武装，守护 build 前打
--      一拍即可，call 恒读到新鲜数据、零真实睡眠。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数，
-- attach 动态委托 dev.write(bytes) 契约）。

local M = {}
M._VERSION = "0.1.0"

-- ==================== 寄存器地址 / 常量 ====================
local REG_CHIP_ID   = 0x40
local REG_DATA      = 0x42		-- 8B：XL,XH,YL,YH,ZL,ZH,RL,RH 终于 0x49
local REG_POWER     = 0x4B		-- bit0=Power Control, bit7=Soft Reset
local REG_OP_MODE   = 0x4C		-- bit[4:3]=ODR, bit[2:1]=工作模式
local REG_REP_XY    = 0x51
local REG_REP_Z     = 0x52
local REG_DIG_X1    = 0x5D		-- trim 段 1：2B（dig_x1 s8, dig_y1 s8）
local REG_DIG_Z4    = 0x62		-- trim 段 2：4B（z4 s16LE, x2 s8, y2 s8）
local REG_DIG_Z2    = 0x68		-- trim 段 3：10B 终于 0x71
local REG_XYZ1_HI   = 0x6D		-- dig_xyz1 高字节，bit7 保留读恒 0

local CHIP_ID       = 0x32
local STARTUP_MS    = 3

-- ODR 码（OP_MODE bit[4:3]）→ Hz（datasheet 表）
local ODR_HZ = { [0] = 10, [1] = 2, [2] = 6, [3] = 8, [4] = 15, [5] = 20, [6] = 25, [7] = 30 }

-- 模型默认工厂 trim：XY 段取 Bosch C 黄金向量 trimA（典型量级），Z 段取
-- 真实量级大值（dig_z2=4000/dig_z1=30000，使 Z 轴 µT 量化步进 <1µT，
-- virt_tick 正弦平滑）。黄金向量单测会显式 set_trim(trimA/trimB) 覆盖。
local DEFAULT_TRIM = {
	dig_x1 = -32, dig_y1 = -3, dig_x2 = -25, dig_y2 = 250,
	dig_z1 = 30000, dig_z2 = 4000, dig_z3 = 0, dig_z4 = 0,
	dig_xy1 = 137, dig_xy2 = 57, dig_xyz1 = 7920,
}

-- virt_tick 32 拍原始码表（默认工厂 trim、rhall=512 离线反解，逐拍命中
-- 目标 µT 误差 0：X 30±20、Y -12±15、Z -42±18，模型内不含补偿公式）。
-- 离线 codegen 用驱动同款 Bosch 管线搜索 -4095..4095 / -16383..16383。
local TICK_FRAMES = {
	{249,22,-54,512},  {270,22,-54,512},  {292,17,-56,512},  {308,3,-61,512},
	{324,-2,-65,512},  {341,-16,-72,512}, {346,-26,-78,512}, {357,-40,-85,512},
	{357,-54,-93,512}, {357,-68,-102,512},{346,-82,-109,512},{341,-92,-115,512},
	{324,-106,-122,512},{308,-111,-126,512},{292,-120,-130,512},{270,-125,-133,512},
	{249,-125,-133,512},{227,-125,-133,512},{206,-120,-130,512},{189,-111,-126,512},
	{173,-106,-122,512},{157,-92,-115,512},{152,-82,-109,512},{141,-68,-102,512},
	{141,-54,-93,512}, {141,-40,-85,512}, {152,-26,-78,512}, {157,-16,-72,512},
	{173,-2,-65,512},  {189,3,-61,512},   {206,17,-56,512},  {227,22,-54,512},
}

-- ==================== 纯算术助手 ====================
local function clampi(v, lo, hi)
	v = math.floor(tonumber(v) or 0)
	if v < lo then return lo elseif v > hi then return hi else return v end
end

-- X/Y 13 位：LSB=低 5 位<<3、MSB=floor(v/32) 字节（驱动 s8(msb)*32+lsb/8）
local function encode_xy(v)
	v = clampi(v, -4096, 4095)
	return (v % 32) * 8, math.floor(v / 32) % 256
end
-- Z 15 位：LSB=低 7 位<<1、MSB=floor(v/128) 字节
local function encode_z(v)
	v = clampi(v, -16384, 16383)
	return (v % 128) * 2, math.floor(v / 128) % 256
end
-- RHALL 14 位无符号：LSB=低 6 位<<2、MSB=floor(r/64) 字节
local function encode_r(r)
	r = clampi(r, 0, 16383)
	return (r % 64) * 4, math.floor(r / 64) % 256
end

local function s8(v)
	v = v % 256
	if v >= 0x80 then v = v - 256 end
	return v
end

-- trim 表 → 16 字节 NVM 布局（拼装顺序严格同驱动 read_trim）
local function trim_bytes(t)
	return {
		[0x5D] = { t.dig_x1 % 256, t.dig_y1 % 256 },
		[0x62] = { t.dig_z4 % 256, math.floor(t.dig_z4 / 256) % 256,
			t.dig_x2 % 256, t.dig_y2 % 256 },
		[0x68] = { t.dig_z2 % 256, math.floor(t.dig_z2 / 256) % 256,
			t.dig_z1 % 256, math.floor(t.dig_z1 / 256) % 256,
			t.dig_xyz1 % 256, math.floor(t.dig_xyz1 / 256) % 128,
			t.dig_z3 % 256, math.floor(t.dig_z3 / 256) % 256,
			t.dig_xy2 % 256, t.dig_xy1 % 256 },
	}
end

function M.new()
	local d = {}

	-- 芯片侧状态
	local regs = {}				-- 字节寄存器影子（稀疏，缺省读 0）
	local read_ptr = nil		-- 最近一笔指针写（read 回调据此起 burst）
	local powered = false		-- 0x4B bit0：false=suspend（仅 0x4B 可访问）
	local chip_id = CHIP_ID		-- ROM 身份（who 可覆写，软复位不恢复）
	local trim = DEFAULT_TRIM	-- NVM 当前内容（软复位不丢）
	local reset_count = 0		-- 软复位（写 0x4B bit7）次数
	local write_count = 0		-- ≥2B 写事务笔数
	local write_log = {}		-- ≥2B 写事务 {reg=首地址, bytes=...}

	-- 故障钩子（reset 保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	-- ---------------- 地址派生 ----------------

	local function in_trim(a)
		return a == 0x5D or a == 0x5E
			or (a >= 0x62 and a <= 0x65)
			or (a >= 0x68 and a <= 0x71)
	end
	local function in_data(a) return a >= REG_DATA and a <= 0x49 end

	-- ---------------- POR / 软复位 ----------------

	local function seed_body()
		regs = {}
		regs[0x4B] = 0x00			-- 仅控制寄存器在挂起态有内容
		regs[REG_CHIP_ID] = chip_id
		-- trim NVM 影子（挂起态不可读，上电后可读）
		for base, bytes in pairs(trim_bytes(trim)) do
			for i = 1, #bytes do regs[base + i - 1] = bytes[i] end
		end
		-- 0x42~0x49 数据区 / 0x4C / 0x51 / 0x52 等易失寄存器不播种，读 0
	end

	local function power_off_reset()
		-- 软复位 / 进挂起：易失内容全丢（datasheet：除 0x4B 外全部丢失），
		-- NVM trim 与 ROM 身份保留，重新上电后 trim 仍在
		seed_body()
		read_ptr = nil
	end

	seed_body()

	-- ---------------- 总线回调（点定义，无 self） ----------------

	local function write_byte(a, v)
		v = v % 256
		if a == REG_POWER then
			-- 进挂起（bit0 1→0）或软复位（bit7）：易失内容立即丢失
			-- （datasheet：挂起态除 0x4B 外全部 lose content），NVM/ROM 保留
			local lose = (v >= 0x80) or (powered and v % 2 == 0)
			if v >= 0x80 then
				reset_count = reset_count + 1
				v = v % 0x80			-- bit7 自清不落盘，低 7 位照落
			end
			if lose then power_off_reset() end
			regs[REG_POWER] = v
			powered = (v % 2 == 1)
			return
		end
		if not powered then
			-- 挂起态仅 0x4B 可访问：其余写 ACK 但丢弃（寄存器无内容）
			return
		end
		if a == REG_CHIP_ID or in_data(a) or in_trim(a) then
			-- ROM/只读测量区/NVM：写 ACK 丢弃，测量码只能由注入 API 改
			return
		end
		regs[a] = v
	end

	function d.write(bytes)
		if type(bytes) ~= "table" then return false end
		if d.nack or d.nack_write then return false end
		local nb = #bytes
		if nb == 0 then return false end
		if write_pred and write_pred(bytes) then return false end
		if nb == 1 then
			-- 纯指针写（读族事务的指针相位）：任意地址 ACK 设 ptr
			if bytes[1] > 0xFF then return false end
			read_ptr = bytes[1]
			return true
		end
		if bytes[1] > 0xFF then return false end
		for i = 2, nb do
			local b = bytes[i]
			if type(b) ~= "number" or b < 0 or b > 0xFF then return false end
			write_byte((bytes[1] + i - 2) % 256, b)
		end
		write_count = write_count + 1
		local copy = { bytes[1] }
		for i = 2, nb do copy[i] = bytes[i] end
		write_log[#write_log + 1] = { reg = bytes[1], bytes = copy }
		return true
	end

	function d.read(n)
		if d.nack or d.nack_read then return nil end
		if read_ptr == nil then return nil end	-- 无指针纯读：NACK
		n = math.floor(tonumber(n) or 1)
		if n < 1 then n = 1 elseif n > 16 then n = 16 end
		if read_pred and read_pred(read_ptr, n) then return nil end
		local t = {}
		for i = 1, n do
			local a = read_ptr
			local v
			if not powered and a ~= REG_POWER then
				v = 0					-- 挂起态：CHIP_ID/trim/数据全读 0x00
			else
				v = regs[a] or 0
				if a == REG_XYZ1_HI then v = v % 0x80 end	-- 保留位 bit7 读 0
			end
			t[i] = v
			read_ptr = (read_ptr + 1) % 256		-- burst 自增、0xFF 回绕
		end
		return t
	end

	-- ---------------- 测试 / 守护注入 API（冒号定义） ----------------

	-- 数据块：有符号原始 LSB（XY -4096..4095，Z -16384..16383，RHALL 0..16383）
	function d:set_data(x, y, z, rhall)
		local xl, xh = encode_xy(x)
		local yl, yh = encode_xy(y)
		local zl, zh = encode_z(z)
		local rl, rh = encode_r(rhall)
		regs[0x42], regs[0x43] = xl, xh
		regs[0x44], regs[0x45] = yl, yh
		regs[0x46], regs[0x47] = zl, zh
		regs[0x48], regs[0x49] = rl, rh
		return self
	end
	function d:set_x_raw(v) regs[0x42], regs[0x43] = encode_xy(v); return self end
	function d:set_y_raw(v) regs[0x44], regs[0x45] = encode_xy(v); return self end
	function d:set_z_raw(v) regs[0x46], regs[0x47] = encode_z(v); return self end
	function d:set_rhall(v) regs[0x48], regs[0x49] = encode_r(v); return self end

	-- 装载工厂 trim（16B NVM 三段）；nil 或缺字段按 0 处理
	function d:set_trim(t)
		if type(t) ~= "table" then return self end
		local z = {}
		for k, v in pairs(DEFAULT_TRIM) do z[k] = v end
		for k, v in pairs(t) do z[k] = v end
		trim = z
		for base, bytes in pairs(trim_bytes(trim)) do
			for i = 1, #bytes do regs[base + i - 1] = bytes[i] end
		end
		return self
	end

	-- ROM 身份覆写（喂 build 错 ID 负路径；nil=不改，软复位不恢复）
	function d:who(id)
		if id ~= nil then
			chip_id = math.floor(tonumber(id) or 0) % 256
			regs[REG_CHIP_ID] = chip_id
		end
		return self
	end

	-- 守护逐拍造数：32 拍地磁原始码正弦直接装入数据块（无转换门概念，
	-- build 前 virt_tick(0) 即预武装），call 零真实睡眠
	function d:virt_tick(n)
		local f = TICK_FRAMES[(math.floor(tonumber(n) or 0) % 32) + 1]
		d:set_data(f[1], f[2], f[3], f[4])
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred) read_pred = pred; return self end

	function d:clear_faults()
		d.nack, d.nack_read, d.nack_write = false, false, false
		write_pred, read_pred = nil, nil
		return self
	end

	function d:reset_logs()
		write_count, reset_count = 0, 0
		write_log = {}
		return self
	end

	-- 重新上电：全状态回 POR（默认 trim/CHIP_ID/挂起态）；故障钩子保留
	function d:reset()
		chip_id = CHIP_ID
		trim = DEFAULT_TRIM
		powered = false
		write_count, reset_count = 0, 0
		write_log = {}
		read_ptr = nil
		seed_body()
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:x_raw() return s8(regs[0x43] or 0) * 32 + math.floor((regs[0x42] or 0) / 8) end
	function d:y_raw() return s8(regs[0x45] or 0) * 32 + math.floor((regs[0x44] or 0) / 8) end
	function d:z_raw() return s8(regs[0x47] or 0) * 128 + math.floor((regs[0x46] or 0) / 2) end
	function d:rhall() return (regs[0x49] or 0) * 64 + math.floor((regs[0x48] or 0) / 4) end

	function d:trim()
		local z = {}
		for k, v in pairs(trim) do z[k] = v end
		return z
	end
	function d:chip_id() return chip_id end
	function d:powered() return powered end
	function d:ptr() return read_ptr end
	function d:rep_xy() return regs[REG_REP_XY] or 0 end
	function d:rep_z() return regs[REG_REP_Z] or 0 end
	function d:op_mode() return regs[REG_OP_MODE] or 0 end
	function d:odr_code() return math.floor((regs[REG_OP_MODE] or 0) / 8) % 8 end
	function d:odr_hz() return ODR_HZ[d:odr_code()] end
	function d:mode_code() return math.floor((regs[REG_OP_MODE] or 0) / 2) % 4 end
	function d:reset_count() return reset_count end
	function d:write_count() return write_count end
	function d:writes() return write_log end
	function d:reg(a) return regs[math.floor(tonumber(a) or 0) % 256] end

	return d
end

return M
