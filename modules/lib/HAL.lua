-- HAL.lua - 基层硬件抽象层（LuaJIT ffi，位于 modules/lib/）
-- bot4cj 仓颉语言机器人框架
--
-- issue #14：完全解耦的基层硬件应用层驱动——不再依赖仓颉注册的 cfunc。
--
-- 【运行环境】仅支持 LuaJIT：经 ffi 直调 glibc（open/ioctl/read/write/close/usleep），
-- 原生性能，fd 为真实内核句柄，跨 Lua 状态机天然有效。
-- （原 i2c-tools/spi-tools shell-out 降级后端 HAL_shell.lua 已移除。）
--
-- 【安全】设备路径经白名单模式校验；所有数值参数 tonumber + 范围校验后才进入
-- ioctl/缓冲区，无 shell 参与。
--
-- 【可测试性】系统调用统一走 HAL.sys.* 间接层，离线单测（test/test_hal_ffi.lua）
-- 注入 mock 即可验证调用序列与缓冲区内容。
--
-- 【delay 说明】delay_usecs 是逐次传输参数，内核无 per-fd 持久化 ioctl；
-- 需要延时的驱动经传输函数的 delay_us 尾参显式传入（参考 ADS1256.lua：
-- delay 配置在 @SPI 矩阵，由 SPI.lua 的 call() 随 fd 一并返回，
-- 经元数据引用机制流入设备驱动，驱动统一附带在每次传输上）。

-- 重复加载保护：同一状态机内只初始化一次
if _G.__BOT4CJ_HAL ~= nil then return _G.__BOT4CJ_HAL end

-- 环境检查：必须有 ffi（LuaJIT）；shell 降级后端已移除
local has_ffi, ffi = false, nil
if rawget(_G, "jit") ~= nil then
	has_ffi, ffi = pcall(require, "ffi")
end
if not has_ffi then
	error("HAL requires LuaJIT with ffi (the shell-out fallback backend has been removed)")
end

------------------------------------------------------------------
-- ffi 声明（glibc + Linux spidev ABI）
------------------------------------------------------------------
ffi.cdef[[
int  open(const char *pathname, int flags, ...);
int  close(int fd);
int  ioctl(int fd, unsigned long request, ...);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
int  usleep(unsigned int usec);

/* /usr/include/linux/spi/spidev.h（内核 6.18），共 32 字节 */
typedef struct {
	unsigned long long tx_buf;
	unsigned long long rx_buf;
	unsigned int       len;
	unsigned int       speed_hz;
	unsigned short     delay_usecs;
	unsigned char      bits_per_word;
	unsigned char      cs_change;
	unsigned char      tx_nbits;
	unsigned char      rx_nbits;
	unsigned char      word_delay_usecs;
	unsigned char      pad;
} spi_ioc_transfer;

/* /usr/include/linux/i2c.h：I2C_RDWR 原子事务（写指针+读合并为单次内核传输） */
typedef struct {
	unsigned short  addr;
	unsigned short  flags;
	unsigned short  len;
	unsigned char  *buf;
} cj_i2c_msg;
typedef struct {
	cj_i2c_msg   *msgs;
	unsigned int  nmsgs;
} cj_i2c_rdwr;
]]

-- 布局自检：结构体尺寸与内核头文件不符则立即失败（防止静默内存越界）
assert(ffi.sizeof("spi_ioc_transfer") == 32,
	"spi_ioc_transfer size mismatch: expect 32, got " .. ffi.sizeof("spi_ioc_transfer"))
assert(ffi.sizeof("cj_i2c_msg") == 16,
	"cj_i2c_msg size mismatch: expect 16, got " .. ffi.sizeof("cj_i2c_msg"))

local HAL = {}
_G.__BOT4CJ_HAL = HAL
HAL.backend = "ffi"

------------------------------------------------------------------
-- 常量
------------------------------------------------------------------
local O_RDWR = 2
HAL.I2C_RDWR = 0x0707		-- i2c-dev 原子多消息事务
HAL.I2C_M_RD = 0x0001		-- i2c_msg.flags：读（0 为写）
HAL.i2c_addr = {}		-- fd -> 7位从地址（per-state；每条 i2c_msg 自带地址，无需 I2C_SLAVE ioctl）

-- ioctl 编号按 _IOC(dir=WRITE(1)<<30 | size<<16 | type('k'=0x6b)<<8 | nr) 计算
HAL.SPI_IOC_WR_MODE          = 0x40016b01	-- _IOW('k', 1, __u8)
HAL.SPI_IOC_WR_BITS_PER_WORD = 0x40016b03	-- _IOW('k', 3, __u8)
HAL.SPI_IOC_WR_MAX_SPEED_HZ  = 0x40046b04	-- _IOW('k', 4, __u32)
HAL.SPI_IOC_MESSAGE_1        = 0x40206b00	-- _IOW('k', 0, spi_ioc_transfer[1])

------------------------------------------------------------------
-- 系统调用间接层（离线单测在此注入 mock）
------------------------------------------------------------------
HAL.sys = {
	open    = function(path, flags) return ffi.C.open(path, flags) end,
	close   = function(fd)          return ffi.C.close(fd) end,
	-- 变参必须显式 cast：JIT 编译后 Lua number 会以 double 位模式传入变参
	-- （ioctl 收到 0x4050... > 0x7f → EINVAL），解释执行却传 int——
	-- cast 为 unsigned long cdata 后两种执行路径 ABI 一致（第 58 次调用起复现）
	ioctl_i = function(fd, req, val)
		return ffi.C.ioctl(fd, req, ffi.cast("unsigned long", val))
	end,
	ioctl_p = function(fd, req, ptr) return ffi.C.ioctl(fd, req, ptr) end,
	read    = function(fd, buf, n)  return tonumber(ffi.C.read(fd, buf, n)) end,
	write   = function(fd, buf, n)  return tonumber(ffi.C.write(fd, buf, n)) end,
	usleep  = function(us)          return ffi.C.usleep(us) end,
	errno   = function()            return ffi.errno() end,
}

------------------------------------------------------------------
-- 参数归一化
------------------------------------------------------------------
-- 归一化为 0~255 的整数，非法返回 nil
local function byte_of(v)
	local n = tonumber(v)
	if n == nil then return nil end
	n = math.floor(n)
	if n < 0 or n > 255 then return nil end
	return n
end

-- 归一化为非负整数，非法返回 nil
local function uint_of(v)
	local n = tonumber(v)
	if n == nil then return nil end
	n = math.floor(n)
	if n < 0 then return nil end
	return n
end

------------------------------------------------------------------
-- I2C（fd 为真实内核句柄，跨 Lua 状态机有效）
------------------------------------------------------------------

-- 打开 I2C 总线，成功返回 fd（>=0），失败返回 -1
function i2c_open(path)
	if type(path) ~= "string" then return -1 end
	if string.match(path, "^/dev/i2c%-%d+$") == nil then
		print("HAL: invalid i2c path: " .. tostring(path))
		return -1
	end
	local fd = tonumber(HAL.sys.open(path, O_RDWR))
	if fd == nil or fd < 0 then
		print("HAL: failed to open i2c bus: " .. path)
		return -1
	end
	return fd
end

function i2c_close(fd)
	fd = tonumber(fd)
	if fd ~= nil and fd >= 0 then
		HAL.sys.close(fd)
		HAL.i2c_addr[fd] = nil
	end
end

-- 设置从设备地址：仅记录到 per-fd 地址表。
-- I2C_RDWR 每条消息自带地址，无需 I2C_SLAVE ioctl，也就没有 EBUSY/地址被占用问题。
function i2c_set_addr(fd, addr)
	fd = tonumber(fd)
	addr = uint_of(addr)
	if fd == nil or addr == nil or addr > 0x7F then
		print("HAL: i2c_set_addr invalid args: " .. tostring(fd) .. ", " .. tostring(addr))
		return false
	end
	HAL.i2c_addr[fd] = addr
	return true
end

-- 内部：执行一次 I2C_RDWR 原子事务（msgs 为 cj_i2c_msg[n]），返回内核传输数或负值
local function i2c_rdwr(fd, msgs, nmsgs)
	local data = ffi.new("cj_i2c_rdwr[1]")
	data[0].msgs = msgs
	data[0].nmsgs = nmsgs
	return HAL.sys.ioctl_p(fd, HAL.I2C_RDWR, data)
end

-- 内部：读缓冲区 n 字节按大端合并
local function be_merge(buf, n)
	local v = 0
	for i = 0, n - 1 do v = v * 256 + tonumber(buf[i]) end
	return v
end

-- 写寄存器（单条写消息 {reg,val}，原子），成功返回 true
function i2c_write_reg(fd, reg, val)
	fd = tonumber(fd)
	reg, val = byte_of(reg), byte_of(val)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg == nil or val == nil or addr == nil then return false end
	local buf = ffi.new("uint8_t[2]", reg, val)
	local msgs = ffi.new("cj_i2c_msg[1]")
	msgs[0].addr = addr; msgs[0].flags = 0; msgs[0].len = 2; msgs[0].buf = buf
	return i2c_rdwr(fd, msgs, 1) >= 0
end

-- 读寄存器（写指针 + 读 合并为单次原子事务），返回 0~255；失败返回 -1
function i2c_read_reg(fd, reg)
	fd = tonumber(fd)
	reg = byte_of(reg)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg == nil or addr == nil then return -1 end
	local wb = ffi.new("uint8_t[1]", reg)
	local rb = ffi.new("uint8_t[1]")
	local msgs = ffi.new("cj_i2c_msg[2]")
	msgs[0].addr = addr; msgs[0].flags = 0;            msgs[0].len = 1; msgs[0].buf = wb
	msgs[1].addr = addr; msgs[1].flags = HAL.I2C_M_RD; msgs[1].len = 1; msgs[1].buf = rb
	if i2c_rdwr(fd, msgs, 2) < 0 then return -1 end
	return tonumber(rb[0])
end

-- 写寄存器+多字节数据（单条写消息，原子），返回 0 = 成功，-1 = 失败
function i2c_write_multi(fd, reg, ...)
	fd = tonumber(fd)
	reg = byte_of(reg)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg == nil or addr == nil then return -1 end
	local data = { reg }
	for _, v in ipairs({ ... }) do
		local b = byte_of(v)
		if b == nil then return -1 end
		data[#data + 1] = b
	end
	local buf = ffi.new("uint8_t[?]", #data)
	for i, b in ipairs(data) do buf[i - 1] = b end
	local msgs = ffi.new("cj_i2c_msg[1]")
	msgs[0].addr = addr; msgs[0].flags = 0; msgs[0].len = #data; msgs[0].buf = buf
	if i2c_rdwr(fd, msgs, 1) >= 0 then return 0 end
	return -1
end

-- 写指针后读 n 字节（合并为单次原子事务），返回大端合并值；失败返回 -1（n 钳制 1~8）
function i2c_read_multi(fd, reg, n)
	fd = tonumber(fd)
	reg = byte_of(reg)
	n = uint_of(n)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg == nil or n == nil or addr == nil then return -1 end
	if n < 1 or n > 8 then n = 1 end
	local wb = ffi.new("uint8_t[1]", reg)
	local rb = ffi.new("uint8_t[?]", n)
	local msgs = ffi.new("cj_i2c_msg[2]")
	msgs[0].addr = addr; msgs[0].flags = 0;            msgs[0].len = 1; msgs[0].buf = wb
	msgs[1].addr = addr; msgs[1].flags = HAL.I2C_M_RD; msgs[1].len = n; msgs[1].buf = rb
	if i2c_rdwr(fd, msgs, 2) < 0 then return -1 end
	return be_merge(rb, n)
end

-- 纯读 n 字节（单条读消息，不写指针，SGP30 这类命令式传感器用），大端合并；失败返回 -1
function i2c_read(fd, n)
	fd = tonumber(fd)
	n = uint_of(n)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or n == nil or addr == nil then return -1 end
	if n < 1 or n > 8 then n = 1 end
	local rb = ffi.new("uint8_t[?]", n)
	local msgs = ffi.new("cj_i2c_msg[1]")
	msgs[0].addr = addr; msgs[0].flags = HAL.I2C_M_RD; msgs[0].len = n; msgs[0].buf = rb
	if i2c_rdwr(fd, msgs, 1) < 0 then return -1 end
	return be_merge(rb, n)
end

------------------------------------------------------------------
-- 延时
------------------------------------------------------------------
-- 毫秒级延时（usleep，零 fork）
function milli_sleep(ms)
	ms = tonumber(ms)
	if ms == nil or ms <= 0 then return end
	HAL.sys.usleep(math.floor(ms * 1000))
end

------------------------------------------------------------------
-- SPI（spidev ioctl；fd 为真实内核句柄）
------------------------------------------------------------------

-- 打开 SPI 设备，成功返回 fd（>=0），失败返回 -1
function spi_open(path)
	if type(path) ~= "string" then return -1 end
	if string.match(path, "^/dev/spidev%d+%.%d+$") == nil then
		print("HAL: invalid spi path: " .. tostring(path))
		return -1
	end
	local fd = tonumber(HAL.sys.open(path, O_RDWR))
	if fd == nil or fd < 0 then
		print("HAL: failed to open spi device: " .. path)
		return -1
	end
	return fd
end

function spi_close(fd)
	fd = tonumber(fd)
	if fd ~= nil and fd >= 0 then HAL.sys.close(fd) end
end

-- 配置 SPI 设备，返回 0 = 成功，-1 = 失败
-- mode/bits/speed 经 ioctl 持久化到 fd（跨状态机有效）；
-- delay 无 per-fd 持久化途径：请由驱动在每次传输时经 delay_us 尾参显式传入
-- （spi_write/spi_read/spi_write3/spi_read_multi 均支持）。
function spi_setup(fd, mode, bits, speed, delay)
	fd = tonumber(fd)
	mode, bits, speed = uint_of(mode), uint_of(bits), uint_of(speed)
	if fd == nil or mode == nil or mode > 3
	   or bits == nil or bits < 7 or bits > 16
	   or speed == nil or speed <= 0 then
		print("HAL: spi_setup invalid args: mode=" .. tostring(mode) ..
		      " bits=" .. tostring(bits) .. " speed=" .. tostring(speed))
		return -1
	end
	if tonumber(delay) ~= nil and tonumber(delay) > 0 then
		print("HAL: spi_setup delay cannot persist on fd; pass delay_us per transfer instead")
	end

	local m8 = ffi.new("uint8_t[1]", mode)
	if HAL.sys.ioctl_p(fd, HAL.SPI_IOC_WR_MODE, m8) ~= 0 then return -1 end
	local b8 = ffi.new("uint8_t[1]", bits)
	if HAL.sys.ioctl_p(fd, HAL.SPI_IOC_WR_BITS_PER_WORD, b8) ~= 0 then return -1 end
	local s32 = ffi.new("uint32_t[1]", speed)
	if HAL.sys.ioctl_p(fd, HAL.SPI_IOC_WR_MAX_SPEED_HZ, s32) ~= 0 then return -1 end
	return 0
end

-- 内部：单次 SPI 事务（一条 message，CS 保持低）。
-- tx/rx 为 uint8_t 缓冲区 cdata（nil 表示该方向不使用）；len 为字节数。
-- speed_hz/bits_per_word 填 0：内核自动沿用 fd 上 spi_setup 配置的值。
-- delay_us：可选，本次传输最后一位之后、CS 拉高之前的等待微秒数
-- （内核无 per-fd 持久化途径，由驱动逐次显式传入，__u16 钳制到 65535）。
local function spi_xfer(fd, tx, rx, len, delay_us)
	local msg = ffi.new("spi_ioc_transfer[1]")	-- ffi.new 零初始化
	msg[0].len = len
	delay_us = uint_of(delay_us)
	if delay_us ~= nil then
		if delay_us > 65535 then delay_us = 65535 end
		msg[0].delay_usecs = delay_us
	end
	if tx ~= nil then
		msg[0].tx_buf = ffi.cast("unsigned long long", ffi.cast("const uint8_t *", tx))
	end
	if rx ~= nil then
		msg[0].rx_buf = ffi.cast("unsigned long long", ffi.cast("uint8_t *", rx))
	end
	local r = HAL.sys.ioctl_p(fd, HAL.SPI_IOC_MESSAGE_1, msg)
	return r ~= nil and r >= 0
end

-- 写 1 字节，返回 0 = 成功，-1 = 失败；delay_us 可选（CS 拉高前等待）
function spi_write(fd, val, delay_us)
	fd = tonumber(fd)
	val = byte_of(val)
	if fd == nil or val == nil then return -1 end
	local buf = ffi.new("uint8_t[1]", val)
	if spi_xfer(fd, buf, nil, 1, delay_us) then return 0 end
	return -1
end

-- 读 1 字节（发一个 dummy 字节），返回 0~255；失败返回 -1；delay_us 可选
function spi_read(fd, delay_us)
	fd = tonumber(fd)
	if fd == nil then return -1 end
	local buf = ffi.new("uint8_t[1]")
	if not spi_xfer(fd, nil, buf, 1, delay_us) then return -1 end
	return tonumber(buf[0])
end

-- 一次写 3 字节（单次传输，CS 保持低），返回 0 = 成功，-1 = 失败；delay_us 可选
function spi_write3(fd, b0, b1, b2, delay_us)
	fd = tonumber(fd)
	b0, b1, b2 = byte_of(b0), byte_of(b1), byte_of(b2)
	if fd == nil or b0 == nil or b1 == nil or b2 == nil then return -1 end
	local buf = ffi.new("uint8_t[3]", b0, b1, b2)
	if spi_xfer(fd, buf, nil, 3, delay_us) then return 0 end
	return -1
end

-- 一次读 n 字节（单次传输），返回大端合并值；失败返回 -1（n 钳制 1~8）；delay_us 可选
function spi_read_multi(fd, n, delay_us)
	fd = tonumber(fd)
	n = uint_of(n)
	if fd == nil or n == nil then return -1 end
	if n < 1 or n > 8 then n = 1 end
	local buf = ffi.new("uint8_t[?]", n)
	if not spi_xfer(fd, nil, buf, n, delay_us) then return -1 end
	local v = 0
	for i = 0, n - 1 do
		v = v * 256 + tonumber(buf[i])
	end
	return v
end

return HAL
