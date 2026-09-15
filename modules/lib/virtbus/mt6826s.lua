-- virtbus/mt6826s.lua —— MT6826S 15 位磁编码器的虚拟 SPI 器件模型
-- bot4cj 仓颉语言机器人框架
--
-- 对应真实驱动 modules/MT6826S.lua（SPI Mode 3，CPOL=1/CPHA=1，bits=8）。
-- 模型只存协议事实层：24 位帧结构、角度四寄存器布局、固定位、CRC-8；
-- 角度换算 raw*360/32768 只存在于真实驱动，本模型给 15 位原始码与度数逆注入。
--
-- 挂载（SPI 路径即片选身份）：
--   local mm = dofile("modules/lib/virtbus/mt6826s.lua")
--   local dev = mm.new()
--   vbus.attach_spi("MT6826S", "/dev/spidev0.0", dev)
--
-- 24 位帧 = 命令 4 位 + 地址 12 位 + 数据 8 位（手册第 11 章）：
--   读 0011(0x3) / 写 0110(0x6) / 烧 EEPROM 1100(0xC) / 连续角度读 1010(0xA)
--   MOSI：{cmd*16 + addr_hi, addr_lo, dummy}；MISO 命令+地址段 Hi-Z，
--   数据在第 3 字节。驱动只用读命令（不写寄存器/不烧 EEPROM），模型只把
--   读路径建完整；其余命令解析留痕、MISO 保持 0（协议事实未用到即不声称）。
--
-- 角度寄存器（只读，CSN 下降沿锁存四寄存器快照，逐寄存器读跨 CSN 一致）：
--   0x003 = ANGLE[14:7]
--   0x004 = ANGLE[6:0]<<1，bit0 恒 0
--   0x005 = 高 5 位恒 0 + STATUS[2:0]（bit0 超速/bit1 磁场弱/bit2 欠压，0 正常）
--   0x006 = CRC-8（poly 0x07、init 0x00、MSB first，覆盖 0x003~0x005）
-- 驱动四帧为一组读同一快照；模型里 angle/status 只由注入 API/virt_tick 改变，
-- 组内四帧天然取自同一 upvalue，锁存一致性自动成立。
local bitc = nil
-- bitcompat 在测试进程与守护 lua_State 都可经 modules 路径拿到；失败则退化为
-- 纯算术（CRC 只用到 bxor/band/lshift，Lua 5.4 环境缺 bitc 时走 fallback）。
local ok_bc, mpath = pcall(dofile, "modules/lib/mpath.lua")
if ok_bc then
	local ok2, mod = pcall(dofile, mpath.resolve("bitcompat.lua"))
	if ok2 then bitc = mod end
end
local band, bxor, lshift
if bitc then
	band, bxor, lshift = bitc.band, bitc.bxor, bitc.lshift
else
	-- 理论兜底（本仓库测试/守护均能加载 bitcompat）：必须用 Lua 5.1 语法，
	-- 不能写 &/^/<<（LuaJIT 解析期即报错，即使分支不执行）。
	local function b_and(a, b)
		local r, p = 0, 1
		while a > 0 and b > 0 do
			if a % 2 == 1 and b % 2 == 1 then r = r + p end
			a = math.floor(a / 2); b = math.floor(b / 2); p = p * 2
		end
		return r
	end
	local function b_xor(a, b)
		local r, p = 0, 1
		while a > 0 or b > 0 do
			local x, y = a % 2, b % 2
			if x ~= y then r = r + p end
			a = math.floor(a / 2); b = math.floor(b / 2); p = p * 2
		end
		return r
	end
	band = b_and
	bxor = b_xor
	lshift = function(a, n) return a * 2 ^ n end
end

local M = {}
M.BUS = "spi"		-- 供 sensor_daemon proto=virt 区分 I2C/SPI 挂载路径

-- ==================== 命令 / 寄存器（与驱动一致）====================
local CMD_READ = 0x3			-- 0011 读寄存器
local REG_ANGLE_H = 0x003		-- ANGLE[14:7]
local REG_ANGLE_L = 0x004		-- ANGLE[6:0]<<1（bit0 恒 0）
local REG_STATUS  = 0x005		-- [7:3]恒 0，[2:0]STATUS
local REG_CRC     = 0x006		-- CRC（覆盖 0x003~0x005）

local ANGLE_MAX = 32768			-- 15 位满量程（360°），有效码 0~32767

-- CRC-8：多项式 0x07，初值 0x00，MSB 先行；公开供测试与驱动三方交叉
local function crc8(bytes)
	local crc = 0x00
	for _, b in ipairs(bytes) do
		crc = bxor(crc, b)
		for _ = 1, 8 do
			if band(crc, 0x80) ~= 0 then
				crc = bxor(band(lshift(crc, 1), 0xFF), 0x07)
			else
				crc = band(lshift(crc, 1), 0xFF)
			end
		end
	end
	return crc
end

function M.new()
	local self = {}

	-- ---- 器件状态 ----
	local angle15 = 0			-- 当前角度快照（15 位原始码 0~32767）
	local status_bits = 0		-- STATUS[2:0]（0=正常）
	local read_count = 0		-- 累计读帧计数（4 帧一组）
	local frames = {}			-- 帧流水 {cmd=, addr=}

	-- ---- 故障注入 ----
	local crc_always_bad = false	-- 持续坏 CRC（CRC_OK=0，数值照常）
	local bad_arm_group = -1		-- 坏 CRC 组区间下界（武装时所处组号）
	local bad_end_group = -1		-- 坏 CRC 组区间上界（不含）
	local bad_fixed_l = false		-- 0x004 bit0 固定位违例
	local bad_fixed_st = false		-- 0x005 高 5 位固定位违例
	local fail_persistent = false	-- 持续传输失败（xfer 返 nil）
	local fail_pred = nil			-- function(tx) → true 则本帧失败

	-- ===================== 注入 API =====================

	-- 注入 15 位原始角码（钳到 0~32767；码空间无负值，超界钳制而非回绕）
	function self:set_angle_raw(v)
		v = math.floor(tonumber(v) or 0)
		if v < 0 then v = 0 elseif v > ANGLE_MAX - 1 then v = ANGLE_MAX - 1 end
		angle15 = v
		return self
	end

	-- 便捷 API：按物理角度反推原始码（先归一化到 [0,360)，再四舍五入）。
	-- 仅做注入逆变换；正算 raw*360/32768 归驱动，模型不含该公式。
	function self:set_angle_deg(deg)
		deg = (tonumber(deg) or 0) % 360
		angle15 = math.floor(deg * ANGLE_MAX / 360 + 0.5) % ANGLE_MAX
		return self
	end

	function self:angle_raw() return angle15 end

	-- STATUS[2:0]：bit0 超速 / bit1 磁场弱 / bit2 欠压，钳 0~7
	function self:set_status(s)
		s = math.floor(tonumber(s) or 0)
		if s < 0 then s = 0 elseif s > 7 then s = 7 end
		status_bits = s
		return self
	end
	function self:status() return status_bits end

	-- ---- CRC / 固定位故障 ----
	function self:corrupt_crc(on) crc_always_bad = (on ~= false); return self end
	-- 从「下一帧所在组」起连续 n 个读组（四帧一组）CRC 坏（固定半开区间
	-- [arm, arm+n)，不随后续帧消减）；build 已耗一组后武装，组号按武装瞬间
	-- read_count 锚定——call 注 1 组即「首组坏、重试组好」
	function self:corrupt_next_groups(n)
		bad_arm_group = math.floor(read_count / 4)
		bad_end_group = bad_arm_group + math.floor(tonumber(n) or 0)
		return self
	end
	function self:bad_fixed_l(on) bad_fixed_l = (on ~= false); return self end
	function self:bad_fixed_st(on) bad_fixed_st = (on ~= false); return self end

	-- ---- 传输层故障 ----
	function self:fail_transfer(on) fail_persistent = (on ~= false); return self end
	function self:fail_when(pred) fail_pred = pred; return self end
	function self:clear_faults()
		crc_always_bad = false
		bad_arm_group = -1
		bad_end_group = -1
		bad_fixed_l = false
		bad_fixed_st = false
		fail_persistent = false
		fail_pred = nil
		return self
	end

	-- ===================== 留痕 / 复位 API =====================

	function self:frames() return frames end
	function self:read_count() return read_count end
	function self:crc8(bytes) return crc8(bytes) end
	function self:reset_logs()
		frames = {}
		read_count = 0
		return self
	end

	-- 器件状态复位（回零位/正常状态/清日志）；不清故障注入钩子——硬件复位
	-- 语义上修不好总线故障，若 reset 清钩子，负路径 section 重开时会假绿
	-- （同 ads1256 模型约定）。
	function self:reset()
		angle15 = 0
		status_bits = 0
		frames = {}
		read_count = 0
		return self
	end

	-- ===================== 全双工帧核心 =====================

	-- 组装某地址读帧的末字节（含固定位/CRC 注入）。h/l/st 同取自当前快照
	-- upvalue：一次 read_snapshot 的四帧之间无注入介入即严格一致（CSN 锁存）。
	-- corrupt 为本组 CRC 是否翻转（由 xfer 按组号/配额判定后传入）。
	local function read_byte(addr, corrupt)
		local h = math.floor(angle15 / 128) % 256
		local l = (angle15 % 128) * 2
		local st = status_bits % 8
		if bad_fixed_l then l = l + 1 end			-- bit0 翻 1（l 最大 254，+1 不溢出）
		if bad_fixed_st then st = st + 8 end		-- [7:3] 出现非零位
		if addr == REG_ANGLE_H then return h end
		if addr == REG_ANGLE_L then return l end
		if addr == REG_STATUS then return st end
		if addr == REG_CRC then
			local c = crc8({ h, l, st })
			if corrupt then c = bxor(c, 0xA5) end
			return c
		end
		return 0
	end

	-- xfer(tx)：点定义、无 self 参数（virtbus attach 动态委托契约）。
	-- 入参 1 基 MOSI 表（驱动恒 3 字节），返回等长 MISO 表；nil/false = 帧失败。
	local function xfer(tx)
		if fail_persistent then return nil end
		if type(fail_pred) == "function" and fail_pred(tx) then return nil end

		local n = #tx
		local rx = {}
		for i = 1, n do rx[i] = 0 end
		if n ~= 3 then
			-- 24 位定长协议之外的帧形：MISO 保持 Hi-Z 读回 0，不声称行为
			return rx
		end

		local cmd = math.floor(tx[1] / 16) % 16
		local addr = (tx[1] % 16) * 256 + tx[2]
		frames[#frames + 1] = { cmd = cmd, addr = addr }

		if cmd == CMD_READ then
			-- 组计数在取数之前：0x003/4/5/6 四帧共享同一组号；坏 CRC 配额是
			-- 武装时锚定的固定半开区间，build 耗掉的组不会让 call 边界错位
			local group = math.floor(read_count / 4)
			local quota_bad = group >= bad_arm_group and group < bad_end_group
			rx[3] = read_byte(addr, crc_always_bad or quota_bad)
			read_count = read_count + 1
		end
		-- 写 0110 / 烧 EEPROM 1100 / 连续读 1010：驱动不用，解析留痕即可，
		-- MISO 命令+地址段本就 Hi-Z、数据段未定义，保持 0。
		return rx
	end
	self.xfer = xfer

	-- ===================== 守护节拍（确定性合成波形）====================
	-- 32 拍一周的线性角度爬坡：第 m 拍角度 m*360/32（11.25° 步进），
	-- 0~359.99° 全覆盖、确定无随机；守护 build 前自动 virt_tick(0) 预武装
	-- （零位，build 固定位自检照过）。无转换门/无睡眠，装码即可读。
	function self:virt_tick(n)
		n = tonumber(n) or 0
		local m = n % 32
		self:set_angle_deg(m * 360 / 32)
		return self
	end

	return self
end

return M
