-- modules/lib/virtbus/emc2101.lua —— Microchip EMC2101 智能风扇控制器虚拟器件模型
-- bot4cj 虚拟总线器件库（无 BUS 字段 = I2C，attach 动态委托 dev.write/dev.read）
-- 对应真实驱动：modules/EMC2101.lua（同协议独立实现，防互证假绿；改协议两处同步）
--
-- 定位：【8 位字节指针 + 测量反馈（内外双温度/TACH 转速）与输出控制
-- （手动占空比/片上 LUT 温控）混合 + 双路数据读互锁】样板（Microchip
-- 原 SMSC EMC2101，固定 I2C 0x4C，纯闭包、纯算术无 bitcompat）。
-- 寄存器全按字节寻址（i2c_read_reg = 写 1B 指针 + 读 1B，
-- i2c_write_reg = 写 2B {ptr,val}），读指针随字节自增、0xFF 后算术
-- 回绕 0x00，与 amg8833/bmm150/as3935 同族，个性六处（协议事实逐条
-- 照 SMSC EMC2101 Datasheet Rev.2.54 第 6 章钉死，迁移时戳破旧桩
-- 三处互证假绿，见文件末「迁移订正」）：
--
--   1) **双路 Data Read Interlock（§6.1，本模型最核心的新范式）**：
--      外部温度必须先读高字节 0x01——该读把当前低字节 0x10 拍进影子，
--      再读 0x10 取到的就是与高字节同源的锁存值；顺序反了可能撕裂。
--      TACH 恰好相反：必须先读低字节 0x46——该读把当前高字节 0x47
--      拍进影子，再读 0x47 才与低字节同源（旧桩与旧驱动误按「都先读
--      MSB」互证，Arduino/CircuitPython 两库均明确 LSB first）。
--      模型照实：读 0x01 刷新 ext 影子、读 0x10 出影子；读 0x46 刷新
--      TACH 影子、读 0x47 出影子。先注入 A 值读首字节、再注入 B 值
--      读次字节，拿到的仍是 A 对（互锁可观测），直接跳读次字节拿到
--      的是上次锁存（POR 影子 0/0xFF）。
--   2) **外温 11 位二进制补码**：0x01 = 符号+7 整数位，0x10 高 3 位
--      为 0.5/0.25/0.125°C 分数（低 5 位无定义读 0），raw11 =
--      ((msb<<8|lsb)>>5)&0x7FF，s11×0.125 得 °C（码域 -1024~1023
--      = -128.0~127.875°C）。raw11=0x3F8 二极管开路、0x3FF 短路
--      是特征码（驱动返 TEMP_EXT=nil，STATUS.0x04 另由主机查），
--      模型只装字节，换算/判定全在驱动侧。
--   3) **PROG 位（FAN_CONFIG 0x4A bit5）是 LUT/手动模式闸门，且
--      POR=0x20（PROG=1）**：PROG=1 时 FAN_SETTING(0x4C) 与 LUT
--      16 字节可写、写 0x4C 立即加在风扇上、LUT 不参与控速（手动）；
--      PROG=0 时 LUT 激活片上自闭环，0x4C 与 LUT 寄存器变只读——
--      主机写仍 ACK 但不落盘（datasheet Table 6.1 Note 6.1 文案把
--      极性写反，§6.16 正文与 Adafruit _fan_lut_prog 语义为准）。
--      驱动 build/call 都是先读-改-写置 PROG=1 再写占空比/LUT，
--      LUT 写完才清 PROG=0，模型闸门逐笔验证此顺序。
--   4) **占空比满量程不是固定 63**：PWM 模式 100% 对应 FAN_SETTING
--      = 2×max(1, PWM_F)，PWM_F 是 0x4D 的低 5 位，POR=0x17=23
--      （满码 lsb=46；datasheet 建议写 0x1F=31 得最大分辨率 62 档）。
--      驱动 build 现读 PWM_F 现算 full_lsb，模型只存寄存器，换算归
--      驱动。DAC 模式满量程才是 0x3F（本驱动不切 DAC）。
--   5) **LUT 8 槽 ×（7 位温度,6 位占空比）交错位于 0x50~0x5F**，
--      POR 全是兜底 [0x7F,0x3F]（127°C 前尾项一直生效，Adafruit
--      同款空槽填充）；仅 PROG=1 可写，温度槽 &0x7F、风速槽 &0x3F。
--   6) TACH 计数 16 位小端（0x46 低、0x47 高），POR=0xFFFF
--      （低速/停转特征），RPM=5,400,000/计数（§6.14），计数 0 才是
--      驱动返回 RPM=0 的防除零情形。无转换门/无 advance：温度与
--      TACH 是连续测量值常驻寄存器，virt_tick 直接装码预武装。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数，
-- attach 动态委托 dev.write(bytes) 契约）。

local M = {}
M._VERSION = "0.1.0"

-- ==================== 寄存器地址 / 常量 ====================
local REG_INT_TEMP  = 0x00		-- 内部温度 s8（只读测量）
local REG_EXT_MSB   = 0x01		-- 外部温度高字节（先读锁存 0x10）
local REG_STATUS    = 0x02		-- 状态位（只读）
local REG_EXT_LSB   = 0x10		-- 外部温度低字节（高 3 位分数）
local REG_TACH_LSB  = 0x46		-- TACH 低字节（先读锁存 0x47）
local REG_TACH_MSB  = 0x47		-- TACH 高字节
local REG_FAN_CFG   = 0x4A		-- bit5 PROG：1=手动/可编程，0=LUT 激活
local REG_FAN_SPIN  = 0x4B		-- 自旋启动配置（驱动不碰，POR 0x3F）
local REG_FAN_SET   = 0x4C		-- 手动占空比（低 6 位）
local REG_PWM_F     = 0x4D		-- PWM_F 低 5 位
local REG_PWM_DIV   = 0x4E		-- PWM 分频（驱动不碰，POR 0x01）
local REG_LUT_HYST  = 0x4F		-- LUT 迟滞（低 5 位，POR 0x04）
local REG_LUT_BASE  = 0x50		-- LUT 8×(temp,duty) 至 0x5F
local REG_PART_ID   = 0xFD
local REG_MFG_ID    = 0xFE
local REG_REV       = 0xFF

local FAN_CFG_PROG  = 0x20		-- PROG 位权
local MFG_ID_SMSC   = 0x5D
local PART_ID       = 0x16		-- EMC2101-R 为 0x28（who 覆写喂甄别）
local REV_ID        = 0x01

local RPM_DIVISOR   = 5400000.0	-- §6.14 式 [3]
local EXT_OPEN      = 0x3F8		-- 外温特征码：二极管开路
local EXT_SHORT     = 0x3FF		-- 外温特征码：二极管短路

-- POR 默认值（Datasheet Rev.2.54 Table 6.1/6.18/6.23/6.24/6.25）
local POR_FAN_CFG   = 0x20		-- PROG=1（上电即手动/可编程态）
local POR_FAN_SPIN  = 0x3F
local POR_FAN_SET   = 0x00
local POR_PWM_F     = 0x17		-- 23 → 100% 对应 lsb 46
local POR_PWM_DIV   = 0x01
local POR_LUT_HYST  = 0x04
local POR_TACH      = 0xFF		-- TACH 两字节均 FF
local POR_LUT_TEMP  = 0x7F		-- LUT 空槽兜底
local POR_LUT_DUTY  = 0x3F

local function clampi(v, lo, hi)
	v = math.floor(tonumber(v) or 0)
	if v < lo then return lo elseif v > hi then return hi else return v end
end

-- 11 位二进制补码还原（外部温度）
local function s11(v)
	if v >= 0x400 then v = v - 0x800 end
	return v
end

function M.new()
	local d = {}

	-- 芯片侧状态
	local regs = {}				-- 字节寄存器影子（稀疏，缺省读 0）
	local read_ptr = nil		-- 最近一笔指针写（read 回调据此起 burst）
	local ext_shadow = 0		-- 读 0x01 锁存的外温低字节
	local tach_shadow = 0xFF	-- 读 0x46 锁存的 TACH 高字节（POR 影子 FF）
	local part_id = PART_ID
	local mfg_id = MFG_ID_SMSC
	local write_count = 0		-- ≥2B 写事务笔数
	local write_log = {}		-- ≥2B 写事务 {reg=首地址, bytes=...}

	-- 故障钩子（reset 保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	-- ---------------- POR / 重新上电 ----------------

	local function seed_body()
		regs = {}
		-- 测量区 POR 全 00h（TACH 为 FFFFh）
		regs[REG_INT_TEMP], regs[REG_EXT_MSB], regs[REG_STATUS] = 0, 0, 0
		regs[REG_EXT_LSB] = 0
		regs[REG_TACH_LSB], regs[REG_TACH_MSB] = POR_TACH, POR_TACH
		-- 风扇控制块
		regs[REG_FAN_CFG] = POR_FAN_CFG
		regs[REG_FAN_SPIN] = POR_FAN_SPIN
		regs[REG_FAN_SET] = POR_FAN_SET
		regs[REG_PWM_F] = POR_PWM_F
		regs[REG_PWM_DIV] = POR_PWM_DIV
		regs[REG_LUT_HYST] = POR_LUT_HYST
		-- LUT 8 槽兜底 [0x7F,0x3F]
		for i = 0, 7 do
			regs[REG_LUT_BASE + 2 * i] = POR_LUT_TEMP
			regs[REG_LUT_BASE + 2 * i + 1] = POR_LUT_DUTY
		end
		-- ROM 身份
		regs[REG_PART_ID] = part_id
		regs[REG_MFG_ID] = mfg_id
		regs[REG_REV] = REV_ID
		-- 锁存影子回 POR
		ext_shadow, tach_shadow = 0, 0xFF
	end

	seed_body()

	-- ---------------- 协议语义 ----------------

	local function prog1()
		return math.floor((regs[REG_FAN_CFG] or 0) / FAN_CFG_PROG) % 2 == 1
	end

	-- 单字节落盘（地址已按自增折入 0x00~0xFF）
	local function write_byte(a, v)
		v = v % 256
		if a == REG_INT_TEMP or a == REG_EXT_MSB or a == REG_STATUS
			or a == REG_EXT_LSB or a == REG_TACH_LSB or a == REG_TACH_MSB
			or a == REG_PART_ID or a == REG_MFG_ID or a == REG_REV then
			return						-- 测量/ROM 只读：写 ACK 丢弃
		elseif a == REG_FAN_CFG then
			regs[a] = v % 0x80			-- bit7 无定义读 0
		elseif a == REG_FAN_SPIN then
			regs[a] = v % 0x40			-- bit[7:6] 无定义
		elseif a == REG_FAN_SET then
			if prog1() then regs[a] = v % 0x40 end	-- PROG=0 只读，写丢弃
		elseif a == REG_PWM_F then
			regs[a] = v % 0x20			-- 仅低 5 位
		elseif a == REG_PWM_DIV then
			regs[a] = v
		elseif a == REG_LUT_HYST then
			regs[a] = v % 0x20			-- 仅低 5 位
		elseif a >= REG_LUT_BASE and a <= 0x5F then
			if prog1() then
				-- 仅 PROG=1 可编程；偶址温度槽 7 位、奇址风速槽 6 位
				if a % 2 == 0 then regs[a] = v % 0x80
				else regs[a] = v % 0x40 end
			end
		end
		-- 其余未定义地址：ACK 丢弃（datasheet：未定义寄存器读 0）
	end

	function d.write(bytes)
		if type(bytes) ~= "table" then return false end
		if d.nack or d.nack_write then return false end
		local nb = #bytes
		if nb == 0 then return false end
		if write_pred and write_pred(bytes) then return false end
		if nb == 1 then
			-- 纯指针写（读族事务的指针相位）：0x00..0xFF ACK 设 ptr
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
			if a == REG_EXT_MSB then
				-- 读外温高字节：把当前低字节拍进影子
				ext_shadow = regs[REG_EXT_LSB] or 0
				t[i] = regs[REG_EXT_MSB] or 0
			elseif a == REG_EXT_LSB then
				t[i] = ext_shadow		-- 出锁存值，不直接出活值
			elseif a == REG_TACH_LSB then
				-- 读 TACH 低字节：把当前高字节拍进影子
				tach_shadow = regs[REG_TACH_MSB] or 0
				t[i] = regs[REG_TACH_LSB] or 0
			elseif a == REG_TACH_MSB then
				t[i] = tach_shadow		-- 出锁存值，不直接出活值
			else
				t[i] = regs[a] or 0		-- 稀疏影子，未定义读 0
			end
			read_ptr = (read_ptr + 1) % 256	-- burst 自增、0xFF 算术回绕
		end
		return t
	end

	-- ---------------- 测试 / 守护注入 API（冒号定义） ----------------

	-- 内部温度（整数 °C，s8 码域 -128~127）
	function d:set_internal_temp_c(t)
		t = clampi(t, -128, 127)
		regs[REG_INT_TEMP] = t % 256
		return self
	end

	-- 外部温度直接装 11 位有符号码（-1024~1023）
	function d:set_external_raw(code)
		code = clampi(code, -1024, 1023) % 0x800
		regs[REG_EXT_MSB] = math.floor(code / 8) % 256		-- code>>3
		regs[REG_EXT_LSB] = (code % 8) * 0x20				-- 低 3 位置 bit7:5
		return self
	end

	-- 外部温度物理量（°C，1/8°C 量化），返回量化后实际值
	function d:set_external_temp_c(x)
		x = tonumber(x) or 0
		local code = clampi(math.floor(x * 8 + 0.5), -1024, 1023)
		d:set_external_raw(code)
		return s11(code) * 0.125
	end

	-- 二极管故障特征码："open"/"short"（只装测量码，STATUS 由 set_status 另注）
	function d:set_diode_fault(which)
		if which == "open" then
			d:set_external_raw(EXT_OPEN)
		elseif which == "short" then
			d:set_external_raw(EXT_SHORT)
		end
		return self
	end

	-- 状态寄存器原始字节（BUSY/告警/FAULT/TACH 等位由测试侧显式注入）
	function d:set_status(s)
		regs[REG_STATUS] = clampi(s, 0, 255)
		return self
	end

	-- TACH 16 位计数直接装（0=停转；POR FFFF=低速特征）
	function d:set_tach_count(c)
		c = clampi(c, 0, 65535)
		regs[REG_TACH_LSB] = c % 256
		regs[REG_TACH_MSB] = math.floor(c / 256) % 256
		return self
	end

	-- 物理逆注入：按 RPM 反推 TACH 计数（与驱动常数同源）；rpm<=0→计数 0
	function d:set_rpm(rpm)
		rpm = tonumber(rpm) or 0
		if rpm <= 0 then
			d:set_tach_count(0)
		else
			d:set_tach_count(clampi(math.floor(RPM_DIVISOR / rpm + 0.5), 1, 65535))
		end
		return self
	end

	-- 覆写 ROM 身份喂 build 甄别负路径（nil=不改；完整 reset 恢复出厂）
	function d:who(part, mfg)
		if part ~= nil then
			part_id = clampi(part, 0, 255)
			regs[REG_PART_ID] = part_id
		end
		if mfg ~= nil then
			mfg_id = clampi(mfg, 0, 255)
			regs[REG_MFG_ID] = mfg_id
		end
		return self
	end

	-- 守护逐拍造数：内温 35±5°C 整数、外温 42±6°C（1/8 量化）、
	-- 转速 3000±1200 RPM（恒四位数 1800~4200，count 反推装码）；
	-- STATUS 恒清 0。连续测量器件：tick 直接装码，守护 call 零睡眠。
	function d:virt_tick(n)
		n = math.floor(tonumber(n) or 0)
		local s = math.sin(2 * math.pi * n / 32)
		d:set_status(0)
		d:set_internal_temp_c(math.floor(35 + 5 * s + 0.5))
		d:set_external_temp_c(42 + 6 * s)
		d:set_rpm(3000 + 1200 * s)
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
		write_count = 0
		write_log = {}
		return self
	end

	-- 重新上电：全状态回 POR（身份覆写/日志清零）；故障钩子保留
	function d:reset()
		part_id = PART_ID
		mfg_id = MFG_ID_SMSC
		write_count = 0
		write_log = {}
		read_ptr = nil
		seed_body()
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:reg(a) return regs[math.floor(tonumber(a) or 0) % 256] end
	function d:ptr() return read_ptr end
	function d:prog() return prog1() end

	function d:internal_c()
		local v = regs[REG_INT_TEMP] or 0
		if v >= 0x80 then v = v - 0x100 end
		return v
	end

	-- 外温 11 位原始码（从寄存器字节重组，与驱动同布局）
	function d:ext_code()
		local msb, lsb = regs[REG_EXT_MSB] or 0, regs[REG_EXT_LSB] or 0
		return math.floor((msb * 256 + lsb) / 32) % 0x800
	end

	-- 外温 °C；开路/短路特征码返 nil
	function d:ext_temp_c()
		local code = d:ext_code()
		if code == EXT_OPEN or code == EXT_SHORT then return nil end
		return s11(code) * 0.125
	end

	function d:status() return regs[REG_STATUS] or 0 end

	function d:tach_count()
		return (regs[REG_TACH_MSB] or 0) * 256 + (regs[REG_TACH_LSB] or 0)
	end

	function d:rpm()
		local c = d:tach_count()
		if c >= 1 then return RPM_DIVISOR / c end
		return 0
	end

	function d:pwm_f() return (regs[REG_PWM_F] or 0) % 0x20 end
	function d:full_lsb() return 2 * math.max(1, d:pwm_f()) end
	function d:fan_setting() return regs[REG_FAN_SET] or 0 end

	-- LUT 8 槽 {{temp=,duty=}...}（按槽位顺序，未排序）
	function d:lut()
		local t = {}
		for i = 0, 7 do
			t[i + 1] = {
				temp = regs[REG_LUT_BASE + 2 * i] or 0,
				duty = regs[REG_LUT_BASE + 2 * i + 1] or 0,
			}
		end
		return t
	end

	function d:part_id() return regs[REG_PART_ID] or 0 end
	function d:mfg_id() return regs[REG_MFG_ID] or 0 end
	function d:rev() return regs[REG_REV] or 0 end
	function d:write_count() return write_count end
	function d:writes() return write_log end

	return d
end

return M

-- ==================== 迁移订正（旧手写桩三处互证假绿） ====================
-- 1) FAN_CONFIG(0x4A) POR 旧桩给 0x00（LUT 激活态）；datasheet Table 6.18
--    DEFAULT=20h——上电即 PROG=1 手动/可编程态。
-- 2) PWM_F(0x4D) POR 旧桩给 0x0F=15（full_lsb=30）；datasheet Table 6.24
--    DEFAULT=17h=23——真实 full_lsb=46（50%→23、75%→35、100%→46）。
-- 3) TACH 读序旧桩与旧驱动都按「先 MSB(0x47) 后 LSB(0x46)」断言；
--    datasheet §6.1 明确 TACH 是读低字节(0x46)锁存高字节(0x47)，
--    外温才是读高字节(0x01)锁存低字节(0x10)。已修驱动 call 读序。
