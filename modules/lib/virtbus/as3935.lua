-- modules/lib/virtbus/as3935.lua —— ams AS3935 Franklin 闪电传感器虚拟器件模型
-- bot4cj 虚拟总线器件库（无 BUS 字段 = I2C，attach 动态委托 dev.write/dev.read）
-- 对应真实驱动：modules/AS3935.lua（同协议独立实现，防互证假绿；改协议两处同步）
--
-- 定位：【8 位字节指针 + 事件型中断锁存（INT 读后清）+ 两条直接命令
-- （0x3C 软复位 / 0x3D RCO 校准）+ 21 位能量三字节小端】样板（ams
-- AS3935，I2C 地址族 0x01~0x03 由 A0/A1 决定，GY/SparkFun 模块常见
-- 0x03；纯闭包、纯算术无 bitcompat）。
-- 寄存器全按字节寻址（i2c_read_reg = 写 1B 指针 + 读 1B，
-- i2c_write_reg = 写 2B {ptr,val}），读指针随字节自增、0x3F 后回绕
-- 0x00，与 amg8833/bmm150 同族但六处个性：
--
--   1) **事件型器件**（区别于连续采样型）：平时没有"数据"，芯片检测到
--      噪声过高/干扰源/闪电后把事件码锁存进 0x03 低 4 位，call 读一次
--      0x03 取走事件，**该读动作立即清事件码**（datasheet：reading
--      register 0x03 clears the interrupt；模型在读出字节的同一拍把
--      低 4 位清 0，bit5 MASK_DIST 配置位保留）。事件码 0x01=NOISE、
--      0x04=DISTURBER、0x08=LIGHTNING（可叠加/带杂散位，驱动按位判、
--      闪电优先），0x00=无事件。
--   2) 闪电的距离/能量是**独立测量区**（0x07 S_DIST、0x04/05/06 能量
--      LSB/MSB/MMSB），随 strike 一起装载、**读 INT 不清它们**（保留
--      最近一次闪电值直到下一次闪电）；写 ACK 丢弃，只能由注入 API
--      改。S_DIST 6 位：1=头顶/极近、2~62=公里数、0x3F=超量程、
--      0=未知（POR 即 0x3F）。能量 21 位：MMSB 仅低 5 位有效，
--      (mmsb&0x1F)*65536 + msb*256 + lsb，换算/判定全在驱动侧，
--      模型只存字节，避免模型-驱动互证假绿。
--   3) 两条直接命令（写后自清不落盘，命令数据必须 0x96，其他值
--      ACK 忽略）：{0x3C,0x96}=PRESET_DEFAULT 软复位，全部易失寄存器
--      回 POR（reset_count++）；{0x3D,0x96}=CALIB_RCO，模型立即把
--      0x3A/0x3B 写成 DONE(bit7)=1、NOK(bit6) 按注入的失败标志
--      （set_calib_fail）决定——驱动不查 DONE 只查 NOK，失败标志是
--      喂 build 校准失败负路径的唯一口子。0x3A/0x3B 只读，写丢弃。
--   4) 0x03 是【动态合成 + RMW 安全】寄存器：低 4 位是事件锁存
--      （只读语义，主机写入无效、也不清事件），bit5=MASK_DIST 是
--      可写配置位（驱动 keep_mask=0xDF 读-改-写）。模型把主机写
--      0x03 合并为 regs[3]=(事件低 nibble)|(val&0x20)，RMW 不丢
--      事件也不污染保留位。MASK_DIST=1 时 disturber() 注入被硬件
--      屏蔽（返回 false、INT 不置位），噪声/闪电不受影响。
--   5) 0x00 bit0=PWD 电源关断：置 1 后芯片检测停摆（I2C 仍可访问，
--      寄存器照读照写），noise/disturber/strike 注入一律拒绝。
--      配置位域：0x00 [5:1]=AFE_GB（POR 0x24=室内字段 0x12，复位后
--      在位甄别依据；写掩码 0x3F）、0x01 [6:4]=NF_LEV/[3:0]=WDTH
--      （写掩码 0x7F）、0x02 [5:4]=MIN_NUM_LIG 码/[3:0]=SREJ
--      （写掩码 0x3F）、0x08 [7:6]=SRCO/TRCO 显示位/[3:0]=TUN_CAP
--      （写掩码 0xCF）。保留位写入丢弃、读回 0。
--   6) 无周期测量、无转换门、无 advance：事件不是连续量，virt_tick
--      按 8 拍确定性脚本【逐拍定义当前锁存事件】（none 拍会清事件，
--      守护 build 前 virt_tick(0) 预武装的就是 none），闪电拍的
--      距离 10~35km、能量 1e5~9e5 随拍号正弦变化，让守护多拍出现
--      全部四种 CODE。call 前 milli_sleep(2) 是驱动实睡（IRQ 锁存
--      稳定窗口），模型不建时间门，测试桩只累加 sleep_total。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数，
-- attach 动态委托 dev.write(bytes) 契约）。

local M = {}
M._VERSION = "0.1.0"

-- ==================== 寄存器地址 / 常量 ====================
local REG_AFE_GAIN   = 0x00
local REG_THRESHOLD  = 0x01
local REG_LIGHTNING  = 0x02
local REG_INT        = 0x03
local REG_EGY_LSB    = 0x04
local REG_EGY_MSB    = 0x05
local REG_EGY_MMSB   = 0x06
local REG_DISTANCE   = 0x07
local REG_FDISP      = 0x08
local REG_CALIB_TRCO = 0x3A
local REG_CALIB_SRCO = 0x3B
local REG_RESET      = 0x3C
local REG_CALIB_RCO  = 0x3D

local CMD_DATA       = 0x96
local EVT_NOISE      = 0x01
local EVT_DISTURBER  = 0x04
local EVT_LIGHTNING  = 0x08

-- POR 默认值（datasheet reset 缺省：室内增益、NF=2、WDTH=2、SREJ=2、
-- MIN_NUM_LIG=1 码 0、距离 0x3F 超量程）
local POR_AFE       = 0x24		-- PWD=0, AFE_GB[5:1]=0x12 室内
local POR_THRESHOLD = 0x22		-- NF_LEV=2<<4 | WDTH=2
local POR_LIGHTNING = 0x02		-- MIN_NUM=0(1 次) | SREJ=2
local POR_DISTANCE  = 0x3F		-- 复位后距离=超量程

-- virt_tick 8 拍事件脚本：0/1/3/5=none，2/6=闪电，4=噪声，7=干扰源
-- （n=0 必须是 none：守护 build 前用它预武装，build 的 0x03 RMW
-- 读不得读到事件，否则首拍 call 会吃到陈旧事件码）
local TICK_EVENTS = {
	[0] = "none", "none", "lightning", "none",
	"noise", "none", "lightning", "disturber",
}

local function clampi(v, lo, hi)
	v = math.floor(tonumber(v) or 0)
	if v < lo then return lo elseif v > hi then return hi else return v end
end

function M.new()
	local d = {}

	-- 芯片侧状态
	local regs = {}				-- 字节寄存器影子（稀疏，缺省读 0）
	local read_ptr = nil		-- 最近一笔指针写（read 回调据此起 burst）
	local afe_por = POR_AFE		-- 软复位后 0x00 的值（who() 可覆写喂负路径）
	local trco_nok = false		-- RCO 校准失败注入（影响下次 0x3D=0x96）
	local srco_nok = false
	local reset_count = 0		-- 软复位 {0x3C,0x96} 次数
	local rco_count = 0		-- RCO 校准 {0x3D,0x96} 次数
	local write_count = 0		-- ≥2B 写事务笔数
	local write_log = {}		-- ≥2B 写事务 {reg=首地址, bytes=...}

	-- 故障钩子（reset 保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	-- ---------------- POR / 软复位 ----------------

	local function seed_body()
		regs = {}
		regs[REG_AFE_GAIN] = afe_por
		regs[REG_THRESHOLD] = POR_THRESHOLD
		regs[REG_LIGHTNING] = POR_LIGHTNING
		regs[REG_INT] = 0x00
		regs[REG_EGY_LSB], regs[REG_EGY_MSB], regs[REG_EGY_MMSB] = 0, 0, 0
		regs[REG_DISTANCE] = POR_DISTANCE
		regs[REG_FDISP] = 0x00
		-- 0x3A/0x3B 校准状态 POR=0x00（DONE=0/NOK=0），命令寄存器不落盘
		regs[REG_CALIB_TRCO], regs[REG_CALIB_SRCO] = 0x00, 0x00
	end

	seed_body()

	-- ---------------- 总线回调（点定义，无 self） ----------------

	local function write_byte(a, v)
		v = v % 256
		if a == REG_RESET then
			if v == CMD_DATA then
				reset_count = reset_count + 1
				seed_body()			-- 软复位：易失寄存器全回 POR
			end						-- 其他值：ACK 忽略；命令寄存器不落盘
			return
		end
		if a == REG_CALIB_RCO then
			if v == CMD_DATA then
				rco_count = rco_count + 1
				-- 模型无 2ms 时间门：命令即完成（DONE bit7=1），NOK 按注入
				regs[REG_CALIB_TRCO] = 0x80 + (trco_nok and 0x40 or 0)
				regs[REG_CALIB_SRCO] = 0x80 + (srco_nok and 0x40 or 0)
			end
			return
		end
		if a == REG_AFE_GAIN then
			regs[a] = v % 0x40			-- bit[7:6] 保留
		elseif a == REG_THRESHOLD then
			regs[a] = v % 0x80			-- bit7 保留
		elseif a == REG_LIGHTNING then
			regs[a] = v % 0x40			-- bit[7:6] 保留
		elseif a == REG_INT then
			-- 仅 bit5 MASK_DIST 可写；低 4 位事件锁存与保留位不受写影响
			regs[a] = (regs[a] % 16) + (v % 0x40 >= 0x20 and 0x20 or 0)
		elseif a == REG_FDISP then
			regs[a] = v - math.floor(v / 0x10) % 4 * 0x10	-- 清 bit[5:4] 保留位 = v & 0xCF
		elseif a >= REG_EGY_LSB and a <= REG_DISTANCE then
			-- 能量/距离只读测量区：写 ACK 丢弃，只能由注入 API 改
			return
		elseif a == REG_CALIB_TRCO or a == REG_CALIB_SRCO then
			return						-- 校准状态只读
		end
		-- 其余保留地址（0x09~0x39/0x3E/0x3F）：ACK 丢弃
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
		if read_ptr > 0x3F then return nil end	-- 寄存器空间 0x00..0x3F
		local t = {}
		for i = 1, n do
			local a = read_ptr
			t[i] = regs[a] or 0
			if a == REG_INT then
				-- 读 0x03 清事件锁存（低 4 位），MASK_DIST 保留
				regs[REG_INT] = regs[REG_INT] - (regs[REG_INT] % 16)
			end
			read_ptr = (read_ptr + 1) % 0x40	-- burst 自增、0x3F 回绕
		end
		return t
	end

	-- ---------------- 测试 / 守护注入 API（冒号定义） ----------------

	local function pwd() return (regs[REG_AFE_GAIN] or 0) % 2 == 1 end
	local function masked() return (regs[REG_INT] or 0) >= 0x20 end

	-- 闪电事件：dist=S_DIST 码（1=头顶、2~62=km、0x3F 超量程、0 未知），
	-- energy=21 位相对能量（MMSB 仅低 5 位）。PWD 下拒绝。
	function d:strike(dist, energy)
		if pwd() then return false end
		dist = clampi(dist, 0, 63)
		energy = clampi(energy, 0, 0x1FFFFF)
		regs[REG_INT] = (regs[REG_INT] - regs[REG_INT] % 16) + EVT_LIGHTNING
		regs[REG_DISTANCE] = dist
		regs[REG_EGY_LSB] = energy % 256
		regs[REG_EGY_MSB] = math.floor(energy / 256) % 256
		regs[REG_EGY_MMSB] = math.floor(energy / 65536) % 0x20
		return true
	end

	-- 噪声过高事件（不受 MASK_DIST 影响）
	function d:noise()
		if pwd() then return false end
		regs[REG_INT] = (regs[REG_INT] - regs[REG_INT] % 16) + EVT_NOISE
		return true
	end

	-- 干扰源事件：MASK_DIST=1 时被硬件屏蔽（INT 不置位）
	function d:disturber()
		if pwd() or masked() then return false end
		regs[REG_INT] = (regs[REG_INT] - regs[REG_INT] % 16) + EVT_DISTURBER
		return true
	end

	-- 直接装事件码（喂杂散位/叠加位用例，如 0x0A），不动距离/能量
	function d:set_int_raw(code)
		regs[REG_INT] = (regs[REG_INT] - regs[REG_INT] % 16)
			+ (math.floor(tonumber(code) or 0) % 16)
		return self
	end

	function d:clear_event()
		regs[REG_INT] = regs[REG_INT] - regs[REG_INT] % 16
		return self
	end

	-- 校准失败注入："trco"/"srco"/true=两个都失败，false/nil=清除
	function d:set_calib_fail(which)
		if which == "trco" then
			trco_nok, srco_nok = true, false
		elseif which == "srco" then
			trco_nok, srco_nok = false, true
		elseif which == true then
			trco_nok, srco_nok = true, true
		else
			trco_nok, srco_nok = false, false
		end
		return self
	end

	-- 复位后身份覆写（喂 build "not AS3935?" 负路径；软复位保留，
	-- 完整 reset() 恢复 POR_AFE）
	function d:who(byte)
		if byte ~= nil then
			afe_por = math.floor(tonumber(byte) or 0) % 256
			regs[REG_AFE_GAIN] = afe_por
		end
		return self
	end

	-- 守护逐拍事件脚本：8 拍周期定义当前锁存事件（none 拍清事件）。
	-- 闪电距离 10~35km 按 16 拍正弦、能量 1e5~9e5 按 8 拍正弦。
	function d:virt_tick(n)
		n = math.floor(tonumber(n) or 0)
		local kind = TICK_EVENTS[n % 8]
		if kind == "lightning" then
			local dist = 10 + math.floor(25 * (0.5 + 0.5 * math.sin(2 * math.pi * n / 16)))
			local energy = 100000 + math.floor(800000 * (0.5 + 0.5 * math.sin(2 * math.pi * n / 8 + math.pi / 4)))
			d:strike(dist, energy)
		elseif kind == "noise" then
			d:noise()
		elseif kind == "disturber" then
			d:disturber()			-- 被 mask 时安静无事件（默认配置不 mask）
		else
			d:clear_event()
		end
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred) read_pred = pred; return self end

	function d:clear_faults()
		d.nack, d.nack_read, d.nack_write = false, false, false
		write_pred, read_pred = nil, nil
		trco_nok, srco_nok = false, false
		return self
	end

	function d:reset_logs()
		write_count, reset_count, rco_count = 0, 0, 0
		write_log = {}
		return self
	end

	-- 重新上电：全状态回 POR（身份/校准失败注入/日志清零）；故障钩子保留
	function d:reset()
		afe_por = POR_AFE
		trco_nok, srco_nok = false, false
		write_count, reset_count, rco_count = 0, 0, 0
		write_log = {}
		read_ptr = nil
		seed_body()
		return self
	end

	-- ---------------- 只读观察口 ----------------

	function d:reg(a) return regs[math.floor(tonumber(a) or 0) % 256] end
	function d:ptr() return read_ptr end
	function d:event() return regs[REG_INT] % 16 end
	function d:pwd() return pwd() end
	function d:mask_dist() return masked() end
	function d:afe_gb() return math.floor((regs[REG_AFE_GAIN] or 0) / 2) % 0x20 end
	function d:nf_lev() return math.floor((regs[REG_THRESHOLD] or 0) / 16) % 8 end
	function d:wdth() return (regs[REG_THRESHOLD] or 0) % 16 end
	function d:min_lig_code() return math.floor((regs[REG_LIGHTNING] or 0) / 16) % 4 end
	function d:srej() return (regs[REG_LIGHTNING] or 0) % 16 end
	function d:tun_cap_code() return (regs[REG_FDISP] or 0) % 16 end
	function d:distance() return regs[REG_DISTANCE] or 0 end
	function d:energy()
		return (regs[REG_EGY_MMSB] or 0) * 65536
			+ (regs[REG_EGY_MSB] or 0) * 256 + (regs[REG_EGY_LSB] or 0)
	end
	function d:trco_status() return regs[REG_CALIB_TRCO] or 0 end
	function d:srco_status() return regs[REG_CALIB_SRCO] or 0 end
	function d:reset_count() return reset_count end
	function d:rco_count() return rco_count end
	function d:write_count() return write_count end
	function d:writes() return write_log end

	return d
end

return M
