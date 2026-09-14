-- mlx90393.lua —— Melexis MLX90393 Triaxis 三轴磁力计虚拟器件模型
-- bot4cj 虚拟硬件库（modules/lib/virtbus/，配合 modules/lib/virtbus.lua 使用）
--
-- 定位：【命令字 + status-first 应答 + 易失内存读写的命令式 I2C 器件】——没有
-- 寄存器指针事务：每条命令先一笔写（NOP 0x00 无副作用，驱动用它轮询），
-- 再 Sr 纯读；应答【首字节永远是 status】，其后才是数据（datasheet
-- 3901090393 Rev012 §15，与 Adafruit/tedyapo 实现交叉验证）。
--
-- 只复刻协议层事实：
--   1) 命令集（Table 11）：0x00 NOP | 0x1n SB 突发 | 0x2n SW 唤醒 |
--      0x3n SM 单次 | 0x4n RM 读测量 | 0x50 RR（2B [0x50,A<<2]，
--      应答 3B [status,Dhi,Dlo]）| 0x60 WR（【4B】 [0x60,Dhi,Dlo,A<<2]，
--      应答 1B status）| 0x80 EX | 0xD0 HR | 0xE0 HS | 0xF0 RT。
--      zyxt 低 4 位 bit0=T/bit1=X/bit2=Y/bit3=Z；=0 时取 reg0x01 的
--      BURST_SEL。RR/WR 的 abc（命令低 3 位/地址字节低 2 位）必须为 0。
--   2) I2C 上命令恒 ACK（即使非法）——非法命令靠随后的 status ERROR 位
--      甄别（模型：ACK + ERROR 锁存）；只有帧长/abc 坏帧走总线 NACK。
--   3) status（§15.2 Table 12）：bit7 BURST_MODE | bit6 WOC_MODE |
--      bit5 SM_MODE（单次转换完成）| bit4 ERROR | bit3 SED | bit2 RS
--      （RT 后首 status 置位、读/推进自清）| bit1:0 D（仅 RR/RM 有效：
--      附加数据字节数 = 2D+2；RM XYZT 4 分量 D=3，全帧含 status 9B）。
--   4) RM 按 Status-TXYZ 组帧，zyxt 中为 0 的分量跳过（Figure 9/17）；
--      DRDY 低时读数据 → ERROR 且无数据。SM 一次转换完回 IDLE，结果锁存
--      到被 RM 读走；SB 突发首转换后数据持续刷新、RM 不消费。
--   5) 易失内存 64×16bit：客户区 0x00~0x1F 可写，Melexis 区 0x20~0x3F
--      写保护（WR 被忽略 + ERROR，读允许）；RT/HR 重灌种子。
--      reg0x00 = Z-SERIES[15]/GAIN_SEL[14:12]/HALLCONF[11:8]，POR=0x007C。
--   6) 转换门（§电气特性，OSR/DIG_FILT/OSR2 全 0）：单磁轴
--      67+64·2^OSR·(2+2·DIG_FILT) µs、温度 67+192·2^OSR2 µs，
--      XYZT 全转换约 1.3ms（模型 ceil 2ms）；advance 虚拟时钟门，
--      virt_tick 预武装让守护零真实睡眠。
--
-- 换算公式归驱动（µT/LSB 见 Table 17；温度 45.2LSB/°C、25°C=46244），
-- 模型只存原始码（磁场 s16、温度 u16），set_sample 物理量 API 仅供
-- 守护 virt_tick 造样本（常量与驱动同源，独立测试另行交叉）。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数，
-- attach 动态委托 dev.write(bytes) 契约）。LuaJIT 兼容：无 5.4 位运算符。

local mpath = dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua")
local bitc = dofile(mpath.resolve("bitcompat.lua"))

local M = {}
M._VERSION = "0.1.0"

M.I2C_ADDR = 0x0C			-- EE_I2C_ADDR=0x03 时 0x0C/0x0D/0x0E/0x0F，驱动用 0x0C
M.POR_GAIN = 0x007C		-- reg0x00 上电值：GAIN_SEL=7、HALLCONF=0xC
M.T_RAW_25C = 46244		-- 25°C 温度原始输出（45.2 LSB/°C）

-- status 位（datasheet §15.2）
local ST_BURST, ST_WOC, ST_SM, ST_ERROR, ST_RS = 0x80, 0x40, 0x20, 0x10, 0x04

-- GAIN_SEL 0~7 相对 SEL=7（最高增益 1x）的增益倍数（Table 17）
local GAIN_MULT = { 5, 4, 3, 2.5, 2, 5 / 3, 4 / 3, 1 }

local function clamp16(v)
	v = math.floor(tonumber(v) or 0)
	if v < 0 then return 0 elseif v > 65535 then return 65535 else return v end
end
local function clamp_s16(v)
	v = math.floor(tonumber(v) or 0)
	if v < -32768 then return -32768 elseif v > 32767 then return 32767 else return v end
end
-- 四舍五入（含负值：-0.6 -> -1；按幅度取整，避免 -75.1/0.751 的二进制
-- 表示误差 99.9999999 被旧 floor(v-0.5) 误杀成 -101）
local function rround(v)
	if v >= 0 then return math.floor(v + 0.5) end
	return -math.floor(-v + 0.5)
end

function M.new()
	local d = {}

	-- ---------------- 芯片侧状态 ----------------
	local regs = {}						-- 64×16bit 易失内存（缺省 0）
	local mode = "idle"					-- idle | single | burst | woc
	local sel = 0						-- 最近一次 SM/SB 的 zyxt 分量
	local drdy = false					-- single：转换完成结果锁存；burst：首转换后恒真
	local remaining = nil				-- 转换剩余 ms（nil=无转换进行中）
	local prearmed = false				-- virt_tick 预武装：下一次 SM 零转换门
	local never_ready = false			-- 转换永不完成（轮询超时注入）
	local error_latch = false			-- status ERROR 位锁存（被接受的命令清除）
	local rs_latch = false				-- RT 后首 status 的 RS 位
	local pending = nil					-- nil | {k="status"} | {k="rr",a=} | {k="rm",m=}
	local sample_t, sample_x, sample_y, sample_z = M.T_RAW_25C, 0, 0, 0
	local cmds, unknown, frames = {}, {}, {}

	-- 故障钩子（reset 保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	local function reseed()
		for k in pairs(regs) do regs[k] = nil end
		regs[0x00] = M.POR_GAIN
	end
	reseed()

	-- 当前 GAIN_SEL/RES 配置下的 µT/LSB（与驱动 Table 17 同源，仅供 set_sample）
	local function sensitivities()
		local g = bitc.band(bitc.rshift(regs[0x00] or 0, 4), 0x7)
		local mult = GAIN_MULT[g + 1]
		return 0.751 * mult / 5, 1.210 * mult / 5
	end

	-- status 合成：dbits 仅 RR/RM 应答有效（附加数据 2D+2 字节）
	local function build_status(dbits)
		local s = dbits or 0
		if mode == "burst" then s = s + ST_BURST end
		if mode == "woc" then s = s + ST_WOC end
		if mode == "single" and drdy then s = s + ST_SM end
		if error_latch then s = s + ST_ERROR end
		if rs_latch then s = s + ST_RS end
		return s
	end

	-- 转换门 ms（§电气特性公式；RES 只选输出位宽、不影响时间）
	local function conv_ms(mask)
		local r2 = regs[0x02] or 0
		local hi = math.floor(r2 / 256) % 256
		local lo = r2 % 256
		local osr = bitc.band(hi, 0x03)
		local dig = bitc.band(hi, 0x0C) / 4
		local osr2 = bitc.band(lo, 0xC0) / 64
		local tmag = 0.067 + 0.064 * 2 ^ osr * (2 + 2 * dig)
		local ttmp = 0.067 + 0.192 * 2 ^ osr2
		local nm = (bitc.band(mask, 0x2) ~= 0 and 1 or 0)
			+ (bitc.band(mask, 0x4) ~= 0 and 1 or 0)
			+ (bitc.band(mask, 0x8) ~= 0 and 1 or 0)
		local total = 0.36 + 0.10 + nm * tmag
			+ (bitc.band(mask, 0x1) ~= 0 and ttmp or 0)
		local ms = math.ceil(total)
		if ms < 1 then ms = 1 end
		return ms
	end

	local function start_single(mask)
		mode, sel, drdy = "single", mask, false
		error_latch = false
		if prearmed and not never_ready then
			remaining, drdy = 0, true
		elseif never_ready then
			remaining = nil
		else
			remaining = conv_ms(mask)
		end
		prearmed = false
	end

	-- ---------------- 总线回调（点定义，无 self） ----------------

	function d.write(bytes)
		if type(bytes) ~= "table" or #bytes == 0 then return false end
		frames[#frames + 1] = {}
		for i = 1, #bytes do frames[#frames][i] = bytes[i] % 256 end
		if d.nack or d.nack_write or (write_pred and write_pred(bytes)) then
			return false
		end

		local c = bytes[1] % 256
		local hi = math.floor(c / 16) % 16
		local nib = c % 16

		if c == 0x00 then									-- NOP
			if #bytes ~= 1 then return false end
			error_latch = false
			pending = { k = "status" }

		elseif hi == 0x1 or hi == 0x2 then					-- SB / SW
			if #bytes ~= 1 then return false end
			if mode ~= "idle" then
				error_latch = true; pending = { k = "status" }; unknown[#unknown + 1] = c
				return true
			end
			local mask = nib ~= 0 and nib or (regs[0x01] or 0) % 16
			if mask == 0 then
				error_latch = true; pending = { k = "status" }; unknown[#unknown + 1] = c
				return true
			end
			mode = (hi == 0x1) and "burst" or "woc"
			sel, drdy = mask, false
			if never_ready then
				remaining = 1e9
			else
				remaining = prearmed and 0 or conv_ms(mask)
			end
			prearmed = false
			error_latch = false
			pending = { k = "status" }

		elseif hi == 0x3 then								-- SM
			if #bytes ~= 1 then return false end
			-- 仅 IDLE 接受：拒答时期望模式位保持清零 + ERROR
			if mode ~= "idle" then
				error_latch = true; pending = { k = "status" }; unknown[#unknown + 1] = c
				return true
			end
			local mask = nib ~= 0 and nib or (regs[0x01] or 0) % 16
			if mask == 0 then
				error_latch = true; pending = { k = "status" }; unknown[#unknown + 1] = c
				return true
			end
			start_single(mask)
			pending = { k = "status" }

		elseif hi == 0x4 then								-- RM（就绪与否在读时判）
			if #bytes ~= 1 then return false end
			error_latch = false
			pending = { k = "rm", m = nib }

		elseif c == 0x50 then								-- RR：恰 2B，abc=0
			if #bytes ~= 2 or bytes[2] % 4 ~= 0 then return false end
			local addr = math.floor(bytes[2] / 4)
			if mode ~= "idle" then
				error_latch = true; pending = { k = "status" }
			else
				error_latch = false
				pending = { k = "rr", a = addr }
			end

		elseif c == 0x60 then								-- WR：恰 4B，地址字节低 2 位 0
			if #bytes ~= 4 or bytes[4] % 4 ~= 0 then return false end
			local addr = math.floor(bytes[4] / 4)
			local data = bytes[2] * 256 + bytes[3]
			error_latch = false
			if mode ~= "idle" or addr >= 0x20 then
				-- 突发中 RR/WR 不允许、Melexis 区写保护：命令照 ACK，ERROR 置位
				error_latch = true
			else
				regs[addr] = data % 65536
			end
			pending = { k = "status" }

		elseif c == 0x80 then								-- EX
			if #bytes ~= 1 then return false end
			mode, sel, drdy, remaining = "idle", 0, false, nil
			error_latch = false
			pending = { k = "status" }

		elseif c == 0xD0 then								-- HR：NVRAM -> 易失
			if #bytes ~= 1 then return false end
			reseed()
			mode, sel, drdy, remaining = "idle", 0, false, nil
			error_latch = false
			pending = { k = "status" }

		elseif c == 0xE0 then								-- HS（后等 15ms，模型无 NVRAM 写）
			if #bytes ~= 1 then return false end
			error_latch = false
			pending = { k = "status" }

		elseif c == 0xF0 then							-- RT：暖复位
			if #bytes ~= 1 then return false end
			reseed()
			mode, sel, drdy, remaining = "idle", 0, false, nil
			-- 注意：prearmed【不清】——它是测试桩注入态而非芯片状态。守护
			-- build 前 virt_tick(0) 预武装，而 build 第一条命令正是 RT，
			-- 若随复位取消，build 内试测转换门在真实 milli_sleep 下永不完成。
			error_latch = false
			rs_latch = true
			pending = { k = "status" }

		else
			-- 非法命令：I2C 恒 ACK，ERROR 置位（datasheet §15.1）
			error_latch = true
			pending = { k = "status" }
			unknown[#unknown + 1] = c
		end

		cmds[#cmds + 1] = c
		return true
	end

	function d.read(n)
		if d.nack or d.nack_read then return nil end
		n = math.floor(tonumber(n) or 0)
		if n < 1 then return nil end
		if read_pred and read_pred(pending, n) then return nil end
		if pending == nil then return nil end

		if pending.k == "status" then
			if n ~= 1 then return nil end
			local s = build_status(0)
			rs_latch = false					-- RS 首读后自清
			pending = nil
			return { s }
		end

		if pending.k == "rr" then
			if n ~= 3 then return nil end
			local v = regs[pending.a] or 0
			pending = nil
			return { build_status(0), math.floor(v / 256) % 256, v % 256 }
		end

		if pending.k == "rm" then
			local mask = pending.m
			local knt = (bitc.band(mask, 0x1) ~= 0 and 1 or 0)
				+ (bitc.band(mask, 0x2) ~= 0 and 1 or 0)
				+ (bitc.band(mask, 0x4) ~= 0 and 1 or 0)
				+ (bitc.band(mask, 0x8) ~= 0 and 1 or 0)
			if n ~= 1 + 2 * knt then return nil end
			-- DRDY 低读数据：ERROR 置位、无数据（datasheet §15.2）
			if mode == "single" and not drdy then
				error_latch = true; return nil
			end
			if mode ~= "burst" and not drdy then
				error_latch = true; return nil
			end
			if mode == "burst" and remaining ~= nil and remaining > 0 then
				error_latch = true; return nil
			end
			local out = { build_status(knt - 1) }
			local function put(v)
				v = v % 65536
				out[#out + 1] = math.floor(v / 256) % 256
				out[#out + 1] = v % 256
			end
			if bitc.band(mask, 0x1) ~= 0 then put(sample_t) end
			if bitc.band(mask, 0x2) ~= 0 then put(sample_x) end
			if bitc.band(mask, 0x4) ~= 0 then put(sample_y) end
			if bitc.band(mask, 0x8) ~= 0 then put(sample_z) end
			if mode == "single" then
				-- 结果读走：回 IDLE、SM_MODE 清
				mode, drdy = "idle", false
			end
			pending = nil
			return out
		end

		return nil
	end

	-- ---------------- 测试 / 守护 API（冒号定义） ----------------

	function d:advance(ms)
		ms = tonumber(ms) or 0
		if rs_latch and ms > 0 then rs_latch = false end
		if never_ready then return self end
		if remaining ~= nil then
			remaining = remaining - ms
			if remaining <= 0 then
				remaining = nil
				if mode == "single" then drdy = true end
			end
			if mode == "burst" and remaining == nil then drdy = true end
		end
		return self
	end

	-- 注入原始码：磁场 s16 计数（µT 换算归驱动），温度 u16 无符号
	function d:set_raw(t, x, y, z)
		sample_t = clamp16(t)
		sample_x = clamp_s16(x)
		sample_y = clamp_s16(y)
		sample_z = clamp_s16(z)
		return self
	end

	-- 物理量注入（守护/E2E 造样本；按当前 GAIN_SEL 反算成码）
	function d:set_sample(t_c, x_uT, y_uT, z_uT)
		local sxy, sz = sensitivities()
		sample_t = clamp16(rround(M.T_RAW_25C + 45.2 * ((tonumber(t_c) or 25) - 25)))
		sample_x = clamp_s16(rround((tonumber(x_uT) or 0) / sxy))
		sample_y = clamp_s16(rround((tonumber(y_uT) or 0) / sxy))
		sample_z = clamp_s16(rround((tonumber(z_uT) or 0) / sz))
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred) read_pred = pred; return self end
	function d:set_error(on) error_latch = on ~= false; return self end
	function d:set_never_ready(on) never_ready = on ~= false; return self end

	function d:clear_faults()
		d.nack, d.nack_read, d.nack_write = false, false, false
		write_pred, read_pred = nil, nil
		error_latch, never_ready = false, false
		return self
	end

	function d:reset_logs()
		cmds, unknown, frames = {}, {}, {}
		return self
	end

	-- 回上电态（故障钩子保留，防负路径假解除）
	function d:reset()
		reseed()
		mode, sel, drdy = "idle", 0, false
		remaining, prearmed, error_latch, rs_latch, pending = nil, false, false, false, nil
		sample_t, sample_x, sample_y, sample_z = M.T_RAW_25C, 0, 0, 0
		cmds, unknown, frames = {}, {}, {}
		return self
	end

	-- 守护逐拍造数：确定性地磁场量级正弦（X±30µT 含负值、Y 25µT 移相、
	-- Z 30~50µT、T 20~30°C，32 拍周期）+ 预武装（下一次 SM 零转换门）。
	function d:virt_tick(n)
		n = tonumber(n) or 0
		local ph = 2 * math.pi * (n % 32) / 32
		self:set_sample(25 + 5 * math.sin(ph + math.pi / 4),
			30 * math.sin(ph),
			25 * math.sin(ph + math.pi / 3),
			40 + 10 * math.sin(ph + math.pi / 2))
		prearmed = true
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:reg(a) return regs[a] or 0 end
	function d:gain_sel() return bitc.band(bitc.rshift(regs[0x00] or 0, 4), 0x7) end
	function d:sensitivities() local a, b = sensitivities(); return a, b end
	function d:mode() return mode end
	function d:select_mask() return sel end
	function d:is_ready() return drdy end
	function d:remaining_ms() return remaining end
	function d:pending() return pending and pending.k or nil end
	function d:current_status() return build_status(0) end
	function d:cmd_count() return #cmds end
	function d:commands() return cmds end
	function d:unknown_commands() return unknown end
	function d:write_log() return frames end
	function d:raw() return sample_t, sample_x, sample_y, sample_z end

	return d
end

return M
