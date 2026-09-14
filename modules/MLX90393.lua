-- MLX90393 三轴磁力计 驱动模块（Melexis Triaxis Hall，I2C 地址 0x0C~0x0F）
-- bot4cj 仓颉语言机器人框架
--
-- 用法（结构脚本 .sql）：
--   CREATE TABLE MLX90393 (
--       name    String,
--       I2C     String,
--       address String,
--       gain    String,
--       timeout String
--   );
--   INSERT INTO MLX90393 (name, I2C, address, gain, timeout)
--     VALUES ('mlx0', 'i2c-1', '0x0C', '-', '-');
--
-- 列说明：
--   gain    = GAIN_SEL 0~7（0=最低增益 0.751µT/LSB 量程最大，7=最高增益
--              0.150µT/LSB，灵敏度相差 5 倍），非法值钳到界内，缺省 0
--   timeout = 单次测量就绪轮询超时 ms（缺省 50；就绪位轮不到即报 ERR）
--   各列写 '-' 按缺省值处理
--
-- call 无需额外参数，返回：
--   { result, X/Y/Z(µT，浮点), T(°C), XR/YR/ZR(有符号原始计数), TR(无符号),
--     status(状态字节) }
-- 接 sensor_daemon：fields=X:f|Y:f|Z:f|T:f（驱动内已做补码还原，浮点透传）
--
-- 协议要点（datasheet 3901090393 Rev012 §15；命令式，非寄存器地图）：
--   每条命令的应答【首字节永远是 status】，其后才是数据；先发命令写事务，
--   再 Sr 纯读（NOP 0x00 无副作用，驱动用它轮询 status）。
--   命令：0x00 NOP | 0x10+zyxt SB 突发 | 0x20+zyxt SW 唤醒 |
--         0x30+zyxt SM 单次 | 0x40+zyxt RM 读测量 | 0x50 RR 读寄存器 |
--         0x60 WR 写寄存器 | 0x80 EX 退出模式 | 0xD0 HR Memory Recall |
--         0xE0 HS Memory Store | 0xF0 RT 复位
--   zyxt：bit0=T bit1=X bit2=Y bit3=Z，故 0x3F=SM XYZT、0x4F=RM XYZT；
--         RM/ SM 的片内转换顺序为 TXYZ，应答按 Status-TXYZ 组帧，zyxt
--         中为 0 的分量跳过。
--   WR 帧【4 字节】：[0x60, D15..8, D7..0, A5..A0<<2]（地址在独立第 4 字节）
--   RR 帧【2 字节】：[0x50, A5..A0<<2]，应答 3 字节 = [status, D15..8, D7..0]
--   status：bit7=BURST_MODE bit6=WOC_MODE bit5=SM_MODE（单次转换完成）、
--           bit4=ERROR、bit3=SED、bit2=RS（复位后首帧自清）、bit1:0=D
--           （仅 RR/RM 有效：附加数据字节数 = 2D+2）
--   RM XYZT 应答 9 字节 [status,Tmsb,Tlsb,Xmsb,Xlsb,Ymsb,Ylsb,Zmsb,Zlsb]，
--   走 HAL.i2c_read_bytes（大端合并超 2^53 精度线）
--
-- 寄存器 0x00 = Z-SERIES[15]/GAIN_SEL[14:12]/HALLCONF[11:8]，POR=0x007C
--   （GAIN_SEL=7、HALLCONF=0xC）；build 读-改-写保留 HALLCONF，只改 GAIN_SEL。
--
-- 换算（HALLCONF=0xC、RES=0、TC off、25°C，datasheet Table 17）：
--   GAIN_SEL=0 时 XY=0.751 / Z=1.210 µT/LSB；GAIN_SEL 7~0 的增益倍数表
--   {1,4/3,5/3,2,2.5,3,4,5}，SEL=0 对应 5（相对 SEL=7 基准 0.150/0.242）。
--   温度：45.2 LSB/°C、25°C 输出 46244（u16 无符号），°C = 25+(TR-46244)/45.2。
--
-- build 流程（自检链路）：RT 复位 -> HR Memory Recall（POR/RT 时片内其实
--   自动执行一次 HR，显式再做一次无害）-> 写 GAIN_SEL（读-改-写 + 回读校验）
--   -> SM/RM 试测一帧并丢弃（甄别在位、打通 触发/轮询/读回 全链路）

-- 加载纯 Lua 基层硬件抽象层（HAL，issue #14：不再依赖仓颉 cfunc）
-- 调试打印辅助 print_table（modules/lib/）
local mpath = dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua")
dofile(mpath.resolve("lib/HAL.lua"))
dofile(mpath.resolve("lib/print_table.lua"))
dofile(mpath.resolve("lib/drv_common.lua"))
local bitc = dofile(mpath.resolve("bitcompat.lua"))

-- 调试开关：传感器高频读取，默认关闭逐次打印
local DEBUG = false

-- ==================== 命令与状态 ====================
local CMD_NOP    = 0x00
local CMD_SB     = 0x10		-- 启动突发模式（低 4 位 zyxt）
local CMD_SW     = 0x20		-- 启动 Wake-on-Change（低 4 位 zyxt）
local CMD_SINGLE = 0x30		-- 启动单次测量（低 4 位 zyxt）
local CMD_READ   = 0x40		-- 读测量结果（低 4 位 zyxt）
local CMD_RR     = 0x50		-- 读寄存器（第 2 字节 = 地址<<2）
local CMD_WR     = 0x60		-- 写寄存器（4 字节帧）
local CMD_EXIT   = 0x80		-- 退出当前模式
local CMD_RECALL = 0xD0		-- HR：NVRAM -> 易失内存（恢复出厂 TRIM）
local CMD_STORE  = 0xE0		-- HS：易失内存 -> NVRAM
local CMD_RESET  = 0xF0		-- RT：复位

local FLAG_T = 0x01
local FLAG_X = 0x02
local FLAG_Y = 0x04
local FLAG_Z = 0x08

-- status 字节布局（datasheet §15.2 Table 12，status 永远是应答首字节）
local ST_BURST = 0x80		-- bit7：当前处于突发模式
local ST_WOC   = 0x40		-- bit6：当前处于 Wake-on-Change 模式
local ST_SM    = 0x20		-- bit5：单次测量完成（SM_MODE）
local ST_ERROR = 0x10		-- bit4：命令被拒绝/不可纠正错误
local ST_RS    = 0x04		-- bit2：复位发生（RT 后首 status 置位、自清）

local CMD_XYZT  = CMD_SINGLE + FLAG_T + FLAG_X + FLAG_Y + FLAG_Z	-- 0x3F
local READ_XYZT = CMD_READ  + FLAG_T + FLAG_X + FLAG_Y + FLAG_Z	-- 0x4F
local RESP_N    = 9			-- 0x4F 应答：status + T/X/Y/Z 各 2 字节

-- 应答内偏移（1 基；status 占 b[1]，片内转换/组帧顺序 TXYZ）
local OFF_STATUS = 1
local OFF_T = 2
local OFF_X = 4
local OFF_Y = 6
local OFF_Z = 8

local REG_GAIN = 0x00		-- Z-SERIES/GAIN_SEL[6:4]/HALLCONF[3:0]，POR=0x007C

-- ==================== 时序常量 ====================
-- RT 后 datasheet 建议等 1.5ms（取 5ms 余量）；HR 无单独等待要求（留 2ms）
local T_RESET_MS  = 5
local T_RECALL_MS = 2
local POLL_MS     = 1		-- 就绪轮询步长

-- ==================== 灵敏度换算（datasheet Table 17，HALLCONF=0xC/RES=0） ====================
-- GAIN_SEL=0 基准：XY=0.751 / Z=1.210 µT/LSB；GAIN_TAB 是 SEL 7~0 相对
-- SEL=7（最高增益 1x）的增益倍数：SEL=0 为 5x。µT/LSB 随倍数正比放大。
local SENS_XY_0 = 0.751
local SENS_Z_0  = 1.210
local GAIN_BASE = 5									-- SEL=0 对应倍数
local GAIN_TAB  = { 5, 4, 3, 2.5, 2, 5 / 3, 4 / 3, 1 }	-- GAIN_SEL 0~7

-- 温度（datasheet §电气特性）：45.2 LSB/°C，25°C 输出 46244（u16 无符号）
local T_LSB_PER_C = 45.2
local T_RAW_AT_25 = 46244

local DEF_GAIN    = 0
local DEF_TIMEOUT = 50		-- ms

-- 模块级配置（build 写入，跨 call 保持于状态机内存，destroy 后废弃）
local sens_xy			-- XY 轴 µT/LSB
local sens_z			-- Z 轴 µT/LSB
local poll_timeout_ms	-- 就绪轮询超时 ms

-- 16 位有符号还原
local function s16(v)
	if v >= 0x8000 then v = v - 0x10000 end
	return v
end

-- 发送 1 字节命令，成功返回 true
local function send_cmd(fd, cmd)
	return i2c_write_multi(fd, cmd) == 0
end

-- 读 status：NOP 查询（命令本身无副作用），返回 0~255；失败返回 nil
local function read_status(fd)
	if not send_cmd(fd, CMD_NOP) then return nil end
	local v = i2c_read(fd, 1)
	if v < 0 then return nil end
	return v
end

-- 写寄存器（WR 4 字节帧）：[0x60, D15..8, D7..0, A5..A0<<2]
local function write_reg(fd, addr, data16)
	return i2c_write_multi(fd, CMD_WR,
		math.floor(data16 / 256) % 256, data16 % 256, addr * 4) == 0
end

-- 读 16 位寄存器（RR：[0x50, addr<<2] -> [status,Dhi,Dlo] 3 字节应答），
-- 返回 16 位值；失败返回 nil
local function read_reg16(fd, addr)
	if i2c_write_multi(fd, CMD_RR, addr * 4) ~= 0 then return nil end
	local b = i2c_read_bytes(fd, 3)
	if b == nil then return nil end
	return b[2] * 256 + b[3]
end

-- 单次测量：SM 触发 -> NOP 轮询 SM_MODE -> RM 读 9 字节应答。
-- 成功返回字节数组，失败返回 nil + 错误信息
local function measure_once(fd)
	if not send_cmd(fd, CMD_XYZT) then
		return nil, "Failed to start MLX90393 single measurement"
	end

	local left = poll_timeout_ms
	while left > 0 do
		milli_sleep(POLL_MS)
		left = left - POLL_MS

		local st = read_status(fd)
		if st == nil then
			return nil, "Failed to poll MLX90393 status"
		end
		if bitc.band(st, ST_ERROR) ~= 0 then
			return nil, string.format("MLX90393 status ERROR (0x%02X)", st)
		end
		if bitc.band(st, ST_SM) ~= 0 then
			if not send_cmd(fd, READ_XYZT) then
				return nil, "Failed to send MLX90393 read command"
			end
			local b = i2c_read_bytes(fd, RESP_N)
			if b == nil then
				return nil, "Failed to read MLX90393 measurement"
			end
			return b
		end
	end
	return nil, string.format("MLX90393 measurement timeout (%dms, SM_MODE never set; check wiring)", poll_timeout_ms)
end

-- 解析 9 字节应答（status-first，TXYZ 顺序）并换算，返回结果表
local function parse_response(b)
	local tr = b[OFF_T] * 256 + b[OFF_T + 1]			-- 温度 u16 无符号
	local xr = s16(b[OFF_X] * 256 + b[OFF_X + 1])
	local yr = s16(b[OFF_Y] * 256 + b[OFF_Y + 1])
	local zr = s16(b[OFF_Z] * 256 + b[OFF_Z + 1])

	local ret = {
		result = "OK",
		X = xr * sens_xy,				-- µT（浮点）
		Y = yr * sens_xy,
		Z = zr * sens_z,
		T = 25 + (tr - T_RAW_AT_25) / T_LSB_PER_C,	-- °C
		XR = xr, YR = yr, ZR = zr, TR = tr,	-- 磁场有符号；温度无符号
		status = b[OFF_STATUS]
	}

	if DEBUG then
		print(string.format("MLX90393: X=%.2fµT Y=%.2fµT Z=%.2fµT T=%.2f°C", ret.X, ret.Y, ret.Z, ret.T))
	end

	return ret
end

-- 【配置脚本】复位 -> Memory Recall -> 写 GAIN_SEL -> 试测一帧自检
function build(data)

	if type(data) ~= "table" then	-- 检查该参数是否是一张表
		return {
			result = "ERR",
			message = "Invalid Arguments Type: table"
		}
	end

	if DEBUG then
		dbg_table("-------- MLX90393 builder func get a table --------", data)
	end

	-----------------------------------------------------
	local fd = tonumber(data.fd)
	local addr = tonumber(data.address)
	if fd == nil or addr == nil then
		return {
			result = "ERR",
			message = "Invalid Argument: fd or address"
		}
	end

	if not i2c_set_addr(fd, addr) then
		return {
			result = "ERR",
			message = "Invalid Argument: fd or address (set_addr failed)"
		}
	end

	-- gain 列 -> GAIN_SEL：取整后钳到 0~7（非法/'-' 归缺省 0）
	local gain = tonumber(data.gain)
	if gain == nil then gain = DEF_GAIN end
	gain = math.floor(gain)
	if gain < 0 then gain = 0 end
	if gain > 7 then gain = 7 end

	-- timeout 列 -> 轮询超时 ms（非法/非正数归缺省）
	local to = tonumber(data.timeout)
	if to == nil or to < 1 then to = DEF_TIMEOUT end
	poll_timeout_ms = math.floor(to)

	-- 灵敏度：µT/LSB 随增益倍数正比（SEL=0 基准 × tab/5）
	sens_xy = SENS_XY_0 * GAIN_TAB[gain + 1] / GAIN_BASE
	sens_z  = SENS_Z_0  * GAIN_TAB[gain + 1] / GAIN_BASE

	-----------------------------------------------------
	-- RT 暖复位（datasheet：RT 后片内自动执行一次 HR，易失内存回 NVRAM）
	if not send_cmd(fd, CMD_RESET) then
		return {
			result = "ERR",
			message = "Failed to reset MLX90393"
		}
	end
	milli_sleep(T_RESET_MS)

	-- HR 显式再恢复一次出厂 TRIM（自动 HR 之外的冗余保险，无害）
	if not send_cmd(fd, CMD_RECALL) then
		return {
			result = "ERR",
			message = "Failed to recall MLX90393 TRIM"
		}
	end
	milli_sleep(T_RECALL_MS)

	-- 写 GAIN_SEL（读-改-写：保留 GAIN 寄存器其余位，只改 [6:4]）
	local g = read_reg16(fd, REG_GAIN)
	if g == nil then
		return {
			result = "ERR",
			message = "Failed to read MLX90393 GAIN register"
		}
	end
	local newg = bitc.bor(bitc.band(g, 0xFF8F), bitc.lshift(gain, 4))
	if not write_reg(fd, REG_GAIN, newg) then
		return {
			result = "ERR",
			message = "Failed to write MLX90393 GAIN register"
		}
	end
	-- 回读校验
	local g2 = read_reg16(fd, REG_GAIN)
	if g2 ~= newg then
		return {
			result = "ERR",
			message = string.format("MLX90393 GAIN verify failed: wrote 0x%04X, read back 0x%04X", newg, g2 or 0)
		}
	end

	-- 试测一帧并丢弃：甄别在位 + 打通 触发/轮询/读回 全链路
	local b, err = measure_once(fd)
	if b == nil then
		return {
			result = "ERR",
			message = err
		}
	end

	print(string.format("MLX90393 init OK: addr=0x%02X, gain=%d, sens_xy=%.4f sens_z=%.4f µT/LSB, timeout=%dms",
		addr, gain, sens_xy, sens_z, poll_timeout_ms))
	return {  result = "OK"  }

end

-- 【销毁MLX90393】无资源需要释放（总线归 I2C 硬件变量所有）
function destroy(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	return {  result = "OK"  }

end


-- 【调用MLX90393】单次测量：X/Y/Z（µT）、T（°C）+ 有符号原始计数 + 状态字节
-- NOTE: 接收一个参数（表），返回一个结果（表）
function call(data)

	if type(data) ~= "table" then	-- 检查该参数是否是一张表
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	if DEBUG then
		dbg_table("-------- MLX90393 caller func get a table --------", data)
	end

	-----------------------------------------------------
	local fd = tonumber(data.fd)
	local addr = tonumber(data.address)
	if fd == nil or addr == nil then
		return {
			result = "ERR",
			message = "Invalid Argument: fd or address"
		}
	end

	if not i2c_set_addr(fd, addr) then
		return {
			result = "ERR",
			message = "Invalid Argument: fd or address (set_addr failed)"
		}
	end

	-- build 前置守卫：poll_timeout_ms/sens_xy 仅 build 赋值，
	-- 未 build 先 call 会对 nil 做比较/算术裸 error（与 INA219/LTC2944 对齐）
	if poll_timeout_ms == nil or sens_xy == nil then
		return {
			result = "ERR",
			message = "MLX90393 not built (build first)"
		}
	end

	-----------------------------------------------------
	local b, err = measure_once(fd)
	if b == nil then
		return {
			result = "ERR",
			message = err
		}
	end

	return parse_response(b)

end
