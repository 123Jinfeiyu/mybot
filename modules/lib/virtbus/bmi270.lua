-- modules/lib/virtbus/bmi270.lua —— Bosch BMI270 六轴 IMU 虚拟器件模型
-- bot4cj 虚拟总线器件库（M.BUS="i2c"，attach 动态委托 dev.write/dev.read）
-- 对应真实驱动：modules/BMI270.lua（同协议独立实现，防互证假绿；改协议两处同步）
--
-- 定位：【8 位字节指针 + 配置文件 blob 上传状态机 + 小端六轴连续测量】
-- 样板（Bosch BMI270，I2C 0x68/0x69，Chip ID 0x24；0x26 为 BMI260 变体）。
-- 寄存器全按字节寻址（i2c_read_reg = 写 1B 指针 + 读 1B，
-- i2c_write_reg = 写 2B {ptr,val}），读指针随字节自增、0xFF 后算术
-- 回绕 0x00，与 emc2101/bmm150/as3935 同族。BMI270 带来两条新范式
-- （协议事实照 BMI270 datasheet BST-BMI270-DS000-08 + Bosch
-- bmi2.c v2.86.1 upload_file 逐条钉死，旧手写桩三处互证假绿见文件末
-- 「迁移订正」）：
--
--   1) **配置文件 blob 上传状态机（本模型核心新范式）**：陀螺仪与特性
--      引擎必须先上传 blob 才工作。主机经 INIT_CTRL(0x59) 清 0 清缓冲，
--      逐块（CHUNK=8）先写 12 位内部【字地址】到 INIT_ADDR_0(0x5B，
--      低 4 位 base[3:0])/INIT_ADDR_1(0x5C，高 8 位 base[11:4])，再把
--      8 字节突发写入 INIT_DATA(0x5E)；全部块写完后 INIT_CTRL=1 启动
--      片内加载——150ms 门内 INTERNAL_STATUS(0x21) 读 0x00(not_init)，
--      门到读 0x01(init_ok)；长度不符（非 328/8192）启动即 0x02
--      (init_err)。状态寄存器是【状态码】不是位掩码（低 4 位 0..7：
--      0 not_init/1 init_ok/2 init_err/3 drv_err/4 sns_stop/5 nvm_error/
--      6 start_up_error/7 compat_error），驱动只判 ==1。
--   2) **INIT_DATA(0x5E) 是地址陷阱（trap）寄存器**：对 0x5E 的突发
--      写寄存器地址【不自增】——一帧内全部数据字节都落 0x5E 窗口（进
--      blob 缓冲，不进寄存器影子），区别于普通寄存器的 (base+i) 自增
--      落盘。字地址按 datasheet「burst 后按 length/2 推进」；驱动每块
--      前必重写 0x5B，不依赖该自增。
--   3) **测量数据小端、连续常驻**：ACC 0x0C..0x11 / GYR 0x12..0x17
--      各 6 字节，每轴 LSB 在前（X_LSB,X_MSB,Y…,Z…），与 MPU6050 的
--      大端相反。无转换门/无 advance：上电配置后六轴是连续测量值，
--      常驻数据寄存器，virt_tick 直接装码预武装，call 零睡眠；blob
--      加载门是唯一需要 advance 的门。
--
-- 其余寄存器事实：ACC_CONF(0x40) POR=0xA8（[3:0]=ODR 0x06=25Hz…
-- 0x0C=1600Hz，[6:4]=bwp，bit7=filter_perf=1，驱动 RMW 只改低 4 位）；
-- ACC_RANGE(0x41) POR=0x02（0=±2g/1=±4g/2=±8g/3=±16g，LSB/g
-- 16384/8192/4096/2048）；GYR_CONF(0x42) POR=0xA9；GYR_RANGE(0x43)
-- POR=0x00（0=±2000…4=±125dps 代码与量程逆序，LSB/dps
-- 16.384/32.768/65.536/131.072/262.144，bit3=ois_range）；
-- PWR_CONF(0x7C) POR=0x03（低 3 位，上传前驱动清 0 关 APS）；
-- PWR_CTRL(0x7D) POR=0x00（bit0 aux/bit1 gyr_en/bit2 acc_en/bit3
-- temp_en）；CMD(0x7E) 只写读恒 0（0xB6 softreset/0xB0 fifo_flush/
-- 0xA0 nvm_prog 等）。软复位清 blob 缓冲与加载态，但 who 覆写、注入
-- 码、预武装、故障钩子保留（同 bme688 语义：注入代表测试环境，守护
-- build 的软复位不能解除 tick(0) 预武装）；仅上电 reset() 回出厂。
--
-- 【边界约定】模型只存协议事实与原始码：blob 内容（modules/lib/
-- bmi270_config.lua 的 328B fifo / 8192B full 官方 hex）与 g/dps 换算
-- 都不转录进模型——blob 由驱动上传、模型只逐字节接收并校验长度，
-- 物理量注入仅按当前量程档反算原始码（换算常数在测试/驱动处独立
-- 交叉）。virt_tick 32 拍正弦为确定性离线码钉（禁 math.random）。
--
-- 公开 API 一律冒号定义；write/read 总线回调用点定义（无 self 参数）。
-- LuaJIT 兼容：无 5.4 位运算符、无 //，位场一律 floor/% 算术。

local M = {}
M._VERSION = "0.1.0"
M.BUS = "i2c"

-- ==================== 寄存器地址 / 协议常量 ====================
local R_CHIP_ID    = 0x00		-- ROM CHIP_ID=0x24
local R_ACC_X_LSB  = 0x0C		-- 加速度 6 字节小端 0x0C..0x11
local R_GYR_X_LSB  = 0x12		-- 陀螺仪 6 字节小端 0x12..0x17
local R_INT_STATUS = 0x21		-- 内部状态码（非位掩码）：1=init_ok
local R_ACC_CONF   = 0x40		-- [3:0]=ODR [6:4]=bwp bit7=perf
local R_ACC_RANGE  = 0x41		-- [1:0] 量程码
local R_GYR_CONF   = 0x42
local R_GYR_RANGE  = 0x43		-- [2:0] 量程码（与 dps 逆序）bit3=ois
local R_INIT_CTRL  = 0x59		-- 0=准备下载清缓冲 1=启动加载
local R_INIT_ADDR0 = 0x5B		-- 字地址低 4 位
local R_INIT_ADDR1 = 0x5C		-- 字地址高 8 位
local R_INIT_DATA  = 0x5E		-- 配置数据窗口（地址陷阱）
local R_PWR_CONF   = 0x7C		-- 低 3 位
local R_PWR_CTRL   = 0x7D		-- 低 4 位
local R_CMD        = 0x7E		-- 只写命令

local CHIP_ID        = 0x24
local CMD_SOFT_RESET = 0xB6
local LOAD_GATE_MS   = 150		-- INIT_CTRL=1 到 init_ok 的加载门
local BLOB_FIFO_LEN  = 328		-- maximum_fifo 变体 41 块
local BLOB_FULL_LEN  = 8192	-- context 变体 1024 块

-- POR 默认值（datasheet register overview RESET 列）
local POR_ACC_CONF  = 0xA8
local POR_ACC_RANGE = 0x02
local POR_GYR_CONF  = 0xA9
local POR_GYR_RANGE = 0x00
local POR_PWR_CONF  = 0x03
local POR_PWR_CTRL  = 0x00

-- 量程码 → LSB/物理量（物理量注入反算用；驱动头注释常数）
local ACC_LSB_PER_G = { 16384, 8192, 4096, 2048 }		-- ±2/4/8/16g
local GYR_LSB_PER_DPS = { 16.384, 32.768, 65.536, 131.072, 262.144 } -- ±2000..±125

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
	local regs = {}				-- 稀疏字节寄存器影子（未定义地址读 0）
	local read_ptr = nil		-- 最近一笔指针写（read 回调据此起 burst）
	local chip_id = CHIP_ID		-- ROM 身份（who 覆写跨软复位保留）

	-- blob 上传状态机
	local uploaded = {}			-- 配置缓冲（按字节序号 1 基）
	local upload_bytes = 0		-- INIT_CTRL=0 以来写入窗口的字节数
	local word_addr = 0			-- 当前内部字地址（12 位，0..4095）
	local load_phase = "idle"	-- idle | loading | ok | error
	local gate_left = 0			-- 加载门剩余 ms
	local prearmed = false		-- virt_tick：下次 INIT_CTRL=1 立即 init_ok
	local never_ready = false	-- 加载门永不到点（轮询超时负路径）
	local stuck_code = nil		-- 非 nil 时 0x21 恒返该状态码

	-- 六轴测量码（s16，仅注入 API 可改；软复位保留，reset 清零）
	local ax, ay, az = 0, 0, 0
	local gx, gy, gz = 0, 0, 0

	local reset_count = 0		-- 软复位次数
	local write_count = 0		-- ≥2B 写事务笔数
	local read_count = 0		-- 读事务笔数
	local frames = {}			-- ≥2B 写事务副本

	-- 故障钩子（reset 保留，clear_faults 解除）
	d.nack, d.nack_read, d.nack_write = false, false, false
	local write_pred, read_pred

	-- ---------------- POR / 软复位 ----------------

	local function seed_body()
		regs = {}
		regs[R_ACC_CONF] = POR_ACC_CONF
		regs[R_ACC_RANGE] = POR_ACC_RANGE
		regs[R_GYR_CONF] = POR_GYR_CONF
		regs[R_GYR_RANGE] = POR_GYR_RANGE
		regs[R_PWR_CONF] = POR_PWR_CONF
		regs[R_PWR_CTRL] = POR_PWR_CTRL
		regs[R_INIT_CTRL] = 0x00
		regs[R_INIT_ADDR0] = 0x00
		regs[R_INIT_ADDR1] = 0x00
		-- blob 上传状态清空（测量码/身份/预武装/钩子不在此清）
		uploaded = {}
		upload_bytes = 0
		word_addr = 0
		load_phase, gate_left = "idle", 0
	end

	-- 软复位：重种芯片态；注入码/预武装/身份覆写/故障钩子保留
	local function soft_reset()
		seed_body()
		reset_count = reset_count + 1
	end

	-- ---------------- blob 加载状态机 ----------------

	local function start_load()
		if upload_bytes ~= BLOB_FIFO_LEN and upload_bytes ~= BLOB_FULL_LEN then
			load_phase, gate_left = "error", 0	-- 长度不符 → init_err
		elseif never_ready then
			load_phase, gate_left = "loading", math.huge
		elseif prearmed then
			load_phase, gate_left = "ok", 0		-- 守护零睡眠路径
		else
			load_phase, gate_left = "loading", LOAD_GATE_MS
		end
	end

	local function init_clear()
		uploaded = {}
		upload_bytes = 0
		word_addr = 0
		load_phase, gate_left = "idle", 0
	end

	-- 门限惰性到点（advance 已推时钟时首次读即完成）
	local function check_due()
		if load_phase == "loading" and not never_ready and gate_left <= 0 then
			load_phase, gate_left = "ok", 0
		end
	end

	local function status_code()
		if stuck_code ~= nil then return stuck_code end
		if load_phase == "ok" then return 0x01
		elseif load_phase == "error" then return 0x02 end
		return 0x00					-- idle / loading 均 0x00(not_init)
	end

	-- 单测虚拟时钟：驱动 milli_sleep 桩转调，推加载门
	function d:advance(ms)
		ms = math.max(0, math.floor(tonumber(ms) or 0))
		if load_phase == "loading" and not never_ready then
			gate_left = gate_left - ms
			if gate_left <= 0 then load_phase, gate_left = "ok", 0 end
		end
		return self
	end

	-- ---------------- 地址解码 ----------------

	local function meas_code(i)		-- i=0..11 对应 0x0C..0x17
		if i < 2 then return ax elseif i < 4 then return ay elseif i < 6 then return az
		elseif i < 8 then return gx elseif i < 10 then return gy else return gz end
	end

	local function byte_at(a)
		if a == R_CHIP_ID then return chip_id end
		if a >= R_ACC_X_LSB and a <= R_GYR_X_LSB + 5 then
			local i = a - R_ACC_X_LSB
			local v = meas_code(i)
			if i % 2 == 0 then return v % 256 end			-- LSB 在前
			return math.floor(v / 256) % 256				-- MSB
		end
		if a == R_INT_STATUS then return status_code() end
		if a == R_CMD then return 0x00 end				-- 只写读恒 0
		return regs[a] or 0							-- 稀疏影子，未定义读 0
	end

	-- ---------------- 单字节落盘（地址已折入 0x00..0xFF） ----------------

	local function write_byte(a, v)
		if a == R_CHIP_ID then return end				-- ROM 只读
		if a >= R_ACC_X_LSB and a <= R_GYR_X_LSB + 5 then
			return										-- 测量只读：写 ACK 丢弃
		end
		if a == R_INT_STATUS then return end
		if a == R_CMD then
			if v == CMD_SOFT_RESET then soft_reset() end
			return										-- 命令不落盘
		end
		if a == R_ACC_CONF or a == R_GYR_CONF then
			regs[a] = v								-- 全 8 位有效（驱动 RMW）
		elseif a == R_ACC_RANGE then
			regs[a] = v % 4
		elseif a == R_GYR_RANGE then
			regs[a] = v % 16							-- bit3 ois_range 合法
		elseif a == R_PWR_CONF then
			regs[a] = v % 8
		elseif a == R_PWR_CTRL then
			regs[a] = v % 16
		elseif a == R_INIT_CTRL then
			regs[a] = v % 2
			if (v % 2) == 0 then init_clear() else start_load() end
		elseif a == R_INIT_ADDR0 then
			regs[a] = v % 16							-- 仅低 4 位
		elseif a == R_INIT_ADDR1 then
			regs[a] = v
		elseif a == R_INIT_DATA then
			-- 自增落进窗口的单字节（帧陷阱的单字节退化形态）
			uploaded[word_addr * 2 + 1] = v
			upload_bytes = upload_bytes + 1
		else
			regs[a] = v								-- 未定义地址 ACK 落盘
		end
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
		if base == R_INIT_DATA then
			-- **地址陷阱**：帧内全部字节都落 0x5E 窗口（不普通自增、
			-- 不进寄存器影子），按当前字地址顺序进 blob 缓冲
			for k = 2, nb do
				uploaded[word_addr * 2 + (k - 1)] = bytes[k]
			end
			upload_bytes = upload_bytes + (nb - 1)
			word_addr = word_addr + math.floor((nb - 1) / 2)
		else
			for i = 2, nb do
				write_byte((base + i - 2) % 256, bytes[i])
			end
			-- 帧内触及 INIT_ADDR 0/1：按末值合成内部字地址
			local first, last = base, (base + nb - 2) % 256
			if first <= R_INIT_ADDR1 and last >= R_INIT_ADDR0 and first <= last then
				word_addr = (regs[R_INIT_ADDR0] or 0) + (regs[R_INIT_ADDR1] or 0) * 16
			end
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
		check_due()
		local t = {}
		for _ = 1, n do
			t[#t + 1] = byte_at(read_ptr)
			read_ptr = (read_ptr + 1) % 256			-- burst 自增、算术回绕
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

	-- 物理量逆注入：按当前 ACC_RANGE 档 LSB/g 反算原始码
	function d:set_accel_g(g_x, g_y, g_z)
		local lsb = ACC_LSB_PER_G[(regs[R_ACC_RANGE] or 2) % 4 + 1]
		ax = clampi(round_away((tonumber(g_x) or 0) * lsb), -32768, 32767)
		ay = clampi(round_away((tonumber(g_y) or 0) * lsb), -32768, 32767)
		az = clampi(round_away((tonumber(g_z) or 0) * lsb), -32768, 32767)
		return self
	end

	-- 物理量逆注入：按当前 GYR_RANGE 档 LSB/dps 反算（保留码 5..7 按 ±2000）
	function d:set_gyro_dps(w_x, w_y, w_z)
		local code = (regs[R_GYR_RANGE] or 0) % 8
		local lsb = GYR_LSB_PER_DPS[code + 1] or 16.384
		gx = clampi(round_away((tonumber(w_x) or 0) * lsb), -32768, 32767)
		gy = clampi(round_away((tonumber(w_y) or 0) * lsb), -32768, 32767)
		gz = clampi(round_away((tonumber(w_z) or 0) * lsb), -32768, 32767)
		return self
	end

	-- 覆写 ROM 身份喂 build 甄别负路径（0x26=BMI260 变体）；reset 回出厂
	function d:who(id)
		if id ~= nil then chip_id = clampi(id, 0, 255) end
		return self
	end

	-- 强制 INTERNAL_STATUS 返回某状态码（nil 解除）：喂 50×10ms 轮询超时
	function d:set_stuck_status(code)
		if code ~= nil then stuck_code = clampi(code, 0, 255) else stuck_code = nil end
		return self
	end

	function d:set_never_ready(on)
		never_ready = on ~= false
		return self
	end

	-- 守护逐拍造数：32 拍确定性正弦小码（默认 ±8g 档 Z≈1g≈4096），
	-- 连续测量直接装码；同时预武装——下次 INIT_CTRL=1 立即 init_ok。
	-- acc X 300±200 / Y -200∓150 / Z 4096±100；gyr X ±500 / Y ∓400 / Z ±300
	function d:virt_tick(n)
		n = math.floor(tonumber(n) or 0) % 32
		local s = math.sin(2 * math.pi * n / 32)
		ax = round_away(300 + 200 * s)
		ay = round_away(-200 - 150 * s)
		az = round_away(4096 + 100 * s)
		gx = round_away(500 * s)
		gy = round_away(-400 * s)
		gz = round_away(300 * s)
		prearmed = true
		return self
	end

	function d:fail_write(pred) write_pred = pred; return self end
	function d:fail_read(pred) read_pred = pred; return self end

	function d:clear_faults()
		d.nack, d.nack_read, d.nack_write = false, false, false
		write_pred, read_pred = nil, nil
		never_ready = false
		stuck_code = nil
		return self
	end

	function d:reset_logs()
		frames = {}
		write_count, read_count = 0, 0
		return self
	end

	-- 上电复位：全状态回 POR（身份/注入码/预武装/卡死态归位）；钩子保留
	function d:reset()
		chip_id = CHIP_ID
		ax, ay, az, gx, gy, gz = 0, 0, 0, 0, 0, 0
		prearmed = false
		never_ready = false
		stuck_code = nil
		read_ptr = nil
		frames = {}
		write_count, read_count, reset_count = 0, 0, 0
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
	function d:frames() return frames end
	function d:chip_id() return chip_id end
	function d:word_addr() return word_addr end
	function d:blob_len() return upload_bytes end
	function d:phase() return load_phase end
	function d:gate_left_ms() return gate_left end
	function d:init_status() return status_code() end
	function d:is_prearmed() return prearmed == true end

	-- blob 缓冲前 n 字节副本（缺省全部已收字节；空位补 0）
	function d:uploaded(n)
		n = n or upload_bytes
		local t = {}
		for i = 1, n do t[i] = uploaded[i] or 0 end
		return t
	end

	function d:accel_raw() return ax, ay, az end
	function d:gyro_raw() return gx, gy, gz end

	-- 当前配置解码（量程/ODR 码 + 电源位）
	function d:config()
		local ac, gc = regs[R_ACC_CONF] or 0, regs[R_GYR_CONF] or 0
		return {
			acc_conf = ac,
			gyr_conf = gc,
			acc_range = (regs[R_ACC_RANGE] or 0) % 4,
			gyr_range = (regs[R_GYR_RANGE] or 0) % 8,
			acc_odr = ac % 16,
			gyr_odr = gc % 16,
			pwr_conf = regs[R_PWR_CONF] or 0,
			pwr_ctrl = regs[R_PWR_CTRL] or 0,
		}
	end

	seed_body()
	return d
end

return M

-- ==================== 迁移订正（旧手写桩三处互证假绿） ====================
-- 1) ACC_CONF(0x40) POR 旧桩给 0x33（注释还写「odr=6Hz 假设值」），
--    GYR_CONF(0x42) 旧桩给 0x21，PWR_CONF(0x7C) 旧桩根本未播；datasheet
--    register overview RESET 列实证 0xA8 / 0xA9 / 0x03。
-- 2) 旧桩断言默认 build 后 0x40==0x38（沿用错 POR 0x33 推得）；真实
--    RMW 保高 4 位：(0xA8&0xF0)|0x08 = 0xA8，0x42=(0xA9&0xF0)|0x08=0xA8。
-- 3) 旧桩 i2c_write_multi 对非 0x5B/0x5E 帧用 regs[reg+k-1] 普通自增，
--    未模拟 INIT_DATA(0x5E) 的【地址陷阱】——真实 burst 写 0x5E 寄存器
--    地址不自增，整帧字节都进配置数据窗口而非 0x5F.. 影子；且旧桩是
--    dofile 后覆盖全局的假桩，无 NACK/故障钩子/加载门/virt_tick。
