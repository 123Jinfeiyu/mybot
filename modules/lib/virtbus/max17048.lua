-- max17048.lua —— Maxim/ADI MAX17048 单节锂电池 ModelGauge 电量计虚拟器件模型
-- bot4cj 虚拟硬件库（modules/lib/virtbus/，配合 modules/lib/virtbus.lua 使用）
--
-- 【MAX17043 的换代芯片，同地址 0x36 同偶地址稀疏字地图，但寄存器图扩展，
-- 常数逐条不同，不能与 max17043.lua 互抄】（datasheet MAX17048/17049 Rev.7
-- Figure 8/11/13 与 Table.2 核对）：
--   0x02 VCELL  电池电压：16 位直读（**无 17043 的左移 4/>>4**），
--               LSB 78.125µV（17049 双节为 156.25µV，刻度分支在驱动按
--               VERSION 选择，模型只存原始码），码域 0~65535（满码
--               5.11992V）；只读测量区，写 ACK 但丢弃
--   0x04 SOC    u16 无符号定点 1/256%（同 17043：高字节整数 %、低字节
--               1/256%，芯片可报告 >100%，满码 255.996%）；只读测量区
--   0x06 MODE   Figure 8：bit15 保留、bit14 Quick-Start（写 1 触发一次
--               快速启动、位自清不落盘，quickstart_count+1）、bit13
--               EnSleep（照存）、bit12 HibStat（只读状态位，写忽略）、
--               bit11:0 保留；POR 0x0000
--   0x08 VERSION ROM 生产版本：0x0012=MAX17048、0x0013=MAX17049（驱动
--               build 精确甄别，0x001E/0x001F=17043/44 会被拒收）；只读，
--               who() 可覆写；POR 播 0x0012
--   0x0C CONFIG POR 0x971C（同 17043：高字节 RCOMP=0x97，低字节
--               SLEEP/ALSC/ALRT + ATHD[4:0]，bit15 是 RCOMP MSB 非复位位）
--   0x14 VALRT  电压告警阈值 16 位字，POR **0x00FF**，20mV/LSB；
--               Figure 11 明确【地址 0x14 = VALRT.MIN（字的高字节）、
--               地址 0x15 = VALRT.MAX（低字节）】——MIN<<8 | MAX，
--               即 POR MIN=0（0V，欠压永不告警）、MAX=255（5.1V，过压
--               永不告警）；全字照存，字节序的正确使用归驱动
--   0x16 CRATE  充放电速率：16 位【有符号】，LSB 0.208%/hr（充电为正），
--               码域 -32768~32767；只读测量区
--   0x1A STATUS 告警状态，POR **0x0100**（RI 上电即置 1，datasheet：
--               "RI is set when the device powers up"）；高字节（0x1A）
--               bit0 RI/bit1 VH/bit2 VL/bit3 VR/bit4 HD/bit5 SC 六标志、
--               bit6 EnVr（清除告警时保留）、bit7 保留；低字节（0x1B）
--               未定义照存；全字可读可写（清标志=驱动回写清 0 位）
-- 0xFE CMD    只写命令口：**17048 的复位魔数是 0x5400（帧 {0xFE,0x54,
--               0x00}），不是 17043 的 0x0054**——恰写触发全片复位
--               （reseed、reset_count+1），其他值 ACK 忽略；不可读。
--               （datasheet 称末位后芯片不回 ACK，与 17043 模型同按
--               ACK 处理：本驱动从不发此命令，位级细节不可观测。）
-- 驱动不触碰的寄存器不建模：HIBRT 0x0A（POR 0x8030）、VRESET/ID 0x18
-- （POR 0x96__）、TABLE 0x40~0x7F 与解锁口 0x3E/0x3F——读 NACK、字写 NACK。
--
-- 【真实驱动 build/call 事务】build：set + 写 0x08 指针 + 读 VERSION
-- （3 事务）甄别通过后，可选 QuickStart 字写 +5ms 睡眠、VALRT 读改写、
-- CONFIG（ATHD）读改写，最后读 STATUS 做横幅（读失败不致命）；call：
-- set + 依次指针读 VCELL/SOC/CRATE/STATUS 四寄存器（9 事务）零睡眠，
-- clear=1 且有告警位时追加一笔 STATUS 字写（10 事务）。芯片转换自由
-- 运行、结果寄存器恒有值——无转换门/无 pending/不需要 advance，测量
-- 值只由注入 API 改变，模型绝不自造电池数据（QuickStart 只刻自清语义）。
--
-- 用法：
--   local mmod = dofile("modules/lib/virtbus/max17048.lua")
--   local dev = mmod.new()
--   dev:set_vcell_v(3.85):set_soc_pct(80):set_crate_pcthr(-2.5)
--   vbus.attach("MAX17048@0x36", 0x36, dev)
--
--   dev:who(0x0013)     -- 覆写为 MAX17049（驱动切 156.25µV 刻度）
--   dev:set_flags(63)   -- 注入六告警位
--
-- 【边界约定】模型只固化偶地址字地图/16 位直读 VCELL/1/256 定点 SOC/
-- s16 CRATE/三段阈值字节序/STATUS 标志位/CMD 魔数；物理换算在驱动，
-- 物理量注入 API 常量仅为造样本，测试另做独立换算交叉。

local max17048 = {}
max17048._VERSION = "0.1.0"

max17048.REG = {
	VCELL  = 0x02,
	SOC    = 0x04,
	MODE   = 0x06,
	VER    = 0x08,
	CONFIG = 0x0C,
	VALRT  = 0x14,
	CRATE  = 0x16,
	STATUS = 0x1A,
	CMD    = 0xFE,
}

max17048.VER_DEFAULT   = 0x0012	-- 0x0012=MAX17048（0x0013=MAX17049）
max17048.CFG_DEFAULT   = 0x971C	-- RCOMP=0x97（25°C）+ 低字节 POR
max17048.VALRT_DEFAULT = 0x00FF	-- MIN(0x14)=0x00、MAX(0x15)=0xFF
max17048.STATUS_POR    = 0x0100	-- RI 上电置位，低字节未定义取 0
max17048.CMD_RESET     = 0x5400	-- 17048 全片复位魔数（17043 是 0x0054）
max17048.VCELL_LSB_V   = 78.125e-6	-- MAX17048 单节刻度（17049=156.25µV）
max17048.VCELL_MAX     = 65535	-- 16 位无符号码域
max17048.CRATE_LSB     = 0.208	-- %/hr 每 LSB
max17048.VALRT_LSB_V   = 0.020	-- 20mV/LSB
max17048.I2C_ADDR      = 0x36	-- 芯片固定地址（无地址引脚）

-- 可读字（测量/配置）；CMD 是只写口不在其中
local READABLE = {
	[0x02] = true, [0x04] = true, [0x06] = true, [0x08] = true,
	[0x0C] = true, [0x14] = true, [0x16] = true, [0x1A] = true,
}

local function clampi(v, lo, hi)
	if v < lo then return lo end
	if v > hi then return hi end
	return v
end

function max17048.new(opts)
	opts = opts or {}

	local d = {}
	local regs = {}				-- 偶地址 → 16 位值
	local ptr = 0				-- 当前寄存器指针（字节地址）
	local ver_id = opts.version or max17048.VER_DEFAULT
	local reset_cfg = opts.reset_cfg or max17048.CFG_DEFAULT
	local write_pred, read_pred
	local reset_count = 0
	local quickstart_count = 0
	d.write_log = {}			-- 全部写事务（字节表副本），含指针写/失败写

	-- 故障注入三件（与 regmap 约定同名）
	d.nack = false
	d.nack_read = false
	d.nack_write = false

	local function reseed()
		for k in pairs(regs) do regs[k] = nil end
		regs[max17048.REG.VCELL]  = 0
		regs[max17048.REG.SOC]    = 0
		regs[max17048.REG.MODE]   = 0
		regs[max17048.REG.VER]    = ver_id
		regs[max17048.REG.CONFIG] = reset_cfg
		regs[max17048.REG.VALRT]  = max17048.VALRT_DEFAULT
		regs[max17048.REG.CRATE]  = 0
		regs[max17048.REG.STATUS] = max17048.STATUS_POR
	end
	reseed()

	local function log_copy(bytes)
		local cp = {}
		for i = 1, #bytes do cp[i] = bytes[i] % 256 end
		d.write_log[#d.write_log + 1] = cp
	end

	-- ---------------- virtbus 点定义回调（无 self） ----------------

	-- 1 字节 {reg} = 纯指针写（任何地址都 ACK，读时才验可读性）；恰 3 字节
	-- {reg,hi,lo} = 写一个 16 位寄存器；其余长度在本器件协议下不存在，NACK。
	d.write = function(bytes)
		if type(bytes) ~= "table" or #bytes == 0 then return false end
		log_copy(bytes)
		if d.nack or d.nack_write or (write_pred and write_pred(bytes)) then
			return false
		end
		if #bytes ~= 1 and #bytes ~= 3 then return false end
		local p = bytes[1] % 256
		ptr = p
		if #bytes == 1 then return true end

		local word = (bytes[2] % 256) * 256 + (bytes[3] % 256)
		if p == max17048.REG.VCELL or p == max17048.REG.SOC
			or p == max17048.REG.CRATE or p == max17048.REG.VER then
			-- 测量结果区（VCELL/SOC/CRATE）/ROM（VERSION）：只读，写 ACK
			-- 但丢弃（测量值只能由注入 API 改）
			return true
		elseif p == max17048.REG.MODE then
			-- QuickStart（bit14）自清不落盘且计数；bit15 保留抹掉；
			-- HibStat（bit12）只读写忽略（模型恒 0）；EnSleep（bit13）
			-- 及其余低位照存
			if math.floor(word / 0x4000) % 2 == 1 then
				quickstart_count = quickstart_count + 1
			end
			local v = word % 0x4000					-- 抹 bit15/bit14
			regs[p] = v - math.floor(v / 0x1000) % 2 * 0x1000	-- 抹 bit12
			return true
		elseif p == max17048.REG.CONFIG
			or p == max17048.REG.VALRT
			or p == max17048.REG.STATUS then
			regs[p] = word
			return true
		elseif p == max17048.REG.CMD then
			-- 只写命令口：恰 0x5400 全片复位，其他值 ACK 忽略
			if word == max17048.CMD_RESET then
				reseed()
				reset_count = reset_count + 1
			end
			return true
		end
		-- 未定义偶地址/奇地址（含 HIBRT 0x0A、VRESET 0x18、TABLE 区）
		-- 字写：整笔 NACK
		return false
	end

	-- 读 n 字节：从当前指针寄存器取字的 hi/lo。驱动恒读恰 2 字节/寄存器；
	-- n>2 时字节流按偶地址（每 2 字节一字、字间 +2）连续——读 3 字节 =
	-- 当前字 hi/lo + 下一偶地址字 hi（无「奇尾补 0」，与真实字节流一致），
	-- 途中任一地址不可读整笔 NACK。
	d.read = function(n)
		if d.nack or d.nack_read then return nil end
		n = math.floor(tonumber(n) or 1)
		if n < 1 then return nil end
		if not READABLE[ptr] then return nil end
		if read_pred and read_pred(ptr, n) then return nil end
		local words = math.ceil(n / 2)
		for i = 0, words - 1 do
			if not READABLE[ptr + i * 2] then return nil end
		end
		local out = {}
		for i = 1, n do
			local wi = ptr + math.floor((i - 1) / 2) * 2
			local word = regs[wi] or 0
			out[i] = (i - 1) % 2 == 0 and math.floor(word / 256) % 256 or word % 256
		end
		return out
	end

	-- ---------------- 原始码注入（协议层主输入，冒号 API） ----------------

	-- VCELL 码：u16 0~65535（16 位直读，78.125µV/LSB）
	function d:set_vcell_raw(code)
		regs[max17048.REG.VCELL] = clampi(math.floor(tonumber(code) or 0),
		                                 0, max17048.VCELL_MAX)
		return self
	end

	-- SOC 码：u16 0~65535（1/256%，芯片可报告 >100%，不钳到 100）
	function d:set_soc_raw(word)
		regs[max17048.REG.SOC] = clampi(math.floor(tonumber(word) or 0), 0, 65535)
		return self
	end

	-- CRATE 码：s16 -32768~32767（0.208%/hr，充电正/放电负）
	function d:set_crate_raw(code)
		regs[max17048.REG.CRATE] = clampi(math.floor(tonumber(code) or 0),
		                                 -32768, 32767) % 65536
		return self
	end

	-- STATUS 原始 16 位字（高字节六告警 + EnVr，低字节照存）
	function d:set_status(word)
		regs[max17048.REG.STATUS] = clampi(math.floor(tonumber(word) or 0),
		                                  0, 65535)
		return self
	end

	-- 只置高字节低 6 位告警标志（RI/VH/VL/VR/HD/SC），保留 EnVr 与低字节
	function d:set_flags(bits)
		local cur = regs[max17048.REG.STATUS] or max17048.STATUS_POR
		local hi = (math.floor(bits) % 64)
		           + math.floor(cur / 0x4000) % 2 * 0x40	-- 保留 EnVr(高字节 bit6)
		regs[max17048.REG.STATUS] = hi * 256 + cur % 256
		return self
	end

	-- MODE/CONFIG/VALRT 等协议寄存器留原始 16 位码注入口
	function d:set_word(reg, word)
		reg = clampi(math.floor(reg), 0, 255)
		regs[reg] = clampi(math.floor(word) % 65536, 0, 65535)
		return self
	end

	-- ---------------- 物理量造样本（常量仅造样本用，与驱动独立） ----------------

	-- 电池电压 V（16 位、78.125µV/LSB 最近码；满码 5.11992V）
	function d:set_vcell_v(v)
		local code = clampi(math.floor(v / max17048.VCELL_LSB_V + 0.5),
		                    0, max17048.VCELL_MAX)
		regs[max17048.REG.VCELL] = code
		return self
	end

	-- 剩余电量 %（1/256% 定点最近码；允许 >100，u16 饱和 255.996%）
	function d:set_soc_pct(pct)
		local word = clampi(math.floor(pct * 256 + 0.5), 0, 65535)
		regs[max17048.REG.SOC] = word
		return self
	end

	-- 充放电速率 %/hr（0.208/LSB 最近码，s16 饱和）
	function d:set_crate_pcthr(rate)
		local code = clampi(math.floor(rate / max17048.CRATE_LSB + 0.5),
		                    -32768, 32767)
		regs[max17048.REG.CRATE] = code % 65536
		return self
	end

	-- VALRT 阈值（伏特；0x14=MIN 高字节、0x15=MAX 低字节，20mV 最近码）
	function d:set_valrt_v(vmin, vmax)
		local mn = clampi(math.floor((vmin or 0) / max17048.VALRT_LSB_V + 0.5),
		                  0, 255)
		local mx = clampi(math.floor((vmax or 0) / max17048.VALRT_LSB_V + 0.5),
		                  0, 255)
		regs[max17048.REG.VALRT] = mn * 256 + mx
		return self
	end

	-- ---------------- 守护仿真节拍（sensor_daemon 每拍调用，确定性） ----------------

	-- 32 拍周期，一节典型锂电：电压 3.85±0.25V（3.6~4.1V 常用区间）、
	-- 电量 80±8% 同相正弦，CRATE 取 SOC 变化率量级的余弦 ±9.98%/hr
	--（±48 码 ×0.208），无随机、E2E 数值可复现。
	--   n0  = 3.85V / 80% / CRATE=+9.984（cos 峰，电压中值上升沿）；
	--   n8  = 4.10V / 88% / CRATE≈0（电压峰）；
	--   n16 = 3.85V / 80% / CRATE=-9.984（cos 谷）；
	--   n24 = 3.60V / 72% / CRATE≈0（电压谷）。
	-- 连续转换无武装概念，直接更新结果寄存器（守护不驱动虚拟时钟）；
	-- STATUS 不触碰（告警只由注入 API 改）。
	function d:virt_tick(n)
		n = tonumber(n) or 0
		local s = math.sin(n * (2 * math.pi / 32))
		local c = math.cos(n * (2 * math.pi / 32))
		self:set_vcell_v(3.85 + 0.25 * s)
		self:set_soc_pct(80 + 8 * s)
		self:set_crate_pcthr(48 * max17048.CRATE_LSB * c)
		return self
	end

	-- ---------------- 身份/留痕/故障钩子 ----------------

	-- 覆写 CONFIG 复位默认值（立即置于该值，CMD 复位后也是它；nil=不改）
	function d:set_reset_cfg(word)
		if word ~= nil then
			reset_cfg = math.floor(word) % 65536
			regs[max17048.REG.CONFIG] = reset_cfg
		end
		return self
	end

	-- 覆写 VERSION（nil=不改；CMD 全片复位不恢复，测试 section 须显式恢复）
	function d:who(ver)
		if ver ~= nil then
			ver_id = math.floor(ver) % 65536
			regs[max17048.REG.VER] = ver_id
		end
		return self
	end

	-- 驱动 build/call 写过的 3 字节字写 {reg,hi,lo}；1 字节事务是读指针
	function d:config_writes()
		local out = {}
		for _, e in ipairs(self.write_log) do
			if #e == 3 then
				out[#out + 1] = { reg = e[1], hi = e[2], lo = e[3],
				                  word = e[2] * 256 + e[3] }
			end
		end
		return out
	end

	function d:reset_count() return reset_count end
	function d:quickstart_count() return quickstart_count end
	function d:pointer() return ptr end

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

	-- 回上电态（保留故障钩子与 who()/set_reset_cfg() 身份，复位不得解除钩子）
	function d:reset()
		reseed()
		reset_count = 0
		quickstart_count = 0
		ptr = 0
		self.write_log = {}
		return self
	end

	-- ---------------- 观察口 ----------------

	local function s16(word)
		word = word % 65536
		if word >= 32768 then word = word - 65536 end
		return word
	end

	function d:version() return regs[max17048.REG.VER] or 0 end
	function d:config()  return regs[max17048.REG.CONFIG] or 0 end
	function d:mode()    return regs[max17048.REG.MODE] or 0 end
	function d:valrt()   return regs[max17048.REG.VALRT] or 0 end
	function d:status()  return regs[max17048.REG.STATUS] or 0 end
	function d:word(reg) return regs[reg] or 0 end

	-- VALRT 两侧阈值码（Figure 11：高字节=MIN、低字节=MAX）
	function d:valrt_min_code()
		return math.floor((regs[max17048.REG.VALRT] or 0) / 256) % 256
	end
	function d:valrt_max_code()
		return (regs[max17048.REG.VALRT] or 0) % 256
	end

	-- 高字节低 6 位告警标志；bit6 EnVr 单独观察
	function d:flags()
		return math.floor((regs[max17048.REG.STATUS] or 0) / 256) % 64
	end
	function d:envr()
		return math.floor((regs[max17048.REG.STATUS] or 0) / 0x4000) % 2
	end

	-- VCELL 寄存器字（u16 直读码）
	function d:vcell_raw()
		return regs[max17048.REG.VCELL] or 0
	end
	function d:soc_raw()
		return regs[max17048.REG.SOC] or 0
	end
	function d:crate_raw()
		return s16(regs[max17048.REG.CRATE] or 0)
	end

	function d:vcell_v()  return self:vcell_raw() * max17048.VCELL_LSB_V end
	function d:soc_pct()  return self:soc_raw() / 256 end
	function d:crate_pcthr() return self:crate_raw() * max17048.CRATE_LSB end

	return d
end

return max17048
