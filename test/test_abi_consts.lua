-- requires: cc
-- HAL 硬编码常量的内核头文件交叉校验（需 C 编译器）
-- 用法（项目根目录）: luajit test/test_abi_consts.lua
--
-- 背景：HAL.lua 以字面量硬编码全部 ioctl 编号、struct termios 布局与 errno
-- （ffi 无法 #include 头文件）。此前唯一防线是 test/test_ioctl_nums.c 与
-- test/test_uart_consts.c 两个【只打印不判断、main 恒返回 0】的程序——编号漂移
-- 时它们照样绿灯，且从未被 test/run_all.sh 调用。本测试把两者接起来：
-- C 侧编译运行取内核/libc 头文件的事实，Lua 侧读 HAL.lua 的常量逐项比对。
--
-- 校验两类：① HAL.* 常量 == 系统头文件值；② HAL 的 ffi cdef 结构尺寸
-- == 头文件结构尺寸（cdef 是人手抄的，最易与内核脱节）。

local T = dofile("test/lib/tester.lua")
local check = T.check
local ffi = require("ffi")

-- os.execute 返回值在 5.1 语义下是退出码数字（0 为真值，不能直接 not ok），
-- 5.2 语义下成功返回 true——两种都归一成布尔。
local function sh_ok(cmd)
	local r = os.execute(cmd)
	return r == true or r == 0
end

if not sh_ok("command -v cc >/dev/null 2>&1") then
	print("[FAIL] 无 C 编译器 cc，无法交叉校验（run_all.sh 会按 -- requires: cc 跳过本用例）")
	os.exit(1)
end

local HAL = dofile("modules/lib/HAL.lua")
check("HAL ffi 后端可用", HAL.backend == "ffi", HAL.backend)

------------------------------------------------------------------
-- 编译并运行两个事实程序，解析 `key = value` 行
------------------------------------------------------------------
local base = os.tmpname()
os.remove(base)

local function build_and_run(src, tag)
	local bin = base .. "_" .. tag
	if not sh_ok(string.format("cc -Wall -o '%s' '%s'", bin, src)) then
		return nil, "编译失败：" .. src
	end
	local h = io.popen("'" .. bin .. "'", "r")
	if h == nil then return nil, "无法运行：" .. bin end
	local facts = {}
	for line in h:lines() do
		local k, v = line:match("^([%w_]+) = (%S+)$")
		if k then facts[k] = tonumber(v, v:sub(1, 2) == "0x" and 16 or 10) end
	end
	h:close()
	os.remove(bin)
	return facts
end

local dev, dev_err = build_and_run("test/test_ioctl_nums.c", "dev")
local uart, uart_err = build_and_run("test/test_uart_consts.c", "uart")
check("test_ioctl_nums.c 编译运行成功", dev ~= nil, dev_err)
check("test_uart_consts.c 编译运行成功", uart ~= nil, uart_err)
if dev == nil or uart == nil then T.summary("abi_consts") os.exit(1) end

------------------------------------------------------------------
-- ① HAL.* 常量 vs 系统头文件
------------------------------------------------------------------
local scalar = {
	{ "I2C_RDWR",                        dev.I2C_RDWR,                        HAL.I2C_RDWR },
	{ "I2C_M_RD",                        dev.I2C_M_RD,                        HAL.I2C_M_RD },
	{ "SPI_IOC_WR_MODE",                 dev.SPI_IOC_WR_MODE,                 HAL.SPI_IOC_WR_MODE },
	{ "SPI_IOC_WR_BITS_PER_WORD",        dev.SPI_IOC_WR_BITS_PER_WORD,        HAL.SPI_IOC_WR_BITS_PER_WORD },
	{ "SPI_IOC_WR_MAX_SPEED_HZ",         dev.SPI_IOC_WR_MAX_SPEED_HZ,         HAL.SPI_IOC_WR_MAX_SPEED_HZ },
	{ "SPI_IOC_MESSAGE_1",               dev.SPI_IOC_MESSAGE_1,               HAL.SPI_IOC_MESSAGE_1 },
	{ "GPIO_GET_LINEHANDLE",             dev.GPIO_GET_LINEHANDLE_IOCTL,       HAL.GPIO_GET_LINEHANDLE },
	{ "GPIOHANDLE_GET_LINE_VALUES",      dev.GPIOHANDLE_GET_LINE_VALUES_IOCTL, HAL.GPIOHANDLE_GET_LINE_VALUES },
	{ "GPIOHANDLE_SET_LINE_VALUES",      dev.GPIOHANDLE_SET_LINE_VALUES_IOCTL, HAL.GPIOHANDLE_SET_LINE_VALUES },
	{ "UART_TCSANOW",                    uart.TCSANOW,                        HAL.UART_TCSANOW },
	{ "UART_TCIFLUSH",                   uart.TCIFLUSH,                       HAL.UART_TCIFLUSH },
	{ "UART_VMIN_IDX",                   uart.VMIN,                           HAL.UART_VMIN_IDX },
	{ "UART_VTIME_IDX",                  uart.VTIME,                          HAL.UART_VTIME_IDX },
	{ "EAGAIN",                          uart.EAGAIN,                         HAL.EAGAIN },
	{ "EINTR",                           uart.EINTR,                          HAL.EINTR },
	{ "ENOENT",                          uart.ENOENT,                         HAL.ENOENT },
	{ "EACCES",                          uart.EACCES,                         HAL.EACCES },
	{ "EINVAL",                          uart.EINVAL,                         HAL.EINVAL },
}
for _, c in ipairs(scalar) do
	check("HAL." .. c[1] .. " == 头文件值 " .. tostring(c[3]),
		c[2] == c[3], "头文件 " .. tostring(c[2]) .. " vs HAL " .. tostring(c[3]))
end

-- 组合常量：位或结果（这些标志位互不重叠，求和等价）
check("HAL.UART_O_FLAGS == O_RDWR|O_NOCTTY|O_NONBLOCK",
	uart.O_RDWR + uart.O_NOCTTY + uart.O_NONBLOCK == HAL.UART_O_FLAGS,
	tostring(uart.O_RDWR + uart.O_NOCTTY + uart.O_NONBLOCK) .. " vs " .. tostring(HAL.UART_O_FLAGS))
check("HAL.UART_CREAD_CLOCAL == CREAD|CLOCAL",
	uart.CREAD + uart.CLOCAL == HAL.UART_CREAD_CLOCAL,
	tostring(uart.CREAD + uart.CLOCAL) .. " vs " .. tostring(HAL.UART_CREAD_CLOCAL))

-- 波特率映射整表校验：HAL.UART_BAUD_SPT 每项须等于 <termios.h> 的 B* 常量。
-- （cfsetispeed 收 speed_t 而非 Hz，传 Hz 会被 glibc 拒为 EINVAL——
--   这张表就是 uart_open 的换算依据，错一档即串口停在错误速率上。）
local spt = HAL.UART_BAUD_SPT
check("HAL 导出 UART_BAUD_SPT 表", type(spt) == "table")
local n_bauds = 0
for baud, speed in pairs(spt) do
	n_bauds = n_bauds + 1
	local real = uart["B" .. tostring(baud)]
	check("UART_BAUD_SPT[" .. baud .. "] == B" .. baud .. " (" .. tostring(real) .. ")",
		real == speed, "头文件 " .. tostring(real) .. " vs HAL " .. tostring(speed))
end
check("映射表覆盖 11 档波特率", n_bauds == 11, tostring(n_bauds))
for _, b in ipairs({ 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 }) do
	check("支持波特率 " .. b .. " 在映射表内", spt[b] ~= nil)
end

------------------------------------------------------------------
-- ② HAL 的 ffi cdef 结构 vs 头文件结构（人手抄写，最易脱节）
------------------------------------------------------------------
local structs = {
	{ "spi_ioc_transfer",           dev.sizeof_spi_ioc_transfer,         ffi.sizeof("spi_ioc_transfer") },
	{ "cj_i2c_msg",                 dev.sizeof_i2c_msg,                  ffi.sizeof("cj_i2c_msg") },
	{ "cj_i2c_rdwr",                dev.sizeof_i2c_rdwr_ioctl_data,      ffi.sizeof("cj_i2c_rdwr") },
	{ "cj_gpiohandle_request",      dev.sizeof_gpiohandle_request,       ffi.sizeof("cj_gpiohandle_request") },
	{ "cj_gpiohandle_data",         dev.sizeof_gpiohandle_data,          ffi.sizeof("cj_gpiohandle_data") },
	{ "cj_termios",                 uart.sizeof_termios,                 ffi.sizeof("cj_termios") },
}
for _, c in ipairs(structs) do
	check("sizeof " .. c[1] .. " == 头文件 " .. tostring(c[2]),
		c[2] == c[3], "头文件 " .. tostring(c[2]) .. " vs cdef " .. tostring(c[3]))
end

-- termios 字段偏移：HAL 按 c_line/c_cc 偏移手工切分缓冲区，脱节即越界写
check("offsetof(cj_termios.c_cc) == 头文件 " .. tostring(uart.offsetof_c_cc),
	uart.offsetof_c_cc == ffi.offsetof("cj_termios", "c_cc"))
check("offsetof(cj_termios.c_line) == 头文件 " .. tostring(uart.offsetof_c_line),
	uart.offsetof_c_line == ffi.offsetof("cj_termios", "c_line"))
check("c_cc 元素数 NCCS == HAL 声明的 32", uart.NCCS == 32, tostring(uart.NCCS))

-- 守护共享槽 mutex 预留位：pthread.lua 预留 64B，真实 pthread_mutex_t 超出即踩到 count
check("sizeof(pthread_mutex_t) <= 槽内 mutex 预留 64B",
	dev.sizeof_pthread_mutex_t <= 64, "本机 " .. tostring(dev.sizeof_pthread_mutex_t) .. "B")

T.summary("abi_consts")
