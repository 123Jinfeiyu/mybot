-- bus_contract.lua - 总线契约单点（modules/lib/，HAL 与 virtbus 共用）
--
-- N-25：HAL.lua（真机路径）与 virtbus.lua（虚拟总线）此前**逐字手工镜像**
-- 同一组契约：参数归一化、三条总线的设备路径白名单、UART 波特率白名单、
-- 读数/写数长度上限、合并读的 2^53 精度上限。镜像一旦漂移，后果是静默的：
-- 虚拟总线接受（或拒绝）真机行为相反的调用，表现为「仿真绿真机红」且无报错。
-- 故收敛到此单点，两库共同 dofile。
--
-- 本文件为纯 Lua（不依赖 ffi / HAL / 沙箱外的库），可在任意状态机与任意顺序
-- 被加载；可被 lua5.4 独立调试。
--
-- 约定：函数只做「判定/归一化」，不做打印（打印口径属各库自己的错误文案）。

-- 重入口守卫：同一状态机内复用同一张表（纯常量，重复执行无副作用，
-- 但保证身份一致便于测试断言）
if __BOT4CJ_BUS_CONTRACT ~= nil then return __BOT4CJ_BUS_CONTRACT end

local M = {}
M._VERSION = "0.1.0"

------------------------------------------------------------------
-- 参数归一化
------------------------------------------------------------------

-- 归一化为 0~255 的整数，非法返回 nil
function M.byte_of(v)
	local n = tonumber(v)
	if n == nil then return nil end
	n = math.floor(n)
	if n < 0 or n > 255 then return nil end
	return n
end

-- 归一化为非负整数，非法返回 nil
function M.uint_of(v)
	local n = tonumber(v)
	if n == nil then return nil end
	n = math.floor(n)
	if n < 0 then return nil end
	return n
end

------------------------------------------------------------------
-- 设备路径白名单（Lua pattern）
------------------------------------------------------------------
M.PATH_I2C = "^/dev/i2c%-%d+$"				-- /dev/i2c-<N>
M.PATH_SPI = "^/dev/spidev%d+%.%d+$"			-- /dev/spidev<bus>.<cs>
M.PATH_UART_TTY = "^/dev/tty[A-Za-z0-9]+$"		-- /dev/ttyUSB0、/dev/ttyAMA0…
M.PATH_UART_SERIAL = "^/dev/serial%d+$"			-- /dev/serial0（树莓派软链惯例）

function M.i2c_path_ok(path)
	return type(path) == "string" and string.match(path, M.PATH_I2C) ~= nil
end

function M.spi_path_ok(path)
	return type(path) == "string" and string.match(path, M.PATH_SPI) ~= nil
end

function M.uart_path_ok(path)
	if type(path) ~= "string" then return false end
	return string.match(path, M.PATH_UART_TTY) ~= nil
		or string.match(path, M.PATH_UART_SERIAL) ~= nil
end

------------------------------------------------------------------
-- UART 波特率白名单（含 500k/1M）
------------------------------------------------------------------
M.UART_BAUDS = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 921600, 1000000 }

-- baud ∈ 白名单（入参先 tonumber，与 HAL 旧行为一致）
function M.baud_ok(baud)
	baud = tonumber(baud)
	for _, b in ipairs(M.UART_BAUDS) do
		if b == baud then return true end
	end
	return false
end

------------------------------------------------------------------
-- 长度上限
------------------------------------------------------------------

-- 合并读的长度上限：HAL 把 n 字节按大端拼成一个 double，7 字节起超 2^53
-- 精度（静默截断成错误的合并值），故三总线（I2C/SPI 寄存器族）一律限 1~6
M.MERGED_READ_MAX = 6

-- 字节表读（i2c_read_bytes / i2c_read_multi_bytes）的 n 上限：钳到 1~16
-- （返回的是字节表、长度对调用方可见，故为钳制而非拒绝）
M.BYTES_READ_MAX = 16

-- uart_write 单笔上限 / uart_read 单次上限（超出：写拒绝、读钳制）
M.UART_IO_MAX_BYTES = 512

-- spi_write / spi_read 单笔上限（spidev 逐字节缓冲区，超过即拒绝）
M.SPI_IO_MAX_BYTES = 64

-- uart_read 超时上限（ms）：iters = timeout_ms + 1 按 1ms 空转，配置笔误
-- （如少写一个单位）必须立即失败而非阻塞数小时
M.UART_READ_MAX_MS = 60000

__BOT4CJ_BUS_CONTRACT = M
return M
