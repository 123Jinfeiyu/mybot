-- ADS1256.lua 离线测试（桩替身，不触碰真实 SPI 硬件）
-- 用法: luajit test/test_ads1256.lua（ADS1256 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：dofile HAL（mock HAL.sys.*），再 dofile ADS1256.lua；
-- SPI 传输经 HAL.sys.ioctl_p 拦截，记录 tx 字节序列与 rx 预填充。
-- 覆盖：build 参数校验/寄存器写入序列、call 单端/差分读取与电压换算、
--       destroy SDATAC 命令、SPI 传输失败路径。

local tester = dofile("test/lib/tester.lua")
local ffi = require("ffi")

-- ==================== 加载驱动 ====================
local HAL = dofile("modules/lib/HAL.lua")
dofile("modules/lib/print_table.lua")
dofile("modules/lib/drv_common.lua")
dofile("modules/bitcompat.lua")
dofile("modules/ADS1256.lua")

-- ==================== mock 层 ====================
local tx_log = {}	-- 记录每次 SPI 事务的 tx 字节
local rx_data = {}	-- 预置 SPI 读返回字节（1 基数组）
local mock_fail = {}	-- mock_fail[op] = true 时对应操作返回失败

HAL.sys.open = function(path, flags) return 7 end
HAL.sys.close = function(fd) return 0 end
HAL.sys.usleep = function(us) end	-- 跳过真实延时

HAL.sys.ioctl_p = function(fd, req, ptr)
	if req == HAL.SPI_IOC_MESSAGE_1 then
		local len = tonumber(ptr[0].len)
		local rec = { len = len }
		if ptr[0].tx_buf ~= 0 then
			local txb = ffi.cast("const uint8_t *", ffi.cast("const void *", ptr[0].tx_buf))
			rec.tx = {}
			for i = 0, len - 1 do rec.tx[i + 1] = tonumber(txb[i]) end
		end
		if ptr[0].rx_buf ~= 0 then
			local rxb = ffi.cast("uint8_t *", ffi.cast("void *", ptr[0].rx_buf))
			for i = 0, len - 1 do
				rxb[i] = rx_data[i + 1] or 0
			end
		end
		tx_log[#tx_log + 1] = rec
		if mock_fail.spi then return -1 end
		return 0
	end
	-- spi_setup 相关 ioctl
	if mock_fail.spi_setup then return -1 end
	return 0
end

local function reset()
	tx_log = {}
	rx_data = {}
	mock_fail = {}
end

-- ==================== 常量（与驱动一致）====================
local CMD_RESET  = 0xFE
local CMD_WAKEUP = 0x00
local CMD_SDATAC = 0x0F
local CMD_RDATA  = 0x01
local CMD_RDATAC = 0x03
local CMD_SELFCAL = 0xF0
local CMD_SYNC   = 0xFC
local CMD_WREG   = 0x50

local function wreg_cmd(reg) return bit.bor(CMD_WREG, reg) end

-- ==================== build 测试 ====================
local res

-- 1. 缺 fd
reset()
res = build({ delay = 0, drate = 5, pga = 0, channel = 0 })
tester.check("build 缺 fd 返回 ERR", tostring(res.result) == "ERR", res.message)

-- 2. 非表参数
reset()
res = build("not a table")
tester.check("build 非表参数返回 ERR", tostring(res.result) == "ERR", res.message)

-- 3. 正常 build（默认参数：drate=5, pga=0, channel=0）
reset()
res = build({ fd = 7, delay = 0 })
tester.check("build 默认参数返回 OK", tostring(res.result) == "OK", res.message)

-- 检查发送的命令序列：RESET → WAKEUP → WREG STATUS → WREG MUX → WREG ADCON → WREG DRATE → SELFCAL → RDATAC
tester.check("build 发出 RESET 命令", tx_log[1].tx[1] == CMD_RESET)
tester.check("build 发出 WAKEUP 命令", tx_log[2].tx[1] == CMD_WAKEUP)
-- WREG STATUS: {0x50|0x00, 0x00, 0x14}
tester.check("build 写 STATUS 寄存器 cmd", tx_log[3].tx[1] == wreg_cmd(0x00))
tester.check("build 写 STATUS 寄存器 val=0x14", tx_log[3].tx[3] == 0x14)
-- WREG MUX: channel=0 → MUX = (0<<4)|0x08 = 0x08
tester.check("build 写 MUX 寄存器 val=0x08", tx_log[4].tx[3] == 0x08)
-- WREG ADCON: pga=0
tester.check("build 写 ADCON 寄存器 val=0", tx_log[5].tx[3] == 0)
-- WREG DRATE: drate=5 → 0xA0 (1000SPS)
tester.check("build 写 DRATE 寄存器 val=0xA0", tx_log[6].tx[3] == 0xA0)
tester.check("build 发出 SELFCAL 命令", tx_log[7].tx[1] == CMD_SELFCAL)
tester.check("build 发出 RDATAC 命令", tx_log[8].tx[1] == CMD_RDATAC)
tester.check("build 共 8 次 SPI 传输", #tx_log == 8)

-- 4. build 自定义参数：drate=0 (30000SPS), pga=3 (×8), channel=5
reset()
res = build({ fd = 7, delay = 0, drate = 0, pga = 3, channel = 5 })
tester.check("build 自定义参数返回 OK", tostring(res.result) == "OK")
-- MUX: (5<<4)|0x08 = 0x58
tester.check("build 自定义 MUX=0x58", tx_log[4].tx[3] == 0x58)
-- ADCON: pga=3
tester.check("build 自定义 ADCON=3", tx_log[5].tx[3] == 3)
-- DRATE: drate=0 → 0xF0 (30000SPS)
tester.check("build 自定义 DRATE=0xF0", tx_log[6].tx[3] == 0xF0)

-- 5. build drate 越界（>14）应归缺省 5
reset()
res = build({ fd = 7, delay = 0, drate = 15, pga = 0, channel = 0 })
tester.check("build drate=15 返回 OK", tostring(res.result) == "OK")
tester.check("build drate 越界归缺省 5 (DRATE=0xA0)", tx_log[6].tx[3] == 0xA0)

-- 6. build pga 越界（>6）应归缺省 0
reset()
res = build({ fd = 7, delay = 0, drate = 5, pga = 7, channel = 0 })
tester.check("build pga=7 返回 OK", tostring(res.result) == "OK")
tester.check("build pga 越界归缺省 0", tx_log[5].tx[3] == 0)

-- 7. build channel 越界（>7）应归缺省 0
reset()
res = build({ fd = 7, delay = 0, drate = 5, pga = 0, channel = 8 })
tester.check("build channel=8 返回 OK", tostring(res.result) == "OK")
tester.check("build channel 越界归缺省 0", tx_log[4].tx[3] == 0x08)

-- 8. SPI 传输失败（RESET 失败）
reset()
mock_fail.spi = true
res = build({ fd = 7, delay = 0 })
tester.check("SPI 传输失败 build 返回 ERR", tostring(res.result) == "ERR", res.message)

-- ==================== destroy 测试 ====================
-- 9. 正常 destroy
reset()
res = destroy({ fd = 7 })
tester.check("destroy 返回 OK", tostring(res.result) == "OK")
tester.check("destroy 发出 SDATAC 命令", tx_log[1].tx[1] == CMD_SDATAC)

-- 10. destroy 缺 fd
reset()
res = destroy({ })
tester.check("destroy 缺 fd 返回 ERR", tostring(res.result) == "ERR", res.message)

-- 11. destroy 非表参数
reset()
res = destroy("bad")
tester.check("destroy 非表返回 ERR", tostring(res.result) == "ERR", res.message)

-- 12. destroy SPI 失败
reset()
mock_fail.spi = true
res = destroy({ fd = 7 })
tester.check("destroy SPI 失败返回 ERR", tostring(res.result) == "ERR", res.message)

-- ==================== call 测试 ====================
-- 13. call 单端默认（channel=0）
reset()
-- 预置 4 字节 SPI 读返回：24 位 ADC 值 = 0x7FFFFF (满量程正) + 1 字节丢弃
-- big-endian: 0x7F, 0xFF, 0xFF, 0x00
rx_data = { 0x7F, 0xFF, 0xFF, 0x00 }
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, channel = 0 })
tester.check("call 单端返回 OK", tostring(res.result) == "OK", res.message)
tester.check("call raw=8388607 (0x7FFFFF)", res.raw == 8388607)
-- voltage = 8388607 * 2 * 2.5 / 8388608.0 / 1 ≈ 4.999999...
tester.check("call 电压约 5.0V", math.abs(res.voltage - 5.0) < 0.01)

-- 14. call 差分（pos=0, neg=1）
reset()
rx_data = { 0x00, 0x00, 0x80, 0x00 }  -- raw24 = 0x000080 = 128 → 符号扩展正
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, pos = 0, neg = 1 })
tester.check("call 差分返回 OK", tostring(res.result) == "OK")
-- MUX = (0<<4)|1 = 0x01
tester.check("call 差分 MUX=0x01", tx_log[2].tx[3] == 0x01)

-- 15. call 负值 ADC（24 位补码）
reset()
-- raw24 = 0x800000 → -8388608 (最小负值)
-- bytes: 0x80, 0x00, 0x00, 0x00
rx_data = { 0x80, 0x00, 0x00, 0x00 }
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, channel = 0 })
tester.check("call 负值 raw=-8388608", res.raw == -8388608)
-- voltage = -8388608 * 2 * 2.5 / 8388608.0 = -5.0
tester.check("call 负值电压约 -5.0V", math.abs(res.voltage - (-5.0)) < 0.01)

-- 16. call PGA=2 (×4) 电压换算
reset()
rx_data = { 0x40, 0x00, 0x00, 0x00 }  -- raw24 = 0x400000 = 4194304
res = call({ fd = 7, delay = 0, drate = 5, pga = 2, channel = 0 })
tester.check("call PGA=2 返回 OK", tostring(res.result) == "OK")
-- pga_idx=2 → gain=4; voltage = 4194304 * 2 * 2.5 / 8388608.0 / 4 = 0.625
tester.check("call PGA=2 电压约 0.625V", math.abs(res.voltage - 0.625) < 0.01)

-- 17. call 缺 fd
reset()
res = call({ delay = 0, drate = 5, pga = 0, channel = 0 })
tester.check("call 缺 fd 返回 ERR", tostring(res.result) == "ERR", res.message)

-- 18. call 差分 pos/neg 非数字（占位符 "-"）
reset()
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, pos = "-", neg = "1" })
tester.check("call pos 非数字返回 ERR", tostring(res.result) == "ERR", res.message)

-- 19. call 差分 pos 越界
reset()
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, pos = 8, neg = 0 })
tester.check("call pos=8 返回 ERR", tostring(res.result) == "ERR", res.message)

-- 20. call 单端 channel 越界
reset()
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, channel = 9 })
tester.check("call channel=9 返回 ERR", tostring(res.result) == "ERR", res.message)

-- 21. call SPI 读失败
reset()
mock_fail.spi = true
res = call({ fd = 7, delay = 0, drate = 5, pga = 0, channel = 0 })
tester.check("call SPI 失败返回 ERR", tostring(res.result) == "ERR", res.message)

-- 22. call 非表参数
reset()
res = call("bad")
tester.check("call 非表返回 ERR", tostring(res.result) == "ERR", res.message)

-- ==================== summary ====================
tester.summary("test_ads1256")
return 0
