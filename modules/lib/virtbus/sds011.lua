-- virtbus/sds011.lua —— Nova Fitness SDS011 激光颗粒物传感器的虚拟 UART 器件模型
-- bot4cj 仓颉语言机器人框架
--
-- 对应真实驱动 modules/SDS011.lua（9600 8N1，被动查询模式；请求 19 字节定长帧、
-- 应答 10 字节定长帧，均为累加和 mod 256 校验）。
-- 模型只存协议事实层：AA B4 请求帧解析（命令 + 校验和验证）、按命令组装 10 字节
-- 应答（PM 小端双字节拆分 / 命令回显 / 固件版本）；µg/m³ 换算（原始值 /10）与
-- TEMP 类语义换算只存在于真实驱动，本模型一律以协议原始 16 位量存值。
--
-- 挂载（UART 路径即设备身份）：
--   local mm = dofile("modules/lib/virtbus/sds011.lua")
--   local dev = mm.new()
--   vbus.attach_uart("SDS011", "/dev/serial0", dev)
--
-- 协议（驱动头注释：py-sds011 与 ricki-z/SDS011 两实现互证）：
--   请求 19B：AA B4 CMD D0..D11 ID1 ID2 CS AB；CS = Σ(CMD..ID2) % 256，ID 用 FF FF 广播
--   应答 10B：AA C5 b3 b4 b5 b6 ID1 ID2 CS AB；CS = Σb3..b8 % 256
--   （驱动 read_frame 的窗口即 10 字节：acc[i+2..i+7] 求和、acc[i+8] 比对，
--     故数据段只有 4 字节，ID 两字节计入校验和。）
--   同一帧结构按命令换语义：0x04 查询 → b3..b6 = PM25L PM25H PM10L PM10H；
--   0x02/0x06 命令 ACK → b3 b4 = 命令号 D0（驱动据此回读校验 passive mode）；
--   0x07 版本 → b3 b4 = 固件主/次版本。
--   收到非法请求（帧头/长度/校验和不符或未知命令）不应答——真机对垃圾字节流
--   不回帧；驱动侧表现为读超时。
--
-- on_write 三态（virtbus UART 契约）：返回应答表 = 入环；nil = 不应答；
-- false = 本笔 TX 失败（uart_write 返 -1）。
local M = {}
M.BUS = "uart"		-- 供 sensor_daemon proto=virt 区分 I2C/SPI/UART 挂载路径

local REQ_LEN  = 19
local RESP_LEN = 10
local HEAD_REQ = 0xAA
local HEAD_RESP = 0xAA
local CMD_BYTE = 0xB4		-- 请求第二字节
local RESP_CMD = 0xC5		-- 应答第二字节
local TAIL     = 0xAB
local ID1, ID2 = 0xFF, 0xFF	-- 广播地址（应答原样回）

-- 应答 b3..b8 累加和 mod 256（与驱动 read_frame 兜底校验同式，纯算术）
local function checksum(t, from, to)
	local sum = 0
	for i = from, to do sum = sum + (tonumber(t[i]) or 0) end
	return sum % 0x100
end

function M.new()
	local self = {}

	-- ---- 器件状态（协议事实层原始量：0.1 µg/m³ 计）----
	local pm25_raw = 423		-- 42.3 µg/m³
	local pm10_raw = 1000		-- 100.0 µg/m³
	local fw_major, fw_minor = 7, 0	-- 驱动横幅 "%d.%d" 取应答 b3 b4
	local passive = false		-- 0x02 D0=1 后置位（上电默认主动推送）
	local sleeping = false		-- 0x06 D0=0 后置位（风扇停转）
	local requests = {}		-- 请求帧留痕 { bytes=, cmd=, d0=, valid=bool }

	-- ---- 故障注入 ----
	local no_reply = false		-- 收合法请求也不应答（驱动读超时）
	local fail_write_flag = false	-- TX 失败（uart_write 返 -1）
	local corrupt_cs = false		-- 应答校验和 +1 翻转（驱动帧扫描全落空）
	local stale_push = false		-- 应答前垫一帧「主动模式残留推送」（命令切换 ACK 场景）
	local swallow_cmd = nil		-- 只吞指定命令的一次应答（模拟版本查询丢失）

	-- 残留主动帧：AA C5 | 00 00 00 00 | FF FF | 00 | AB——
	-- 帧头/ID/尾全对，唯 CS 字段填 0（真值应为 FE；真机主动流被 cut 在半帧处的等价形态）：
	-- 驱动扫描时 AA C5 命中但校验和不过 → 跳过，下一轮读取才命中真应答帧。
	-- 注意不能填合法 CS：全零数据帧会被驱动当成合法查询应答（PM=0.0）误消费。
	local STALE = { 0xAA, 0xC5, 0, 0, 0, 0, ID1, ID2, 0x00, TAIL }

	-- ===================== 注入 API =====================

	-- 原始 16 位量（协议码空间 0~65535 钳制；真机量程上限是器件规格，不在此模拟）
	function self:set_pm25_raw(v)
		v = math.floor(tonumber(v) or 0)
		if v < 0 then v = 0 elseif v > 65535 then v = 65535 end
		pm25_raw = v
		return self
	end
	function self:pm25_raw() return pm25_raw end

	function self:set_pm10_raw(v)
		v = math.floor(tonumber(v) or 0)
		if v < 0 then v = 0 elseif v > 65535 then v = 65535 end
		pm10_raw = v
		return self
	end
	function self:pm10_raw() return pm10_raw end

	-- 固件版本（应答 b3 b4；驱动 string.format("%d.%d") 展示）
	function self:set_firmware(major, minor)
		fw_major = math.floor(tonumber(major) or 0) % 256
		fw_minor = math.floor(tonumber(minor) or 0) % 256
		return self
	end
	function self:firmware() return fw_major, fw_minor end

	-- 被动模式状态（真机上电默认主动推送，build 期由驱动切被动）
	function self:passive_mode() return passive end
	function self:sleeping() return sleeping end

	-- ---- 故障钩子 ----
	function self:no_reply(on) no_reply = (on ~= false); return self end
	function self:fail_write(on) fail_write_flag = (on ~= false); return self end
	function self:corrupt_checksum(on) corrupt_cs = (on ~= false); return self end
	function self:stale_push(on) stale_push = (on ~= false); return self end
	-- 定向吞一次：只对本次命令号为 cmd 的合法请求不应答（后续命令恢复正常）。
	-- 驱动 build 期第一条命令是 0x02 切被动，若无差别吞掉首次应答会直接让 build 失败，
	-- 故「版本读不到」这类降级路径必须按命令定向注入（cmd=0x07）。
	function self:swallow_next(cmd) swallow_cmd = tonumber(cmd); return self end
	function self:clear_faults()
		no_reply = false
		fail_write_flag = false
		corrupt_cs = false
		stale_push = false
		swallow_cmd = nil
		return self
	end

	-- ===================== 留痕 / 复位 API =====================

	function self:requests() return requests end
	function self:request_count() return #requests end
	function self:reset_logs()
		requests = {}
		return self
	end

	-- 器件状态复位（回默认浓度/主动模式/风扇运转/清日志）；不清故障钩子
	-- （同其余 virtbus 模型约定——负路径 section 重开时钩子必须仍在才不假绿）。
	function self:reset()
		pm25_raw = 423
		pm10_raw = 1000
		fw_major, fw_minor = 7, 0
		passive = false
		sleeping = false
		requests = {}
		return self
	end

	-- ===================== 应答帧组装 =====================

	-- 10 字节应答：b1 b2 = AA C5，b3..b6 由调用方给，b7 b8 = ID，b9 = CS、b10 = AB
	local function resp(data4)
		local f = { HEAD_RESP, RESP_CMD }
		for i = 1, 4 do f[2 + i] = data4[i] or 0 end
		f[7], f[8] = ID1, ID2
		f[9] = checksum(f, 3, 8)
		f[10] = TAIL
		if corrupt_cs then f[9] = (f[9] + 1) % 256 end
		return f
	end

	local function ack(cmd, d0)
		return resp({ cmd, d0, 0, 0 })
	end

	local function query_frame()
		return resp({ pm25_raw % 256, math.floor(pm25_raw / 256) % 256,
			pm10_raw % 256, math.floor(pm10_raw / 256) % 256 })
	end

	-- on_write(bytes)：点定义、无 self 参数（virtbus attach_uart 动态委托契约）。
	-- 收主机 1 基字节表；返回应答表 = 入环，nil = 不应答，false = TX 失败。
	local function on_write(bytes)
		if fail_write_flag then return false end
		-- 请求帧合法性：AA B4 开头、19 字节、b18 = Σb3..b17、b19 = AB
		local valid = #bytes == REQ_LEN
			and bytes[1] == HEAD_REQ and bytes[2] == CMD_BYTE
			and bytes[19] == TAIL
			and bytes[18] == checksum(bytes, 3, 17)
		local cmd, d0 = bytes[3] or 0, bytes[4] or 0
		requests[#requests + 1] = { bytes = bytes, cmd = cmd, d0 = d0, valid = valid }
		if not valid or no_reply then return nil end
		local reply
		if cmd == 0x02 then
			passive = (d0 == 1)
			reply = ack(0x02, d0)
		elseif cmd == 0x04 then
			reply = query_frame()
		elseif cmd == 0x06 then
			sleeping = (d0 == 0)
			reply = ack(0x06, d0)
		elseif cmd == 0x07 then
			reply = resp({ fw_major, fw_minor, 0, 0 })
		else
			return nil		-- 未知命令：真机不回帧
		end
		if swallow_cmd ~= nil and swallow_cmd == cmd then
			swallow_cmd = nil
			return nil
		end
		if stale_push then
			local out = {}
			for _, b in ipairs(STALE) do out[#out + 1] = b end
			for _, b in ipairs(reply) do out[#out + 1] = b end
			return out
		end
		return reply
	end
	self.on_write = on_write

	-- ===================== 守护节拍（确定性合成波形）====================
	-- 5 拍一周的扬尘爬坡：PM2.5 = 105 + m*8、PM10 = 163 + m*8（0.1 µg/m³ 计，
	-- m = n % 5）→ 10.5/11.3/12.1/12.9/13.7 与 16.3/17.1/17.9/18.7/19.5 µg/m³。
	-- 两列原始值 mod 10 恒非 0（5 与 3 起步、步长 8），故 PM 读数恒带一位小数——
	-- E2E 的浮点断言可直接锚守护 raw 行 "v0":NN.N（与 MHZ19 的整数 ppm 相反，
	-- 无需退到 Monitor Ema 的 out 字段）。无随机、逐拍确定。
	function self:virt_tick(n)
		n = tonumber(n) or 0
		local m = n % 5
		self:set_pm25_raw(105 + m * 8)
		self:set_pm10_raw(163 + m * 8)
		return self
	end

	return self
end

return M
