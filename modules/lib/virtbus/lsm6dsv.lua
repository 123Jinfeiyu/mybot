-- modules/lib/virtbus/lsm6dsv.lua —— ST LSM6DSV16X 六轴 IMU 虚拟器件模型
-- bot4cj 虚拟总线器件库（M.BUS="i2c"，attach 动态委托 dev.write/dev.read）
--
-- 定位：【8 位字节指针 + CTRL3 软复位/IF_INC 门 + 双 6 字节小端连续测量窗】
-- 样板（ST LSM6DSV16X，I2C 0x6A/0x6B（SA0），WHO_AM_I(0x0F)=0x70；
-- 0x6C 是老 LSM6DSO 系，驱动明确拒绝）。寄存器全按字节寻址
-- （i2c_read_reg/i2c_read_multi = 写 1B 指针 + 读；i2c_write_reg = 写
-- 2B {ptr,val}），读指针模 256 回绕，与 bmi270/emc2101/bmm150 同族；
-- 但比 BMI270 简单得多：基础六轴【不需要上传任何固件 blob】，无转换门/
-- 无 advance——配置 ODR 后芯片连续测量，数据常驻输出寄存器，virt_tick
-- 直接装码，守护 call 零睡眠。LSM6DSV16X 相对老 ST 芯片（LSM6DSO 系）
-- 带来四条协议事实（ST 官方 lsm6dsv16x_reg.h + Zephyr dts 绑定逐条钉死，
-- 迁移同时抓出真实驱动一处地址 bug，见文件末「迁移订正」）：
--
--   1) **CTRL3(0x12) 三位语义**：bit0 sw_reset 写 1 触发全片软复位且
--      【写即自清】（模型立即 reseed 回 POR、落盘值不含 bit0，驱动首拍
--      10ms 轮询即见 0 跳出，10 拍上限喂超时负路径）；bit7 BOOT 同样
--      自清（boot_count 计数，只重装修调值、不动寄存器地图，模型不
--      reseed）；bit2 IF_INC=0 时【多字节读重复同址】、=1 读指针才随
--      字节自增——驱动 build 中先把 BDU(bit6)|IF_INC 置 0x44，此后
--      六轴 burst 才正确跨轴；bit2=0 的重复读在模型里显式建模。
--   2) **量程域挪出 CTRL1/CTRL2**：加计 fs_xl 在 CTRL8(0x17)[1:0]
--      （高 6 位是 dualC/滤波域，驱动 RMW 保 0xFC）；陀螺 fs_g 在
--      CTRL6(0x15)[3:0]，±4000dps 的码是【不连续的 0xC】（0..4 =
--      125/250/500/1000/2000dps，5..11/13..15 保留）。0x18 是
--      CTRL9（usr_off/lpf2/hp 各位），不是量程寄存器——真实驱动
--      曾把 REG_CTRL8 写成 0x18，加计量程在真机永远 POR ±2g。
--   3) **小端双窗、陀螺在前**：OUTX_L_G=0x22 起 6 字节陀螺、
--      OUTX_L_A=0x28 起 6 字节加计（与 BMI270「加速计在前」相反），
--      每轴 LSB 在前。两窗只读：写 ACK 丢弃，读时由注入的 s16 码
--      现合成字节。STATUS(0x1E) 驱动不读，模型按 ODR 域动态合成
--      bit0 XLDA / bit1 GDA（ODR 码非 0 = 连续测量常开）。
--   4) **ODR 全新统一码表**：CTRL1[3:0]/CTRL2[3:0] = 0x2..0xC 对应
--      7.5..7680Hz（老芯片 104/208/833Hz 档已废），op_mode 高 3 位
--      =0 高性能；取档/换算全在驱动与测试侧，模型只存原始码。
--
-- 【边界约定】模型只存协议事实与原始码：g/dps 换算公式不转录进模型，
-- 物理量注入（set_accel_g/set_gyro_dps）仅按当前 fs 档灵敏度倒数反算
-- 原始码（常数取自驱动头注释 ST 标称灵敏度，独立交叉）。软复位重种
-- 寄存器但保留 who 覆写/注入码/故障钩子（注入代表测试环境，守护
-- build 的软复位不能解除 tick(0) 预武装）；仅上电 reset() 回出厂。
-- virt_tick 32 拍正弦为确定性离线码钉（禁 math.random）。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数）。
-- LuaJIT 兼容：无 5.4 位运算符、无 //，位场一律 floor/% 算术。

local M = {}
M._VERSION = "0.1.0"
M.BUS = "i2c"

-- ==================== 寄存器地址 / 协议常量 ====================
local R_WHO_AM_I = 0x0F		-- ROM 身份 0x70
local R_CTRL1    = 0x10		-- [3:0]=odr_xl [6:4]=op_mode_xl
local R_CTRL2    = 0x11		-- [3:0]=odr_g  [6:4]=op_mode_g
local R_CTRL3    = 0x12		-- bit0 sw_reset(自清) bit2 if_inc bit6 bdu bit7 boot(自清)
local R_CTRL6    = 0x15		-- [3:0]=fs_g（0xC=±4000dps）[6:4]=lpf1_g_bw
local R_CTRL8    = 0x17		-- [1:0]=fs_xl bit3=xl_dualc_en [7:5]=hp_lpf2_xl_bw
local R_CTRL9    = 0x18		-- usr_off/lpf2/hp_slope 等使能位（非量程！）
local R_STATUS   = 0x1E		-- bit0=XLDA bit1=GDA（读时动态合成）
local R_OUTX_L_G = 0x22		-- 陀螺 6 字节小端 0x22..0x27（在前）
local R_OUTX_L_A = 0x28		-- 加计 6 字节小端 0x28..0x2D

local CHIP_ID = 0x70

-- 量程码 → LSB/物理量（ST 标称灵敏度 mg 或 mdps/LSB 的倒数；物理量逆
-- 注入反算用，模型不做正方向换算）。±2/4/8/16g = 0.061/0.122/0.244/
-- 0.488 mg/LSB；±125..4000dps = 4.375/8.75/17.5/35/70/140 mdps/LSB
local ACC_LSB_PER_G = {
	1 / 0.000061,	-- fs_xl=0 ±2g
	1 / 0.000122,	-- fs_xl=1 ±4g
	1 / 0.000244,	-- fs_xl=2 ±8g（缺省）
	1 / 0.000488,	-- fs_xl=3 ±16g
}
local ACC_G = { 2, 4, 8, 16 }

-- fs_g 码不连续：[0..4]=125/250/500/1000/2000dps，[0xC]=4000dps
local GYR_LSB_PER_DPS = {
	[0x0] = 1 / 0.004375,
	[0x1] = 1 / 0.00875,
	[0x2] = 1 / 0.0175,		-- 缺省 ±500dps
	[0x3] = 1 / 0.035,
	[0x4] = 1 / 0.070,
	[0xC] = 1 / 0.140,
}
local GYR_DPS = { [0x0] = 125, [0x1] = 250, [0x2] = 500, [0x3] = 1000,
                  [0x4] = 2000, [0xC] = 4000 }
local GYR_CODE_DEFAULT = 0x2

local function clampi(v, lo, hi)
	v = math.floor(tonumber(v) or 0)
	if v < lo then return lo elseif v > hi then return hi else return v end
end

local function is_byte(v)
	return type(v) == "number" and v == math.floor(v) and v >= 0 and v <= 255
end

-- 四舍五入到远离零的整数（负物理量造码用）
local function round_away(x)
	if x >= 0 then return math.floor(x + 0.5) end
	return -math.floor(-x + 0.5)
end

function M.new()
	local d = {}

	-- 芯片侧状态
	local regs = {}				-- 稀疏字节寄存器影子（POR 全 0，未定义地址读 0）
	local read_ptr = nil		-- 最近一笔指针写（read 回调据此起 burst）
	local chip_id = CHIP_ID		-- ROM 身份（who 覆写跨软复位保留）

	-- 六轴测量码（s16，仅注入 API 可改；软复位保留，reset 清零）
	local ax, ay, az = 0, 0, 0
	local gx, gy, gz = 0, 0, 0

	local reset_count = 0		-- CTRL3 sw_reset 次数
	local boot_count = 0		-- CTRL3 BOOT 次数
	local reset_stuck = false	-- true：CTRL3 bit0 读恒 1（喂 10 拍轮询超时）
	local write_count = 0		-- ≥2B 写事务笔数
	local read_count = 0		-- 读事务笔数
	local frames = {}			-- ≥2B 写事务副本

	-- 故障钩子（软复位保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	-- ---------------- POR / 软复位 ----------------

	-- POR：除 ROM WHO_AM_I 外全部配置寄存器 0x00（官方 register map RESET
	-- 列：CTRL1/2/3/6/CTRL8/CTRL9 均 0；IF_INC 上电态不影响 build——驱动
	-- 复位后显式 RMW 置 0x44，取 0 与「无突发」的安全语义一致）。测量码/
	-- 身份/钩子不在此清。
	local function seed_body()
		regs = {}
	end

	-- 软复位：重种寄存器态；注入码/身份覆写/故障钩子保留
	local function soft_reset()
		seed_body()
		reset_count = reset_count + 1
	end

	-- ---------------- 地址解码 ----------------

	local function status_byte()
		-- 连续模式：ODR 域非 0（非断电档）即数据常开
		local v = 0
		if (regs[R_CTRL1] or 0) % 16 ~= 0 then v = v + 1 end	-- XLDA
		if (regs[R_CTRL2] or 0) % 16 ~= 0 then v = v + 2 end	-- GDA
		return v
	end

	local function meas_byte(a)
		-- 陀螺窗 0x22..0x27、加计窗 0x28..0x2D，各 3 轴小端
		local i, v
		if a >= R_OUTX_L_G and a <= R_OUTX_L_G + 5 then
			i = a - R_OUTX_L_G
			if i < 2 then v = gx elseif i < 4 then v = gy else v = gz end
		else
			i = a - R_OUTX_L_A
			if i < 2 then v = ax elseif i < 4 then v = ay else v = az end
		end
		if i % 2 == 0 then return v % 256 end			-- LSB 在前
		return math.floor(v / 256) % 256				-- MSB
	end

	local function byte_at(a)
		if a == R_WHO_AM_I then return chip_id end
		if a >= R_OUTX_L_G and a <= R_OUTX_L_G + 5 then return meas_byte(a) end
		if a >= R_OUTX_L_A and a <= R_OUTX_L_A + 5 then return meas_byte(a) end
		if a == R_STATUS then return status_byte() end
		if a == R_CTRL3 and reset_stuck then
			return (regs[R_CTRL3] or 0) - (regs[R_CTRL3] or 0) % 2 + 1
		end
		return regs[a] or 0
	end

	-- ---------------- 单字节落盘（地址已折入 0x00..0xFF） ----------------

	local function write_byte(a, v)
		if a == R_WHO_AM_I then return end			-- ROM 只读
		if a >= R_OUTX_L_G and a <= R_OUTX_L_A + 5 then
			return									-- 测量窗只读：写 ACK 丢弃
		end
		if a == R_STATUS then return end
		if a == R_CTRL3 then
			if v % 2 == 1 then
				-- sw_reset：全片立即回 POR，bit0 写即自清、不落盘；
				-- 同帧其余位随复位作废（真机复位后寄存器地图归 POR）
				soft_reset()
				return
			end
			if math.floor(v / 128) == 1 then
				-- BOOT bit7 自清：重装修调值（模型只计数），其余位照落
				boot_count = boot_count + 1
				v = v % 128
			end
			regs[a] = v
			return
		end
		-- CTRL1/2/6/CTRL8/CTRL9 全 8 位忠实落盘（fs/odr 是位域，高邻位
		-- 为滤波/dualC/op_mode，驱动 RMW 保留；保留码也照存，观察口解码）
		regs[a] = v
	end

	-- ---------------- 总线回调（点定义，无 self） ----------------

	function d.write(bytes)
		if type(bytes) ~= "table" or #bytes == 0 then return false end
		local nb = #bytes
		for i = 1, nb do
			if not is_byte(bytes[i]) then return false end
		end
		if d.nack or d.nack_write then return false end
		if write_pred and write_pred(bytes) then return false end

		if nb == 1 then
			-- 纯指针写（读族事务的指针相位）：0x00..0xFF ACK 设 ptr
			read_ptr = bytes[1]
			return true
		end

		local base = bytes[1]
		for i = 2, nb do
			write_byte((base + i - 2) % 256, bytes[i])
		end

		write_count = write_count + 1
		local cp = {}
		for i = 1, nb do cp[i] = bytes[i] end
		frames[#frames + 1] = cp
		return true
	end

	function d.read(n)
		if d.nack or d.nack_read then return nil end
		if read_ptr == nil then return nil end			-- 无指针纯读：NACK
		n = math.floor(tonumber(n) or 1)
		if n < 1 then n = 1 elseif n > 16 then n = 16 end
		if read_pred and read_pred(read_ptr, n) then return nil end
		local if_inc = math.floor((regs[R_CTRL3] or 0) / 4) % 2 == 1
		local t = {}
		for _ = 1, n do
			t[#t + 1] = byte_at(read_ptr)
			-- IF_INC=1 指针才自增；=0 重复读同址（真机 ST 语义）
			if if_inc then read_ptr = (read_ptr + 1) % 256 end
		end
		read_count = read_count + 1
		return t
	end

	-- ---------------- 测试 / 守护注入 API（冒号定义） ----------------

	-- 六轴原始码直接装（s16，-32768..32767；小端打包在读取时合成）
	function d:set_accel_raw(x, y, z)
		ax = clampi(x, -32768, 32767)
		ay = clampi(y, -32768, 32767)
		az = clampi(z, -32768, 32767)
		return self
	end

	function d:set_gyro_raw(x, y, z)
		gx = clampi(x, -32768, 32767)
		gy = clampi(y, -32768, 32767)
		gz = clampi(z, -32768, 32767)
		return self
	end

	-- 物理量逆注入：按当前 CTRL8 fs_xl 档 LSB/g 反算原始码
	function d:set_accel_g(g_x, g_y, g_z)
		local code = (regs[R_CTRL8] or 0) % 4
		local lsb = ACC_LSB_PER_G[code + 1]
		ax = clampi(round_away((tonumber(g_x) or 0) * lsb), -32768, 32767)
		ay = clampi(round_away((tonumber(g_y) or 0) * lsb), -32768, 32767)
		az = clampi(round_away((tonumber(g_z) or 0) * lsb), -32768, 32767)
		return self
	end

	-- 物理量逆注入：按当前 CTRL6 fs_g 档 LSB/dps 反算（保留码按 ±500dps）
	function d:set_gyro_dps(w_x, w_y, w_z)
		local code = (regs[R_CTRL6] or 0) % 16
		local lsb = GYR_LSB_PER_DPS[code] or GYR_LSB_PER_DPS[GYR_CODE_DEFAULT]
		gx = clampi(round_away((tonumber(w_x) or 0) * lsb), -32768, 32767)
		gy = clampi(round_away((tonumber(w_y) or 0) * lsb), -32768, 32767)
		gz = clampi(round_away((tonumber(w_z) or 0) * lsb), -32768, 32767)
		return self
	end

	-- 覆写 ROM 身份喂 build 甄别负路径（0x6C=LSM6DSO 系）；reset 回出厂
	function d:who(id)
		if id ~= nil then chip_id = clampi(id, 0, 255) end
		return self
	end

	-- CTRL3 sw_reset 位读恒 1：喂驱动 10 拍 ×10ms 轮询超时负路径
	function d:set_reset_stuck(on)
		reset_stuck = on ~= false
		return self
	end

	-- 守护逐拍造数：32 拍确定性正弦小码（默认 ±8g 档 Z≈1g≈4098 码），
	-- 连续测量直接装码、无门无睡眠。
	-- acc X 300±200 / Y -200∓150 / Z 4098±100；gyr X ±500 / Y ∓400 / Z ±300
	function d:virt_tick(n)
		n = math.floor(tonumber(n) or 0) % 32
		local s = math.sin(2 * math.pi * n / 32)
		ax = round_away(300 + 200 * s)
		ay = round_away(-200 - 150 * s)
		az = round_away(4098 + 100 * s)
		gx = round_away(500 * s)
		gy = round_away(-400 * s)
		gz = round_away(300 * s)
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred) read_pred = pred; return self end

	function d:clear_faults()
		d.nack, d.nack_read, d.nack_write = false, false, false
		write_pred, read_pred = nil, nil
		reset_stuck = false
		return self
	end

	function d:reset_logs()
		frames = {}
		write_count, read_count = 0, 0
		return self
	end

	-- 上电复位：全状态回 POR（身份/注入码/卡死态/计数器归位）；钩子保留
	function d:reset()
		chip_id = CHIP_ID
		ax, ay, az, gx, gy, gz = 0, 0, 0, 0, 0, 0
		reset_stuck = false
		read_ptr = nil
		frames = {}
		write_count, read_count = 0, 0
		reset_count, boot_count = 0, 0
		seed_body()
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:reg(a) return regs[math.floor(tonumber(a) or 0) % 256] end
	function d:regs() return regs end
	function d:pointer() return read_ptr end
	function d:write_count() return write_count end
	function d:read_count() return read_count end
	function d:reset_count() return reset_count end
	function d:boot_count() return boot_count end
	function d:frames() return frames end
	function d:chip_id() return chip_id end
	function d:accel_raw() return ax, ay, az end
	function d:gyro_raw() return gx, gy, gz end

	-- 当前配置解码（量程码/物理档/ODR 码 + 原始配置寄存器）
	function d:config()
		local c1, c2 = regs[R_CTRL1] or 0, regs[R_CTRL2] or 0
		local fs_xl = (regs[R_CTRL8] or 0) % 4
		local fs_g = (regs[R_CTRL6] or 0) % 16
		return {
			ctrl1 = c1,
			ctrl2 = c2,
			ctrl3 = regs[R_CTRL3] or 0,
			ctrl6 = regs[R_CTRL6] or 0,
			ctrl8 = regs[R_CTRL8] or 0,
			ctrl9 = regs[R_CTRL9] or 0,
			odr_xl = c1 % 16,
			odr_g = c2 % 16,
			fs_xl = fs_xl,
			acc_g = ACC_G[fs_xl + 1],
			fs_g = fs_g,
			gyr_dps = GYR_DPS[fs_g],		-- 保留码为 nil
			status = status_byte(),
		}
	end

	seed_body()
	return d
end

return M

-- ==================== 迁移订正（旧手写桩 + 真实驱动 bug） ====================
-- 1) **抓出真实驱动 REG_CTRL8 地址 bug（迁移须修驱动，不改模型）**：
--    modules/LSM6DSV.lua 原写 REG_CTRL8 = 0x18——官方 lsm6dsv16x_reg.h
--    实证 fs_xl 在 CTRL8(0x17)[1:0]，0x18 是 CTRL9（usr_off/lpf2 等
--    使能位）。后果：驱动把加计 fs 码写进 CTRL9，真机加计永远 POR
--    ±2g（横幅却印配置值）。模型坚持 0x17/0x18 两址隔离：build 后
--    0x17 低 2 位为 fs 码、0x18 保持 0，迁移测试据此钉死修复。
-- 2) 旧桩 CTRL3 POR 播 0x04（IF_INC=1）且软复位后「首拍读 1、次拍
--    自清」——官方 CTRL3 POR=0x00，sw_reset 写即自清，真实驱动首拍
--    轮询（睡 10ms 一次）即见 0 跳出；旧桩把轮询节奏放慢了一拍，且
--    完全没建 IF_INC=0 多字节读重复同址的门。
-- 3) 旧桩是 dofile 后覆盖全局的假桩：无 0x17/0x18 地址隔离、无 fs_g
--    0xC 不连续码观察、无 STATUS 合成、无 NACK/故障钩子/卡死注入/
--    virt_tick，且小端双窗只测了手写 regs[]，未走真实驱动拼字。
