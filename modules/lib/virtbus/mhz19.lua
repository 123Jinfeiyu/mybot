-- virtbus/mhz19.lua —— MH-Z19B NDIR CO₂ 传感器的虚拟 UART 器件模型
-- bot4cj 仓颉语言机器人框架
--
-- 对应真实驱动 modules/MHZ19.lua（9600 8N1，请求-应答定长帧，9 字节 + 累加和校验）。
-- 模型只存协议事实层：FF 01 86 请求帧解析（含校验和验证）、FF 86 应答帧组装、
-- ppm 双字节拆分；TEMP 字节到摄氏度的换算（f[5]-40）只存在于真实驱动，
-- 本模型收摄氏温度逆推原始字节。
--
-- 挂载（UART 路径即设备身份）：
--   local mm = dofile("modules/lib/virtbus/mhz19.lua")
--   local dev = mm.new()
--   vbus.attach_uart("MHZ19", "/dev/serial0", dev)
--
-- 协议（Winsen MH-Z19B 手册，与驱动一致）：
--   请求 9B：FF 01 86 00 00 00 00 00 CS；CS = (0x100 - Σb2..b8) % 0x100 = 0x79
--   应答 9B：FF 86 HI LO TEMP S 00 00 CS；CO₂ = HI*256 + LO，CS 覆盖 b2..b8
--   收到非法请求（帧头/命令/长度/校验和不符）不应答——真机对垃圾字节流
--   不回帧的事实；驱动侧表现为读超时。
--
-- on_write 三态（virtbus UART 契约）：本模型返回应答 9 字节表；故障钩子
-- 使其返回 nil（不应答）或 false（TX 失败）。
local M = {}
M.BUS = "uart"		-- 供 sensor_daemon proto=virt 区分 I2C/SPI/UART 挂载路径

local FRAME_LEN = 9
local RESP_HEAD = 0xFF
local RESP_CMD  = 0x86

-- MH-Z19 校验和：(0x100 - Σ(from..to)) % 0x100（与驱动 checksum 同式，纯算术）
local function checksum(t, from, to)
	local sum = 0
	for i = from, to do sum = sum + (tonumber(t[i]) or 0) end
	return (0x100 - (sum % 0x100)) % 0x100
end

function M.new()
	local self = {}

	-- ---- 器件状态（协议事实层原始量）----
	local co2 = 400			-- ppm（HI/LO 双字节合成值）
	local temp_raw = 65		-- 应答 TEMP 字节（驱动换算 TEMP-40 → 25°C）
	local status = 0		-- 应答 S 状态字节
	local requests = {}		-- 请求帧留痕 { bytes=, valid=bool }

	-- ---- 故障注入 ----
	local no_reply = false		-- 收合法请求也不应答（驱动读超时）
	local fail_write_flag = false	-- TX 失败（uart_write 返 -1）
	local corrupt_cs = false		-- 应答校验和翻转（驱动帧扫描全落空）
	local noise_on = false		-- 应答前垫 3 个无帧头噪声字节（跨轮聚合读路径）
	local NOISE = { 0x00, 0x01, 0x02 }	-- 不含 0xFF，保证不误构假帧头

	-- ===================== 注入 API =====================

	-- CO₂ ppm（协议码空间 0~65535 钳制；量程上下限是器件规格，不在此模拟）
	function self:set_co2(v)
		v = math.floor(tonumber(v) or 0)
		if v < 0 then v = 0 elseif v > 65535 then v = 65535 end
		co2 = v
		return self
	end
	function self:co2() return co2 end

	-- 摄氏温度逆推 TEMP 原始字节（-40~215 对应字节 0~255；越界钳制）
	function self:set_temp_c(c)
		local b = math.floor(tonumber(c) or 0) + 40
		if b < 0 then b = 0 elseif b > 255 then b = 255 end
		temp_raw = b
		return self
	end
	function self:temp_raw() return temp_raw end

	function self:set_status(s)
		s = math.floor(tonumber(s) or 0) % 256
		status = s
		return self
	end
	function self:status() return status end

	-- ---- 故障钩子 ----
	function self:no_reply(on) no_reply = (on ~= false); return self end
	function self:fail_write(on) fail_write_flag = (on ~= false); return self end
	function self:corrupt_checksum(on) corrupt_cs = (on ~= false); return self end
	function self:noise(on) noise_on = (on ~= false); return self end
	function self:clear_faults()
		no_reply = false
		fail_write_flag = false
		corrupt_cs = false
		noise_on = false
		return self
	end

	-- ===================== 留痕 / 复位 API =====================

	function self:requests() return requests end
	function self:request_count() return #requests end
	function self:reset_logs()
		requests = {}
		return self
	end

	-- 器件状态复位（回 400ppm 初值/清日志）；不清故障钩子（同其余 virtbus
	-- 模型约定——负路径 section 重开时钩子必须仍在才不假绿）。
	function self:reset()
		co2 = 400
		temp_raw = 65
		status = 0
		requests = {}
		return self
	end

	-- ===================== 应答帧组装 =====================

	local function build_reply()
		local hi = math.floor(co2 / 256) % 256
		local lo = co2 % 256
		local f = { RESP_HEAD, RESP_CMD, hi, lo, temp_raw, status, 0x00, 0x00, 0x00 }
		f[9] = checksum(f, 2, 8)
		if corrupt_cs then f[9] = (f[9] + 0xA5) % 256 end
		return f
	end

	-- on_write(bytes)：点定义、无 self 参数（virtbus attach_uart 动态委托契约）。
	-- 收主机 1 基字节表；返回应答表 = 入环，nil = 不应答，false = TX 失败。
	local function on_write(bytes)
		if fail_write_flag then return false end
		-- 请求帧合法性：FF 01 86 开头、9 字节、尾字节 = b2..b8 校验和
		local valid = #bytes == FRAME_LEN
			and bytes[1] == 0xFF and bytes[2] == 0x01 and bytes[3] == RESP_CMD
			and bytes[9] == checksum(bytes, 2, 8)
		requests[#requests + 1] = { bytes = bytes, valid = valid }
		if not valid or no_reply then return nil end
		if noise_on then
			local reply = build_reply()
			local out = {}
			for _, b in ipairs(NOISE) do out[#out + 1] = b end
			for _, b in ipairs(reply) do out[#out + 1] = b end
			return out
		end
		return build_reply()
	end
	self.on_write = on_write

	-- ===================== 守护节拍（确定性合成波形）====================
	-- 32 拍一周的 CO₂ 爬坡：400 + m*128 ppm（m = n % 32，覆盖 400~4368），
	-- 无随机、逐拍确定；温度/状态保持注入值不动（E2E 只消费 CO2 字段）。
	function self:virt_tick(n)
		n = tonumber(n) or 0
		return self:set_co2(400 + (n % 32) * 128)
	end

	return self
end

return M
