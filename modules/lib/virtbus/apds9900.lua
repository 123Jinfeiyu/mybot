-- apds9900.lua —— Broadcom/Avago APDS-9900 环境光（ALS）+近距（PROX）传感器虚拟器件模型
-- bot4cj 虚拟硬件库（modules/lib/virtbus/，配合 modules/lib/virtbus.lua 使用）
--
-- TAOS TSL2771 架构（APDS-9960 的前代，无手势引擎；寄存器布局与 9960 不同），
-- 固定 I2C 地址 0x39。命令字节寻址协议（datasheet AV02-2049EN + Linux 内核
-- apds990x.c 交叉验证），【不是朴素寄存器指针】：
--   每笔寄存器访问的首字节是命令字节：bit7 必为 1（COMMAND），bit6:5 是
--   TYPE 选择随后事务形态，bit4:0 是寄存器地址（地址空间 0x00~0x1F）：
--     TYPE=00（0x80|reg）重复字节：所有数据字节落在同一寄存器；
--     TYPE=01（0xA0|reg）自动增量：每字节后地址 +1（寄存器对小端流用）；
--     TYPE=10（0xC0|reg）重复字：奇数字节回同字低址、偶数字节走高址；
--     TYPE=11（0xE0|reg）自动增量（同 01，线性 +1）。
--   真实驱动只用 TYPE=00 单字节写/读与 TYPE=01 的 2 字节字对读。
--
-- 建模寄存器（驱动 build/call 全部触点；阈值/STATUS 不碰即不建模，NACK）：
--   0x00 ENABLE R/W  POR 0x00：bit0 PON/bit1 AEN/bit2 PEN/bit3 WEN/
--                    bit4 AIEN/bit5 PIEN，bit7:6 保留写时抹 0；
--                    驱动先写 0x00 断电、末尾写 0x07（PON|AEN|PEN）。
--   0x01 ATIME  R/W  POR 0xFF：ALS 积分周期数 = 256 - reg，每周期
--                    87/32=2.71875ms（缺省 50ms → 18 周期 → reg=238）。
--   0x02 PTIME  R/W  POR 0xFF：近距积分固定写 0xFF（1 周期 2.7ms）。
--   0x03 WTIME  R/W  POR 0xFF：等待时间（WEN=0 不用），固定写 0xFF。
--   0x0C PERS   R/W  POR 0x00：中断持续性，轮询不用，写 0。
--   0x0D CONFIG R/W  POR 0x00：WLONG 等位，驱动写 0。
--   0x0E PPCOUNT R/W POR 0x01：每周期 IR LED 脉冲数（1~255，缺省写 8）。
--   0x0F CONTROL R/W POR 0x00：[7:6]PDRIVE [5:4]PDIODE [3:2]PGAIN [1:0]AGAIN，
--                    8 位全用（缺省 0x20：100mA/IR 二极管/×1/×1）。
--   0x12 ID     ROM 只读：批次 ID，驱动接受 0x00/0x04/0x29（模型默认 0x29）；
--                    写 ACK 但丢弃（ROM 不可改）。
--   0x14/0x15 CDATA  【小端字对】CH0 全谱计数 u16（0x14=LSB），只读测量区。
--   0x16/0x17 IRDATA 【小端字对】CH1 红外计数 u16，只读测量区。
--   0x18/0x19 PDATA  【小端字对】近距计数 u16（有效量程约 1023），只读测量区；
--                    三字对写均 ACK 丢弃（测量值只能由注入 API 改）。
--   0x04~0x09 中断阈值（AILT/AIHT/PILT/PIHT）、0x0A/0x0B 保留、0x13 STATUS
--   及 0x1A~0x1F：驱动从不触碰，不建模——指针写/字写/读一律 NACK。
--
-- 【真实驱动 build/call 事务】（modules/APDS9900.lua，全程零 milli_sleep）：
--   build：set + {80,00} 断电写 + ID 指针读（{92} 指针写 + r1）+
--     7 笔配置写 {81,ATIME}{82,FF}{83,FF}{8D,00}{8C,00}{8E,pp}{8F,ctl}
--     + {80,07} 启动 = set + 10 w + 1 r（12 txlog）；
--   call：set + 三笔 TYPE=01 字对指针读 {B4}+r2、{B6}+r2、{B8}+r2
--     = set + 3 w + 3 r（7 txlog），连续转换无就绪握手；
--   destroy：set + {80,00} = 2 txlog。
--
-- 用法：
--   local amod = dofile("modules/lib/virtbus/apds9900.lua")
--   local dev = amod.new()
--   dev:set_counts(10000, 2500, 517)
--   vbus.attach("APDS9900@0x39", 0x39, dev)
--
--   dev:who(0x04)       -- 覆写批次 ID（0x00/0x04/0x29 均被驱动接受）
--
-- 【边界约定】模型只固化命令字节/TYPE 寻址、小端字对字节流、寄存器 POR
-- 与位域、ROM/测量区只读；lux 两线 IR 补偿物理换算在驱动，物理量注入
-- API 常量仅为造样本，测试另做独立换算交叉。

local apds9900 = {}
apds9900._VERSION = "0.1.0"

apds9900.REG = {
	ENABLE  = 0x00,
	ATIME   = 0x01,
	PTIME   = 0x02,
	WTIME   = 0x03,
	PERS    = 0x0C,
	CONFIG  = 0x0D,
	PPCOUNT = 0x0E,
	CONTROL = 0x0F,
	ID      = 0x12,
	CDATA   = 0x14,
	IRDATA  = 0x16,
	PDATA   = 0x18,
}

apds9900.ID_DEFAULT = 0x29		-- 批次 ID（驱动另接受 0x00/0x04）
apds9900.I2C_ADDR   = 0x39		-- 芯片固定地址（无地址引脚）

-- 可写配置寄存器（含 ENABLE）；测量区/ROM 不在其中
local RW = {
	[0x00] = true, [0x01] = true, [0x02] = true, [0x03] = true,
	[0x0C] = true, [0x0D] = true, [0x0E] = true, [0x0F] = true,
}

-- 只读区：ID ROM + 三个小端字对（偶=LSB、奇=MSB）
local READ_ONLY = {
	[0x12] = true,
	[0x14] = true, [0x15] = true,
	[0x16] = true, [0x17] = true,
	[0x18] = true, [0x19] = true,
}

-- 全部已建模地址（未列入者：阈值 0x04~0x09、0x0A/0x0B、STATUS 0x13、
-- 0x1A~0x1F——任何形式访问整笔 NACK）
local MODELED = {}
for a in pairs(RW) do MODELED[a] = true end
for a in pairs(READ_ONLY) do MODELED[a] = true end

-- 配置寄存器 POR（ID/测量区在 reseed 另行放置）
local POR = {
	[0x00] = 0x00,	-- ENABLE：上电断电态
	[0x01] = 0xFF,	-- ATIME：最短积分
	[0x02] = 0xFF,	-- PTIME
	[0x03] = 0xFF,	-- WTIME
	[0x0C] = 0x00,	-- PERS
	[0x0D] = 0x00,	-- CONFIG
	[0x0E] = 0x01,	-- PPCOUNT：复位 1 脉冲
	[0x0F] = 0x00,	-- CONTROL
}

local function clampi(v, lo, hi)
	if v < lo then return lo end
	if v > hi then return hi end
	return v
end

function apds9900.new(opts)
	opts = opts or {}

	local d = {}
	local regs = {}				-- 已建模地址 → 字节值（含 ID/数据字对）
	local ptr = 0				-- 最近命令字节选中的地址
	local ptype = 0			-- 最近命令字节 TYPE（0..3）
	local id_reg = opts.id or apds9900.ID_DEFAULT
	local write_pred, read_pred
	d.write_log = {}			-- 全部写事务（字节表副本），含指针写/失败写

	-- 故障注入三件（与 regmap/max17048 约定同名）
	d.nack = false
	d.nack_read = false
	d.nack_write = false

	local function reseed()
		for k in pairs(regs) do regs[k] = nil end
		for a, v in pairs(POR) do regs[a] = v end
		regs[0x12] = id_reg
		regs[0x14], regs[0x15] = 0, 0		-- CDATA
		regs[0x16], regs[0x17] = 0, 0		-- IRDATA
		regs[0x18], regs[0x19] = 0, 0		-- PDATA
	end
	reseed()

	-- 命令字节解码：bit7 COMMAND 必置；返回 地址(0..31), TYPE(0..3)；
	-- 命令位缺失=协议坏帧，整笔 NACK
	local function decode(cmd)
		cmd = math.floor(cmd) % 256
		if math.floor(cmd / 0x80) % 2 ~= 1 then return nil end
		return cmd % 32, math.floor(cmd / 32) % 4
	end

	-- TYPE 寻址：帧内第 k 个数据字节（0 基）落在哪个寄存器地址
	local function addr_at(kind, base, k)
		if kind == 0 then
			return base				-- 重复字节：恒同址
		elseif kind == 2 then
			return base + (k % 2)	-- 重复字：字内偶奇循环
		end
		return base + k				-- 01/11 自动增量：线性 +1
	end

	local function log_copy(bytes)
		local cp = {}
		for i = 1, #bytes do cp[i] = bytes[i] % 256 end
		d.write_log[#d.write_log + 1] = cp
	end

	-- ---------------- virtbus 点定义回调（无 self） ----------------

	-- 1 字节 {cmd} = 纯指针写（读事务前奏，只锁地址/TYPE，ACK）；
	-- 2 字节起 = 数据写：命令字节后跟 1..N 个数据字节，按 TYPE 落址。
	-- ENABLE 保留位 7:6 写入即抹 0；ROM/测量区只读，写 ACK 但丢弃；
	-- 任何数据字节落未建模地址，整笔 NACK（先验后写，不落半截）。
	d.write = function(bytes)
		if type(bytes) ~= "table" or #bytes == 0 then return false end
		log_copy(bytes)
		if d.nack or d.nack_write or (write_pred and write_pred(bytes)) then
			return false
		end
		local base, kind = decode(bytes[1])
		if base == nil then return false end
		ptr, ptype = base, kind
		if #bytes == 1 then return true end

		for k = 2, #bytes do
			local a = addr_at(kind, base, k - 2)
			if not MODELED[a] then return false end
		end
		for k = 2, #bytes do
			local a = addr_at(kind, base, k - 2)
			if RW[a] then
				local v = bytes[k] % 256
				if a == 0x00 then v = v % 0x40 end	-- ENABLE 保留位抹 0
				regs[a] = v
			end
			-- READ_ONLY：ACK 丢弃
		end
		return true
	end

	-- 读 n 字节：严格按最近指针写的 TYPE 取字节流（小端字对由驱动换序，
	-- 模型只保证偶址 LSB 在前）；途中任一地址未建模整笔 NACK。
	d.read = function(n)
		if d.nack or d.nack_read then return nil end
		n = math.floor(tonumber(n) or 1)
		if n < 1 then return nil end
		if read_pred and read_pred(ptr, ptype, n) then return nil end
		local out = {}
		for k = 0, n - 1 do
			local a = addr_at(ptype, ptr, k)
			if not MODELED[a] then return nil end
			out[k + 1] = regs[a] or 0
		end
		return out
	end

	-- ---------------- 原始码注入（协议层主输入，冒号 API） ----------------

	-- 三通道计数：CH0 全谱/CH1 红外/PROX 近距，均 u16（0~65535），
	-- 按 datasheet 小端字对摆放（偶址 LSB）
	local function set_pair(base, v)
		v = clampi(math.floor(tonumber(v) or 0), 0, 65535)
		regs[base] = v % 256
		regs[base + 1] = math.floor(v / 256) % 256
		return v
	end

	function d:set_counts(ch0, ch1, prox)
		local a = set_pair(0x14, ch0)
		local b = set_pair(0x16, ch1)
		local c = set_pair(0x18, prox)
		return a, b, c
	end
	function d:set_ch0(v) set_pair(0x14, v); return self end
	function d:set_ch1(v) set_pair(0x16, v); return self end
	function d:set_prox(v) set_pair(0x18, v); return self end

	-- 可写配置寄存器留原始码注入口（只允许 RW 地址）
	function d:set_reg(reg, val)
		reg = math.floor(tonumber(reg) or 0)
		if RW[reg] then
			local v = clampi(math.floor(tonumber(val) or 0), 0, 255)
			if reg == 0x00 then v = v % 0x40 end
			regs[reg] = v
		end
		return self
	end

	-- ---------------- 守护仿真节拍（sensor_daemon 每拍调用，确定性） ----------------

	-- 32 拍周期的典型室内光场景（仅造样本，与驱动 lux 管线独立）：
	--   CH0 10000±4000（6000~14000）、CH1 = CH0/4 同相（IR 占比恒定，
	--   两线补偿后 lux 恒正）、PROX 300±200（100~500，有效量程 1023 内）。
	--   n0=10000/2500/300、n8 峰 14000/3500/500、n24 谷 6000/1500/100。
	-- 缺省 build（18 周期 48.94ms/×1/df52/ga0.48）下 lux ≈ 0.51×
	--   (ch0-2.23ch1)：n0≈2257、峰≈3160、谷≈1354，浮点带小数。
	-- 连续转换无武装/无转换门，直接装测量区（守护 build 前还会
	-- virt_tick(0) 预武装一次，确定性不变）。
	function d:virt_tick(n)
		n = tonumber(n) or 0
		local s = math.sin(n * (2 * math.pi / 32))
		local ch0 = math.floor(10000 + 4000 * s + 0.5)
		local ch1 = math.floor(ch0 / 4)
		local prox = math.floor(300 + 200 * s + 0.5)
		self:set_counts(ch0, ch1, prox)
		return self
	end

	-- ---------------- 身份/留痕/故障钩子 ----------------

	-- 覆写批次 ID（nil=不改；驱动接受 0x00/0x04/0x29，其余 build 拒收）
	function d:who(id)
		if id ~= nil then
			id_reg = math.floor(id) % 256
			regs[0x12] = id_reg
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

	-- 回上电态（保留故障钩子与 who() 身份，复位不得解除钩子）
	function d:reset()
		reseed()
		ptr, ptype = 0, 0
		self.write_log = {}
		return self
	end

	-- ---------------- 观察口 ----------------

	function d:reg(a) return regs[a] or 0 end
	function d:id() return regs[0x12] end
	function d:enable() return regs[0x00] end
	function d:atime() return regs[0x01] end
	function d:ptime() return regs[0x02] end
	function d:wtime() return regs[0x03] end
	function d:pers() return regs[0x0C] end
	function d:config() return regs[0x0D] end
	function d:ppcount() return regs[0x0E] end
	function d:control() return regs[0x0F] end
	function d:pointer() return ptr end
	function d:ptype() return ptype end

	local function pair_of(base)
		return (regs[base] or 0) + (regs[base + 1] or 0) * 256
	end
	function d:ch0() return pair_of(0x14) end
	function d:ch1() return pair_of(0x16) end
	function d:prox() return pair_of(0x18) end

	function d:modeled(a) return MODELED[a] == true end

	return d
end

return apds9900
