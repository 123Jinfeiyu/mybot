-- modules/lib/virtbus/bme688.lua —— Bosch BME688 四合一环境传感器虚拟器件模型
-- bot4cj 虚拟总线器件库（无 BUS 字段 = I2C，attach 动态委托 dev.write/dev.read）
-- 对应真实驱动：modules/BME688.lua（同协议独立实现，防互证假绿；改协议两处同步）
--
-- 定位：【8 位寄存器指针 + 稀疏寄存器图 + 3 块 NVM 校准 + forced 转换状态机 +
-- 双气体量程路】的器件样板（BME688 兼容 BME680，I2C 地址 0x76/0x77，Chip ID
-- 0x61）。寄存器指针自增、burst 跨块连续（i2c_read_multi 按 6 字节分块）。
-- 同族 bme280.lua（regmap + NVM + forced 门），BME688 的新增协议面：
--   1) 校准 3 块 42 字节：COEFF1 0x8A×23 / COEFF2 0xE1×14 / COEFF3 0x00×5，
--      ROM 只读，H1/H2 在字节 24/25/26 半字节打包，res_heat_range 与
--      range_sw_err 是 COEFF3 的位场（后者为有符号高 4 位）。
--   2) 17 字节数据场（0x1D 起）：20 位 P/T + 16 位 H + 两条 10 位气体路
--      （低量程 0x2A/0x2B、高量程 0x2C/0x2D，各带 GASM_VALID/HEAT_STAB 标志
--      与 gas_range 低 4 位）；驱动按变体 0xF0 选路（0x00=BME680 低/0x01
--      =BME688 高）。输出寄存器是防撕裂影子缓冲，转换结束才整体装入。
--   3) forced 转换门：写 CTRL_MEAS(0x74) 低 2 位=01 触发一次 TPHG 循环后
--      自动回 sleep；门限 = TPH 时长（bme68x_get_meas_dur 公式，OSR 由
--      0x74/0x72 现解码：cycles*1963+1908+2385+1000 us）+ gas_wait（仅当
--      RUN_GAS 域 0x71 bit5:4 ≠0 且 heat_off 0x70 bit3=0 时计入，取
--      nb_conv 索引的 GAS_WAIT 寄存器，编码 (v%64)*4^floor(v/64) ms）。
--      默认 build 档（OSR×1/1/1 + 150ms 加热）门限 12+148=160ms。
--   4) NEW_DATA：0x1D bit7，datasheet 原文「data field contains a new data,
--      not already read-out」——读出数据场即清（任何含 0x1D 的读事务在
--      返回后清位），下次 forced 触发也清；转换中 bit5 measuring/bit6
--      gas_measuring 合成置 1。模式位 0x74 读回为动态值（转换中=01，
--      完成自动清 0），测量中对控制/加热器寄存器的写 ACK 但忽略。
--   5) 软复位：0xE0 写 0xB6 重种芯片态（NVM/数据场 POR、reset_count++）；
--      CHIP_ID/VARIANT 是 ROM，软复位不变——who() 误挂覆写跨软复位保留
--      （build 先软复位再读 ID 的误挂甄别才有效），仅上电 reset 回出厂。
--      注入的待测测量码、预武装、故障钩子同样保留（注入代表测试环境而非
--      芯片状态，守护 build 的软复位不能解除 tick(0) 预武装）。
--
-- 【边界约定】模型只存协议事实与原始 ADC 码：Bosch 温度/气压/湿度/气体
-- 补偿公式（含 K1/K2 表、res_heat/gas_wait 编码的「语义换算」）只存在于
-- 驱动与测试独立参考中，模型绝不转录。virt_tick 的 32 拍原始码表是离线
-- 码钉（用独立 Bosch float 公式反解后固化，码表本身不含公式），物理区间
-- T 22~28°C / P 100725~101925Pa / H 35~65%RH / GAS 6~18kΩ，守护据此
-- 经真实驱动补偿出物理值。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数）。
-- LuaJIT 兼容：无 5.4 位运算符、无 //，位场一律 floor/% 算术。

local M = {}
M._VERSION = "0.1.0"
M.BUS = "i2c"

-- ==================== 协议常量 ====================
local R_FIELD      = 0x1D		-- 数据场 0 起（17 字节 0x1D..0x2D）
local R_IDAC0      = 0x50		-- 10 个 IDAC 0x50..0x59
local R_RES_HEAT0  = 0x5A		-- 10 个 RES_HEAT 0x5A..0x63
local R_GAS_WAIT0  = 0x64		-- 10 个 GAS_WAIT 0x64..0x6D
local R_CTRL_GAS0  = 0x70		-- bit3 heat_off（0=加热器使能）
local R_CTRL_GAS1  = 0x71		-- bit5:4 RUN_GAS，bit3:0 NBCONV
local R_CTRL_HUM   = 0x72		-- bit6 spi_mem_page，bit2:0 OSR_H
local R_CTRL_MEAS  = 0x74		-- bit7:5 OSR_T，bit4:2 OSR_P，bit1:0 模式
local R_CONFIG     = 0x75		-- bit4:2 IIR，bit0 spi3w
local R_CHIP_ID    = 0xD0
local R_VARIANT    = 0xF0		-- 0x00=BME680 低量程 / 0x01=BME688 高量程
local R_RESET      = 0xE0
local RST_CMD      = 0xB6

-- COEFF1 0x8A×23 / COEFF2 0xE1×14 / COEFF3 0x00×5（42 字节，1 基）
local C1_BASE, C1_LEN = 0x8A, 23
local C2_BASE, C2_LEN = 0xE1, 14
local C3_BASE, C3_LEN = 0x00, 5

-- OSR 码 → 测量周期数（bme68x_defs.h os_to_meas_cycles = {0,1,2,4,8,16}）
local OSC = { 0, 1, 2, 4, 8, 16, 16, 16 }

-- 42 字节 NVM 出厂种子（确定性校准组：t1=26280/p1=37103/h1=706 等，
-- 含 p8=-4000 的 s16 补码字节 b19=0x60,b20=0xF0 与 range_sw_err=2）。
-- 解析语义由驱动 parse_calib / 测试独立参考各自转录，模型只存字节。
local NVM = {
	137, 103, 4, 0,  239, 144, 176, 254, 88, 0,
	107, 12, 64, 0,  189, 45, 0, 0,  96, 240, 199, 247, 100,
	55, 178, 44, 0, 45, 236, 120, 156,
	168, 102, 190, 209, 228, 64,
	30, 0, 16, 0, 32,
}

-- virt_tick 32 拍离线码钉 {t20,p20,h16,gas10,gas_range}（每拍气体两路
-- 同码）。由独立 Bosch float 公式从物理目标反解（n0: 25.0°C/101844.6Pa/
-- 37.0%RH/12000Ω），回算误差 T<0.001°C、P<0.1Pa、H<0.002%RH、GAS<0.1%；
-- gas_range 12/13 为高量程路自动选档。测试组对 n0/n8/n24 复核原始码。
local TICK_RAWS = {
	{ 499588, 398974, 19950, 924, 12 },
	{ 501440, 399361, 19702, 132, 13 },
	{ 503221, 399843, 19563, 246, 13 },
	{ 504861, 400402, 19541, 377, 13 },
	{ 506300, 401017, 19636, 522, 13 },
	{ 507480, 401664, 19845, 668, 13 },
	{ 508357, 402320, 20156, 799, 13 },
	{ 508897, 402958, 20555, 891, 13 },
	{ 509080, 403555, 21025, 924, 13 },
	{ 508897, 404088, 21543, 891, 13 },
	{ 508357, 404535, 22089, 799, 13 },
	{ 507480, 404881, 22641, 668, 13 },
	{ 506300, 405110, 23177, 522, 13 },
	{ 504861, 405215, 23678, 377, 13 },
	{ 503221, 405190, 24127, 246, 13 },
	{ 501440, 405037, 24509, 132, 13 },
	{ 499588, 404762, 24811, 924, 12 },
	{ 497737, 404374, 25024, 766, 12 },
	{ 495956, 403889, 25141, 639, 12 },
	{ 494315, 403325, 25160, 538, 12 },
	{ 492877, 402705, 25079, 460, 12 },
	{ 491697, 402053, 24901, 402, 12 },
	{ 490819, 401393, 24632, 363, 12 },
	{ 490279, 400752, 24279, 339, 12 },
	{ 490097, 400155, 23854, 332, 12 },
	{ 490279, 399623, 23370, 339, 12 },
	{ 490819, 399179, 22845, 363, 12 },
	{ 491697, 398838, 22297, 402, 12 },
	{ 492877, 398613, 21747, 460, 12 },
	{ 494315, 398514, 21216, 538, 12 },
	{ 495956, 398542, 20725, 639, 12 },
	{ 497737, 398698, 20297, 766, 12 },
}

local function clamp(v, lo, hi)
	v = math.floor(tonumber(v) or 0)
	if v < lo then return lo elseif v > hi then return hi else return v end
end

function M.new(opts)
	opts = opts or {}
	local d = {}

	-- 芯片侧状态
	local regs = {}			-- 稀疏寄存器（0..255，未定义地址读 0）
	local ptr = 0
	local clock_ms = 0
	local busy = false		-- forced 转换中
	local remaining = nil	-- 剩余门限 ms（外部时钟模式）
	local last_gate = 0
	local prearmed = false		-- virt_tick：触发即就绪（守护零真实睡眠）
	local never_ready = false	-- 门限永不到点（超时负路径）
	local new_data = false
	local meas_index = 0
	local reset_count = 0
	local write_count, read_count = 0, 0
	local frames = {}		-- 写事务副本（断言用）

	-- 身份 ROM（who 可覆写；reset/软复位回出厂 opts）
	local factory_chip = opts.chip or 0x61
	local factory_variant = opts.variant or 0x01
	local chip_id, variant = factory_chip, factory_variant

	-- 下一次转换完成时装入影子缓冲的原始码（注入 API 改，转换不改注入）
	local pend = {
		t = 524288, p = 300000, h = 30000,
		lo = { adc = 600, range = 8, valid = true, stab = true },
		hi = { adc = 600, range = 8, valid = true, stab = true },
	}

	-- 故障钩子（reset 保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	-- ---------------- 寄存器初态 ----------------

	local function seed_field_por()
		-- datasheet POR：press_msb/temp_msb/hum_msb=0x80（跳过标记），余 0；
		-- 0x1D 状态位不落地（读时按状态机合成）
		regs[0x1F], regs[0x22], regs[0x25] = 0x80, 0x80, 0x80
	end

	local function seed_chip()
		-- 软复位：CHIP_ID/VARIANT 是 ROM 常量，真机软复位不变（who 覆写代表
		-- 误挂别的芯片，软复位不能「洗」回 0x61/0x01，否则误挂甄别被架空）；
		-- 身份回出厂只发生在上电 d:reset()
		regs = {}
		seed_field_por()
		busy, remaining, new_data = false, nil, false
		meas_index, clock_ms, last_gate = 0, 0, 0
	end

	-- ---------------- 转换状态机 ----------------

	-- TPHG 门限（ms）：TPH 时长按 OSR 现解码 + 使能时的 gas_wait
	local function cur_gate_ms()
		local cm = regs[R_CTRL_MEAS] or 0
		local ch = regs[R_CTRL_HUM] or 0
		local ct = OSC[math.floor(cm / 32) % 8 + 1]
		local cp = OSC[math.floor(cm / 4) % 8 + 1]
		local cH = OSC[(ch % 8) + 1]
		local us = (ct + cp + cH) * 1963 + 477 * 4 + 477 * 5 + 1000
		local tph = math.floor(us / 1000)
		if us % 1000 ~= 0 then tph = tph + 1 end
		local gasms = 0
		local g1, g0 = regs[R_CTRL_GAS1] or 0, regs[R_CTRL_GAS0] or 0
		if math.floor(g1 / 16) % 4 ~= 0 and math.floor(g0 / 8) % 2 == 0 then
			-- RUN_GAS 域 bit5:4 =01 低量程/10 高量程，且 heat_off bit3=0
			-- （域值 11 保留，按禁用处理）
			local v = regs[R_GAS_WAIT0 + (g1 % 16)] or 0
			gasms = (v % 64) * 4 ^ math.floor(v / 64)
		end
		return tph + gasms
	end

	local function pack_gas(into, g)
		local flags = (g.valid and 0x20 or 0) + (g.stab and 0x10 or 0)
		into.msb = math.floor(g.adc / 4) % 256
		into.lsb = (g.adc % 4) * 64 + flags + (g.range % 16)
	end

	local function fill_field()
		-- 转换结束：影子缓冲整体装入（防撕裂），meas_index 递增
		regs[0x1F] = math.floor(pend.p / 4096) % 256
		regs[0x20] = math.floor(pend.p / 16) % 256
		regs[0x21] = (pend.p % 16) * 16
		regs[0x22] = math.floor(pend.t / 4096) % 256
		regs[0x23] = math.floor(pend.t / 16) % 256
		regs[0x24] = (pend.t % 16) * 16
		regs[0x25] = math.floor(pend.h / 256) % 256
		regs[0x26] = pend.h % 256
		local gl, gh = {}, {}
		pack_gas(gl, pend.lo); pack_gas(gh, pend.hi)
		regs[0x2A], regs[0x2B] = gl.msb, gl.lsb
		regs[0x2C], regs[0x2D] = gh.msb, gh.lsb
		regs[0x1E] = meas_index
		meas_index = (meas_index + 1) % 256
	end

	local function finish_conv()
		busy, remaining = false, 0
		fill_field()
		new_data = true
	end

	local function start_conv()
		new_data = false
		last_gate = cur_gate_ms()
		if never_ready then
			busy, remaining = true, nil
		elseif prearmed then
			busy, remaining = true, 0
			finish_conv()
		else
			busy, remaining = true, last_gate
		end
	end

	-- 单测虚拟时钟：驱动 milli_sleep 桩转调；门限到点自动装场
	function d:advance(ms)
		clock_ms = clock_ms + math.max(0, math.floor(tonumber(ms) or 0))
		if busy and remaining ~= nil then
			remaining = remaining - math.max(0, math.floor(tonumber(ms) or 0))
			if remaining <= 0 then finish_conv() end
		end
		return self
	end

	-- ---------------- 地址解码（动态/ROM/寄存器） ----------------

	local function nvm_byte(a)
		if a >= C1_BASE and a < C1_BASE + C1_LEN then
			return NVM[a - C1_BASE + 1]
		end
		if a >= C2_BASE and a < C2_BASE + C2_LEN then
			return NVM[C1_LEN + (a - C2_BASE) + 1]
		end
		if a >= C3_BASE and a < C3_BASE + C3_LEN then
			return NVM[C1_LEN + C2_LEN + (a - C3_BASE) + 1]
		end
		return nil
	end

	-- 转换中读：门限惰性到点（advance 已推时钟时首次读即完成）
	local function check_due()
		if busy and remaining ~= nil and remaining <= 0 then finish_conv() end
	end

	local function byte_at(a)
		if a == R_FIELD then
			if busy then return 0x60 end		-- measuring+gas_measuring，index=0
			return new_data and 0x80 or 0x00
		end
		if a == R_CTRL_MEAS then
			return (regs[a] or 0) - ((regs[a] or 0) % 4) + (busy and 1 or 0)
		end
		if a == R_CHIP_ID then return chip_id end
		if a == R_VARIANT then return variant end
		local nb = nvm_byte(a)
		if nb ~= nil then return nb end
		return regs[a] or 0
	end

	-- ---------------- 总线回调（点定义，无 self） ----------------

	local function is_byte(v)
		return type(v) == "number" and v == math.floor(v) and v >= 0 and v <= 255
	end

	local function is_ro(a)
		return (a >= R_FIELD and a <= 0x2D)
			or (a >= C1_BASE and a < C1_BASE + C1_LEN)
			or (a >= C2_BASE and a < C2_BASE + C2_LEN)
			or (a >= C3_BASE and a < C3_BASE + C3_LEN)
			or a == R_CHIP_ID or a == R_VARIANT
	end

	local function put_byte(a, v)
		if a == R_RESET then return end		-- 复位命令在帧层处理，不落盘
		if is_ro(a) then return end
		-- 测量中：控制/加热器寄存器写被器件忽略（datasheet 5.3.6.2）
		if busy and a >= 0x50 and a <= 0x75 then return end
		if a == R_CTRL_GAS0 then
			regs[a] = (math.floor(v / 8) % 2) * 8	-- 仅 bit3 heat_off 有定义
		elseif a == R_CTRL_GAS1 then
			regs[a] = v % 64				-- bit5:4 RUN_GAS + bit3:0 NBCONV
		elseif a == R_CTRL_HUM then
			regs[a] = (math.floor(v / 64) % 2) * 64 + (v % 8)	-- bit6 + OSR_H
		elseif a == R_CTRL_MEAS then
			regs[a] = v - (v % 4)	-- 模式位动态，不落地
		elseif a == R_CONFIG then
			regs[a] = (math.floor(v / 4) % 8) * 4 + (v % 2)	-- IIR bit4:2 + bit0
		else
			regs[a] = v
		end
	end

	function d.write(bytes)
		write_count = write_count + 1
		if type(bytes) ~= "table" or #bytes == 0 then return false end
		for i = 1, #bytes do
			if not is_byte(bytes[i]) then return false end
		end
		if d.nack or d.nack_write then return false end
		if write_pred and write_pred(bytes) then return false end

		local cp = {}
		for i = 1, #bytes do cp[i] = bytes[i] end
		frames[#frames + 1] = cp

		if #bytes == 1 then
			ptr = bytes[1]
			return true
		end
		local base = bytes[1]
		for i = 2, #bytes do
			local a, v = (base + i - 2) % 256, bytes[i]
			if a == R_RESET and v == RST_CMD then
				-- 软复位：重种芯片态；注入码/预武装/故障钩子保留
				seed_chip()
				reset_count = reset_count + 1
			else
				put_byte(a, v)
			end
		end
		if (bytes[1] == R_CTRL_MEAS) and (not busy) and (bytes[2] % 4 == 1) then
			start_conv()
		end
		return true
	end

	function d.read(n)
		n = math.floor(tonumber(n) or 1)
		if n < 1 then n = 1 elseif n > 16 then n = 16 end
		if d.nack or d.nack_read then return nil end
		if read_pred and read_pred(ptr, n) then return nil end
		check_due()
		local t, touched = {}, false
		for _ = 1, n do
			if ptr == R_FIELD then touched = true end
			t[#t + 1] = byte_at(ptr)
			ptr = (ptr + 1) % 256
		end
		-- 「new data, not already read-out」：含状态字节的读事务返回后清位
		if touched then new_data = false end
		read_count = read_count + 1
		return t
	end

	-- ---------------- 测试 / 守护 API（冒号定义） ----------------

	-- 注入下一次转换的 T/P/H 原始码（nil 保持原值；T/P 20 位、H 16 位）
	function d:set_tph_raw(t, p, h)
		if t ~= nil then pend.t = clamp(t, 0, 0xFFFFF) end
		if p ~= nil then pend.p = clamp(p, 0, 0xFFFFF) end
		if h ~= nil then pend.h = clamp(h, 0, 0xFFFF) end
		return self
	end

	-- 注入某条气体路原始码：path="low"(0x2A/2B BME680) | "high"(0x2C/2D BME688)；
	-- nil 参数保持原值（故 set_gas_flags 可只改两标志）
	function d:set_gas_raw(path, adc, range, valid, stab)
		local g = path == "low" and pend.lo or pend.hi
		if adc ~= nil then g.adc = clamp(adc, 0, 1023) end
		if range ~= nil then g.range = clamp(range, 0, 15) end
		if valid ~= nil then g.valid = valid and true or false end
		if stab ~= nil then g.stab = stab and true or false end
		return self
	end

	function d:set_gas_flags(path, valid, stab)
		return self:set_gas_raw(path, nil, nil, valid, stab)
	end
	function d:set_meas_index(n)
		meas_index = clamp(n, 0, 255)
		return self
	end

	function d:set_never_ready(on)
		never_ready = on ~= false
		return self
	end

	function d:who(chip, v)
		if chip ~= nil then chip_id = math.floor(chip) % 256 end
		if v ~= nil then variant = math.floor(v) % 256 end
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred) read_pred = pred; return self end
	function d:clear_faults()
		d.nack, d.nack_read, d.nack_write = false, false, false
		write_pred, read_pred = nil
		never_ready = false
		return self
	end

	function d:reset_logs()
		frames = {}
		write_count, read_count = 0, 0
		return self
	end

	-- 上电复位：芯片态回出厂、注入回默认、预武装解除；故障钩子保留
	function d:reset()
		seed_chip()
		chip_id, variant = factory_chip, factory_variant
		reset_count, ptr = 0, 0
		pend = {
			t = 524288, p = 300000, h = 30000,
			lo = { adc = 600, range = 8, valid = true, stab = true },
			hi = { adc = 600, range = 8, valid = true, stab = true },
		}
		prearmed, never_ready = false, false
		frames = {}
		write_count, read_count = 0, 0
		return self
	end

	-- 守护逐拍造数：装离线码钉 + 预武装（触发即就绪，零真实转换睡眠）
	function d:virt_tick(n)
		local row = TICK_RAWS[(math.floor(tonumber(n) or 0) % 32) + 1]
		pend.t, pend.p, pend.h = row[1], row[2], row[3]
		pend.lo.adc, pend.lo.range, pend.lo.valid, pend.lo.stab = row[4], row[5], true, true
		pend.hi.adc, pend.hi.range, pend.hi.valid, pend.hi.stab = row[4], row[5], true, true
		prearmed = true
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:pointer() return ptr end
	function d:write_count() return write_count end
	function d:read_count() return read_count end
	function d:is_busy() return busy end
	function d:has_new_data() return new_data end
	function d:remaining_ms() return remaining end
	function d:reset_count() return reset_count end
	function d:meas_index() return meas_index	-- 下一场将装入的索引
	end
	function d:last_gate_ms() return last_gate end
	function d:chip_id() return chip_id end
	function d:variant() return variant end
	function d:is_prearmed() return prearmed end
	function d:regs() return regs end
	function d:frames() return frames end
	function d:nvm()
		local cp = {}
		for i = 1, #NVM do cp[i] = NVM[i] end
		return cp
	end
	function d:raw()
		return pend.t, pend.p, pend.h,
			pend.hi.adc, pend.hi.range, pend.hi.valid, pend.hi.stab,
			pend.lo.adc, pend.lo.range, pend.lo.valid, pend.lo.stab
	end
	-- 当前配置解码（OSR 码/气体使能/本拍 gas_wait ms）
	function d:config()
		local cm = regs[R_CTRL_MEAS] or 0
		local ch = regs[R_CTRL_HUM] or 0
		local g1, g0 = regs[R_CTRL_GAS1] or 0, regs[R_CTRL_GAS0] or 0
		local gw = 0
		if math.floor(g1 / 16) % 4 ~= 0 and math.floor(g0 / 8) % 2 == 0 then
			local v = regs[R_GAS_WAIT0 + (g1 % 16)] or 0
			gw = (v % 64) * 4 ^ math.floor(v / 64)
		end
		return {
			os_t = math.floor(cm / 32) % 8,
			os_p = math.floor(cm / 4) % 8,
			os_h = ch % 8,
			run_gas = math.floor(g1 / 16) % 4,
			heat_off = math.floor(g0 / 8) % 2,
			nb_conv = g1 % 16,
			iir = math.floor((regs[R_CONFIG] or 0) / 4) % 8,
			gas_wait_ms = gw,
		}
	end

	seed_chip()
	return d
end

return M
