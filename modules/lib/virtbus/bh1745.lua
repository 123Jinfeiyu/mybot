-- bh1745.lua —— ROHM BH1745NUC 四通道（R/G/B/Clear）数字颜色传感器虚拟器件模型
-- bot4cj 虚拟硬件库（modules/lib/virtbus/，配合 modules/lib/virtbus.lua 使用）
--
-- 【8 位字节指针稀疏寄存器图 + 四通道 16 位【小端】数据块 + VALID 握手 +
-- 积分周期门】样板（BH1745NUC，I2C 地址 0x38 缺省/0x39 由 ADDR 引脚选；
-- 纯闭包、纯算术无 bitcompat，约 330 行）。寄存器事实照 datasheet
-- BH1745NUC Rev.002（2016-05-23，TSZ02201-0M3M0F616080-1-2）逐位钉死：
--
--   0x40 SYSTEM_CONTROL POR 0x0B：bit7 SW reset（写 1 全片回 POR 进 power
--               down、位自清，读回恒 Part ID 0x0B，reset_count+1）、bit6
--               INT reset（写 1 INT 脚高阻、位自清，int_reset_count+1，
--               不动其他寄存器）、[5:0] Part ID 只读 ROM = 0x0B
--   0x41 MODE_CONTROL1 POR 0x00：[7:3] 保留写 0（模型抹保留）、[2:0]
--               RGBC 积分时间码 0..5 = 160/320/640/1280/2560/5120ms，
--               6/7 = Forbidden（写之无转换，remaining=nil）
--   0x42 MODE_CONTROL2 POR 0x00：bit7 VALID 只读动态合成——「上次写
--               MODE_CONTROL1/2/3 或上次【读】MODE_CONTROL2 之后数据已更新」
--               则 1，写 MODE 寄存器重启测量即清 0，**读 0x42 也清 0**；
--               bit4 RGBC_EN（0=power down 停转、1=连续测量）、[1:0]
--               ADC 增益 0/1/2 = x1/x2/x16、3 保留；[6:5]/[3:2] 保留抹
--   0x44 MODE_CONTROL3 POR 0x00：datasheet 要求写 0x02 开通道有效判别，
--               全 8 位照存，写之同样重启测量
--   0x50~0x57 RGBC 数据块 **8 字节小端**：0x50 RED_LSB/0x51 RED_MSB、
--               0x52/53 GREEN、0x54/55 BLUE、0x56/57 CLEAR（datasheet
--               Command Set 逐行明确 LSB 在偶地址）；只读，POR 全 0
--   0x58/0x59 DINT 内部数据（IC 测试用，POR 0，只读）
--   0x60 INTERRUPT POR 0x00：bit7 INT STATUS 只读（本模型不做阈值评估
--               恒 0）、bit4 INT LATCH/[3:2] SOURCE/bit0 ENABLE 照存，
--               [6:5]/bit1 保留抹
--   0x61 PERSISTENCE POR **0x01**：[1:0] 照存（0 每拍翻转/1 每拍更新/
--               2 连 4 次/3 连 8 次），高 6 位保留抹
--   0x62/0x63 TH 高阈值 POR **0xFFFF**（LSB 在 0x62）
--   0x64/0x65 TL 低阈值 POR **0x0000**（LSB 在 0x64）
--   0x92 MANUFACTURER ID 只读 ROM = **0xE0**，who() 可覆写
--
-- 【迁移抓出真实驱动字节序 bug】旧驱动把 0x50 当 MSB（大端）拼字
-- （R=b1*256+b2），与 datasheet「0x50=RED_DATA_LSBs」相反，已修
-- modules/BH1745.lua 为小端（R=b2*256+b1）；旧桩用大端字节序喂数是错的。
--
-- 【真实驱动 build/call 事务】build：set + {40,80} 软复位字写 +5ms +
-- 指针读 0x40/0x92 双 ID 甄别（两个读无条件都发）+ {41} {42} {44} 三笔
-- 字写 + milli_sleep(mt+2) = **9 txlog，睡眠 5+(160+2)=167ms（缺省档）**；
-- call：set + {50} 指针写 + 8B burst 纯读 = **3 txlog 零睡眠**。芯片连续
-- 测量、数据寄存器恒可读（POR/未就绪读 0，不 NACK）——模型用 advance(ms)
-- 推积分门仅供协议测试（VALID 翻转/到点装载注入码），守护不推虚拟时钟，
-- 靠 virt_tick 直接装数据 + VALID=1 预武装，build 后 call 零等待。
--
-- 用法：
--   local mmod = dofile("modules/lib/virtbus/bh1745.lua")
--   local dev = mmod.new()
--   dev:set_rgbc_raw(22000, 26000, 15000, 42000)
--   vbus.attach("BH1745@0x38", 0x38, dev)
--
--   dev:who(0x0C)       -- 覆写 Part ID 喂 build 错挂甄别
--   dev:advance(160)    -- 推过一个积分周期（VALID 置 1、装载注入码）
--
-- 【边界约定】模型只固化寄存器图/POR/小端数据块/VALID 动态语义/积分门；
-- 颜色/照度换算（本驱动也不做，直接交原始计数）在驱动侧，测量值只由
-- 注入 API 或 virt_tick 改变，模型绝不自造「场景」数据（virt_tick 的
-- 确定性正弦是守护仿真样本，非协议转换产物）。

local bh1745 = {}
bh1745._VERSION = "0.1.0"

bh1745.REG = {
	SYS  = 0x40,		-- SYSTEM_CONTROL
	M1   = 0x41,		-- MODE_CONTROL1（积分时间）
	M2   = 0x42,		-- MODE_CONTROL2（VALID/RGBC_EN/增益）
	M3   = 0x44,		-- MODE_CONTROL3
	DATA = 0x50,		-- 8B R/G/B/C 小端
	DINT = 0x58,		-- 2B 内部数据（只读）
	INT  = 0x60,		-- INTERRUPT
	PERS = 0x61,		-- PERSISTENCE
	TH_L = 0x62, TH_M = 0x63,
	TL_L = 0x64, TL_M = 0x65,
	MFR  = 0x92,		-- MANUFACTURER ID
}

bh1745.PART_ID    = 0x0B
bh1745.MFR_ID     = 0xE0
bh1745.SW_RESET   = 0x80
bh1745.INT_RESET  = 0x40
bh1745.RGBC_EN    = 0x10
bh1745.MT_TAB     = { 160, 320, 640, 1280, 2560, 5120 }	-- 码 0..5
bh1745.I2C_ADDR   = 0x38	-- ADDR=L；ADDR=H = 0x39

-- 寄存器类型表（未列地址 = 未定义：指针写/读 ACK，字写 ACK 丢弃，读 0）
-- rw 可写字节 / ro 只读字节 / sys=SYSTEM_CONTROL（命令位+ROM）
local RW_REGS = {
	[0x41] = true, [0x42] = true, [0x44] = true,
	[0x60] = true, [0x61] = true,
	[0x62] = true, [0x63] = true, [0x64] = true, [0x65] = true,
}
local RO_REGS = {}
for a = 0x50, 0x59 do RO_REGS[a] = true end
RO_REGS[0x92] = true

local function clampi(v, lo, hi)
	v = math.floor(tonumber(v) or 0)
	if v < lo then return lo end
	if v > hi then return hi end
	return v
end

function bh1745.new(opts)
	opts = opts or {}

	local d = {}
	local regs = {}				-- 稀疏字节影子
	local ptr = nil			-- 最近一笔 1B 指针写
	local part_id = opts.part_id or bh1745.PART_ID
	local mfr_id  = opts.mfr_id  or bh1745.MFR_ID
	local valid = false			-- MODE2 bit7 动态值
	local remaining = nil		-- 距本积分周期结束 ms（nil=停转/POR）
	local measure_count = 0		-- 积分完成次数
	local reset_count = 0		-- SW reset（写 0x40 bit7）
	local int_reset_count = 0	-- INT reset（写 0x40 bit6）
	-- 最近一次注入样本（周期到点时装入数据区；u16）
	local sample = { 0, 0, 0, 0 }
	local write_pred, read_pred
	d.write_log = {}

	d.nack = false
	d.nack_read = false
	d.nack_write = false

	-- ---------------- 派生 ----------------

	local function mt_code() return regs[bh1745.REG.M1] % 8 end
	local function period_ms()
		local c = mt_code()
		if c >= #bh1745.MT_TAB then return nil end	-- 码 6/7 forbidden
		return bh1745.MT_TAB[c + 1]
	end
	local function enabled()
		return math.floor(regs[bh1745.REG.M2] / 16) % 2 == 1
	end

	-- ---------------- 数据块装载（小端：LSB 偶地址） ----------------

	local function load_data()
		for i = 0, 3 do
			local u = clampi(sample[i + 1], 0, 65535) % 65536
			regs[0x50 + i * 2]     = u % 256		-- LSB
			regs[0x50 + i * 2 + 1] = math.floor(u / 256)	-- MSB
		end
	end

	-- 写 MODE_CONTROL1/2/3：进行中的测量立即中止、从头重启、VALID 清 0
	local function restart_meas()
		valid = false
		if enabled() and period_ms() then
			remaining = period_ms()
		else
			remaining = nil
		end
	end

	-- ---------------- POR / 软复位 ----------------

	local function seed_regs()
		regs = {}
		regs[bh1745.REG.SYS]  = part_id
		regs[bh1745.REG.M1]   = 0x00
		regs[bh1745.REG.M2]   = 0x00
		regs[bh1745.REG.M3]   = 0x00
		for a = 0x50, 0x59 do regs[a] = 0 end
		regs[bh1745.REG.INT]  = 0x00
		regs[bh1745.REG.PERS] = 0x01
		regs[bh1745.REG.TH_L] = 0xFF
		regs[bh1745.REG.TH_M] = 0xFF
		regs[bh1745.REG.TL_L] = 0x00
		regs[bh1745.REG.TL_M] = 0x00
		regs[bh1745.REG.MFR]  = mfr_id
	end

	-- SW reset：全部寄存器回 POR、power down（VALID 清、门停），
	-- 注入样本清零、计数归零；故障钩子/who 身份保留由调用方约束（reseed
	-- 用当前 part/mfr 播种）
	local function reseed()
		seed_regs()
		ptr = nil
		valid = false
		remaining = nil
		measure_count = 0
		sample = { 0, 0, 0, 0 }
	end

	seed_regs()

	-- ---------------- 单字节寄存器写（含命令/只读/保留语义） ----------------

	local function write_one(reg, val)
		val = val % 256
		if reg == bh1745.REG.SYS then
			-- bit7 SW reset 优先：全片回 POR（datasheet：所有寄存器复位、
			-- power down）；bit6 INT reset 只抬 INT 脚；两位都自清，
			-- [5:0] Part ID 只读——读回恒 part_id
			if val >= 0x80 then
				reseed()
				reset_count = reset_count + 1
			elseif val >= 0x40 then
				int_reset_count = int_reset_count + 1
			end
			regs[bh1745.REG.SYS] = part_id
			return
		end
		if reg == bh1745.REG.M1 then
			regs[reg] = val % 8			-- [7:3] 保留抹
			restart_meas()
			return
		end
		if reg == bh1745.REG.M2 then
			-- bit7 VALID 只读不写；[6:5]/[3:2] 保留抹；bit4+[1:0] 照存
			regs[reg] = (math.floor(val / 16) % 2) * 16 + val % 4
			restart_meas()
			return
		end
		if reg == bh1745.REG.M3 then
			regs[reg] = val			-- datasheet 仅要求写 0x02
			restart_meas()
			return
		end
		if reg == bh1745.REG.INT then
			-- bit7/[6:5] 只读、bit1 保留抹；bit4/[3:2]/bit0 照存
			regs[reg] = (math.floor(val / 16) % 2) * 16
			          + (math.floor(val / 4) % 4) * 4
			          + val % 2
			return
		end
		if reg == bh1745.REG.PERS then
			regs[reg] = val % 4		-- [7:2] 保留抹
			return
		end
		if RW_REGS[reg] then
			regs[reg] = val			-- 阈值字节照存（LSB 先）
			return
		end
		-- RO_REGS 与未定义地址：写 ACK 丢弃（测量值只能由注入 API 改）
	end

	-- ---------------- virtbus 点定义回调（无 self） ----------------

	local function log_copy(bytes)
		local cp = {}
		for i = 1, #bytes do cp[i] = bytes[i] % 256 end
		d.write_log[#d.write_log + 1] = cp
	end

	d.write = function(bytes)
		if type(bytes) ~= "table" or #bytes == 0 then return false end
		log_copy(bytes)
		if d.nack or d.nack_write or (write_pred and write_pred(bytes)) then
			return false
		end
		for i = 1, #bytes do
			local b = bytes[i]
			if type(b) ~= "number" or b ~= b or b < 0 or b > 0xFF
				or math.floor(b) ~= b then
				return false			-- 坏帧（非字节）整笔 NACK
			end
		end
		-- 1B = 纯指针写（任何地址 ACK，读时才从该址起 burst）
		ptr = bytes[1]
		-- ≥2B：寄存器写流，地址自增 mod 256（驱动恒发恰 2B 单寄存器写）
		for i = 2, #bytes do
			write_one((bytes[1] + i - 2) % 256, bytes[i])
		end
		return true
	end

	d.read = function(n)
		if d.nack or d.nack_read then return nil end
		n = math.floor(tonumber(n) or 1)
		if n < 1 then return nil end
		if n > 16 then n = 16 end
		if ptr == nil then return nil end	-- 无指针纯读 NACK
		if read_pred and read_pred(ptr, n) then return nil end
		local out = {}
		for _ = 1, n do
			local a = ptr
			if a == bh1745.REG.M2 then
				-- VALID 动态合成；且 datasheet：读 MODE_CONTROL2 后 VALID 清 0
				out[#out + 1] = (regs[a] or 0) % 128 + (valid and 0x80 or 0)
				valid = false
			elseif a == bh1745.REG.SYS then
				out[#out + 1] = regs[a] or part_id	-- 命令位自清，恒 Part ID
			else
				out[#out + 1] = regs[a] or 0
			end
			ptr = (ptr + 1) % 256			-- burst 自增、0xFF 回绕
		end
		return out
	end

	-- ---------------- 原始码注入（协议层主输入，冒号 API） ----------------

	-- 四通道原始计数（u16，立即装数据区并记为后续周期样本）
	function d:set_rgbc_raw(r, g, b, c)
		sample = {
			clampi(r, 0, 65535), clampi(g, 0, 65535),
			clampi(b, 0, 65535), clampi(c, 0, 65535),
		}
		load_data()
		return self
	end
	function d:set_red_raw(v)   sample[1] = clampi(v, 0, 65535); load_data(); return self end
	function d:set_green_raw(v) sample[2] = clampi(v, 0, 65535); load_data(); return self end
	function d:set_blue_raw(v)  sample[3] = clampi(v, 0, 65535); load_data(); return self end
	function d:set_clear_raw(v) sample[4] = clampi(v, 0, 65535); load_data(); return self end

	-- 配置/阈值等协议字节留测试直注入口
	function d:set_reg(a, v)
		a = clampi(a, 0, 255)
		regs[a] = clampi(v, 0, 255)
		return self
	end

	-- ---------------- 积分周期门（协议测试用；守护不驱动） ----------------

	-- 推虚拟时钟：连续测量下周期到点 → 装载注入样本、VALID 置 1、排下周期
	function d:advance(ms)
		ms = tonumber(ms) or 0
		while enabled() and remaining ~= nil do
			remaining = remaining - ms
			if remaining > 0 then break end
			load_data()
			valid = true
			measure_count = measure_count + 1
			if period_ms() then
				remaining = remaining + period_ms()
			else
				remaining = nil
			end
			ms = 0					-- 后续周期只走补偿后的 remaining
		end
		return self
	end

	-- ---------------- 守护仿真节拍（sensor_daemon 每拍调用，确定性） ----------------

	-- 32 拍周期一组典型室内反射色计数（x1 量程内远不削顶）：
	-- R 22000±3000、G 26000±3500、B 15000±2200、C 42000±4000 同相正弦，
	-- 直接装数据区并置 VALID=1（绕过积分门预武装；build 的 MODE 写会把
	-- VALID 清 0 但不清数据，驱动不读 VALID，call 恒有新鲜数据零睡眠）。
	function d:virt_tick(n)
		n = tonumber(n) or 0
		local s = math.sin(n * (2 * math.pi / 32))
		self:set_rgbc_raw(
			math.floor(22000 + 3000 * s + 0.5),
			math.floor(26000 + 3500 * s + 0.5),
			math.floor(15000 + 2200 * s + 0.5),
			math.floor(42000 + 4000 * s + 0.5)
		)
		valid = true
		return self
	end

	-- ---------------- 身份/故障钩子/复位 ----------------

	-- 覆写 ROM ID（part/mfr 任一 nil=该项不改；软复位不恢复，section 显式复原）
	function d:who(part, mfr)
		if part ~= nil then
			part_id = clampi(part, 0, 63)	-- Part ID 仅 [5:0]
			regs[bh1745.REG.SYS] = part_id
		end
		if mfr ~= nil then
			mfr_id = clampi(mfr, 0, 255)
			regs[bh1745.REG.MFR] = mfr_id
		end
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred)  read_pred = pred; return self end
	function d:clear_faults()
		write_pred, read_pred = nil, nil
		self.nack, self.nack_read, self.nack_write = false, false, false
		return self
	end

	function d:reset_logs()
		self.write_log = {}
		return self
	end

	-- 回上电态（保留 who 身份与故障钩子——复位不得解除故障注入）
	function d:reset()
		reseed()
		reset_count, int_reset_count = 0, 0
		self.write_log = {}
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:reg(a) return regs[clampi(a, 0, 255)] or 0 end
	function d:ptr() return ptr end
	function d:part_id() return part_id end
	function d:mfr_id() return mfr_id end
	function d:valid() return valid end
	function d:enabled() return enabled() end
	function d:mt_code() return mt_code() end
	function d:gain_code() return regs[bh1745.REG.M2] % 4 end
	function d:period_ms() return period_ms() end
	function d:remaining_ms() return remaining end
	function d:measure_count() return measure_count end
	function d:reset_count() return reset_count end
	function d:int_reset_count() return int_reset_count end

	-- 数据区四通道 u16（小端拼字）
	function d:red_raw()   return (regs[0x51] or 0) * 256 + (regs[0x50] or 0) end
	function d:green_raw() return (regs[0x53] or 0) * 256 + (regs[0x52] or 0) end
	function d:blue_raw()  return (regs[0x55] or 0) * 256 + (regs[0x54] or 0) end
	function d:clear_raw() return (regs[0x57] or 0) * 256 + (regs[0x56] or 0) end
	function d:rgbc_raw()
		return self:red_raw(), self:green_raw(), self:blue_raw(), self:clear_raw()
	end

	-- 阈值对（LSB 在偶地址）
	function d:th() return (regs[0x63] or 0) * 256 + (regs[0x62] or 0) end
	function d:tl() return (regs[0x65] or 0) * 256 + (regs[0x64] or 0) end

	-- 驱动 build/call 写过的寄存器写 {reg,val}（≥2B 事务的首两字节）
	function d:config_writes()
		local out = {}
		for _, e in ipairs(self.write_log) do
			if #e >= 2 then
				out[#out + 1] = { reg = e[1], val = e[2] }
			end
		end
		return out
	end

	return d
end

return bh1745
