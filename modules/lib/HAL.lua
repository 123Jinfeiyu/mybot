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

-- 平台差异单点（阶段 A-2）：设备根路径常量集中一处——移植 / 仿真桩改这里，
-- GPIO / PWM 各节只取表值，不再散落字面量（i2c/spi/uart 路径白名单在
-- bus_contract.lua，同为单点）。
HAL.platform = {
	pwm_sysfs_root = "/sys/class/pwm",
	gpio_prefix    = "/dev/gpiochip",
}
HAL.PWM_SYSFS_ROOT = HAL.platform.pwm_sysfs_root	-- 兼容既有引用（pwm_attr_path 等）
HAL.backend = "ffi"			-- ABI 类标记（ffi=真实句柄后端；test_hal_ffi/test_jit_warmup 按此分流）
HAL.backend_name = "glibc"	-- 后端模块名（BOT4CJ_HAL_BACKEND 指定，见下方加载段）

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
-- 阶段 A-1 后端接口化：本表由 BOT4CJ_HAL_BACKEND 指定的后端提供
--（缺省 glibc，modules/lib/hal_backend_<名>.lua）。接口契约见
-- hal_backend_glibc.lua 头注（open/close/ioctl_i/ioctl_p/read/write/usleep/
-- errno/version + UART termios 六件），加载后逐项断言、缺函数 fail-fast——
-- 本文件是全部驱动的地基，契约不完整必须在加载期暴露而非首笔调用时炸。
-- 定位方式与 bus_contract 相同：BOT4CJ_MODULE_PATH 各段逐段探测，再以
-- 仓库缺省布局兜底。离线单测仍按函数粒度替换 HAL.sys.* 表项注入 mock，
-- 用法与后端化之前完全一致。
do
	local wanted = os.getenv("BOT4CJ_HAL_BACKEND")
	if wanted == nil or wanted == "" then wanted = "glibc" end
	local loaded
	for p in (os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):gmatch("[^:]+") do
		local okb, mod = pcall(dofile, p .. "/lib/hal_backend_" .. wanted .. ".lua")
		if okb and type(mod) == "table" then loaded = mod break end
	end
	if not loaded then
		local okb, mod = pcall(dofile, "./modules/lib/hal_backend_" .. wanted .. ".lua")
		if okb and type(mod) == "table" then loaded = mod end
	end
	assert(type(loaded) == "table",
		"HAL: backend not found: modules/lib/hal_backend_" .. wanted .. ".lua (BOT4CJ_HAL_BACKEND=" .. wanted .. ")")
	for _, k in ipairs({ "open", "close", "ioctl_i", "ioctl_p", "read", "write",
		"usleep", "errno", "version", "tcgetattr", "tcsetattr", "cfmakeraw",
		"cfsetispeed", "cfsetospeed", "tcflush" }) do
		assert(type(loaded[k]) == "function",
			"HAL: backend '" .. wanted .. "' missing contract function: " .. k
			.. " (see modules/lib/hal_backend_glibc.lua)")
	end
	HAL.sys = loaded
	HAL.backend_name = wanted
end

------------------------------------------------------------------
-- 总线契约单点（N-25）
------------------------------------------------------------------
-- 路径白名单 / UART 波特率表 / 各长度上限 / byte_of·uint_of 归一化 与
-- virtbus 共用同一份（modules/lib/bus_contract.lua），此前是两库逐字手工
-- 镜像、漂移即「仿真绿真机红」且无报错。定位方式：BOT4CJ_MODULE_PATH 各段
-- 逐段探测（fix-plan 3-3：此前只取首段，非缺省布局会让本文件地基直接失败），
-- 再以仓库缺省布局兜底——本文件是全部驱动的地基，加载失败会全线停摆，故兜底一次。
local bus
do
	local probed
	for p in (os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):gmatch("[^:]+") do
		local ok, m = pcall(dofile, p .. "/lib/bus_contract.lua")
		if ok and type(m) == "table" then probed = m break end
	end
	bus = probed or dofile("./modules/lib/bus_contract.lua")
end

-- 参数归一化（0~255 整数 / 非负整数 / 严格整数，非法 nil）
local byte_of, uint_of, int_of = bus.byte_of, bus.uint_of, bus.int_of

------------------------------------------------------------------
-- I2C（fd 为真实内核句柄，跨 Lua 状态机有效）
------------------------------------------------------------------

-- 打开 I2C 总线，成功返回 fd（>=0），失败返回 -1
function i2c_open(path)
	if not bus.i2c_path_ok(path) then
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
	-- 审计 #10：fd<0（open 失败返回 -1）不得登记进地址表，否则后续误用
	-- 该句柄时排查困难——在登记处就拒绝
	if fd == nil or fd < 0 or addr == nil or addr > 0x7F then
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

-- 写指针后读 n 字节（合并为单次原子事务），返回 1 基字节表 {b0, b1, ...}；
-- 失败返回 nil。与 i2c_read_multi 同一内核事务，但返回字节表、不受 Lua
-- number 2^53 精度限制——多字节块读（BH1745 颜色 8B / BMI270 六轴 12B 等）
-- 请用此接口。n 钳制 1~16。
function i2c_read_multi_bytes(fd, reg, n)
	fd = tonumber(fd)
	reg = byte_of(reg)
	n = uint_of(n)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg == nil or n == nil or addr == nil then return nil end
	if n < 1 then n = 1 end
	if n > bus.BYTES_READ_MAX then n = bus.BYTES_READ_MAX end
	local wb = ffi.new("uint8_t[1]", reg)
	local rb = ffi.new("uint8_t[?]", n)
	local msgs = ffi.new("cj_i2c_msg[2]")
	msgs[0].addr = addr; msgs[0].flags = 0;            msgs[0].len = 1; msgs[0].buf = wb
	msgs[1].addr = addr; msgs[1].flags = HAL.I2C_M_RD; msgs[1].len = n; msgs[1].buf = rb
	if i2c_rdwr(fd, msgs, 2) < 0 then return nil end
	local t = {}
	for i = 0, n - 1 do t[i + 1] = tonumber(rb[i]) end
	return t
end

-- 写指针后读 n 字节（合并为单次原子事务），返回大端合并值；失败返回 -1。
-- 注意合并值为 Lua number：6 字节以上（2^48）将超 2^53 精度被静默截断，
-- 因此 n 有效范围 1~6，越界（含非法值）显式返回 -1——静默钳到 1 字节会把
-- 长度错位放大成错误合并值；更长块读请用 i2c_read_multi_bytes。
function i2c_read_multi(fd, reg, n)
	n = uint_of(n)
	if n == nil or n < 1 or n > bus.MERGED_READ_MAX then return -1 end
	local t = i2c_read_multi_bytes(fd, reg, n)
	if t == nil then return -1 end
	local v = 0
	for i = 1, #t do v = v * 256 + t[i] end
	return v
end

-- 纯读 n 字节（单条读消息，不写指针，SGP30 这类命令式传感器用），大端合并；失败返回 -1
-- n 有效范围 1~6（2^53 精度上限，同 i2c_read_multi：7 字节起大端合并值可能
-- 超 2^53 被静默截断），越界（含非法值）显式返回 -1：静默钳制会被调用方的
-- 拆字节数学放大成错误数据；更长纯读请用 i2c_read_bytes（字节表不受精度限制）
function i2c_read(fd, n)
	fd = tonumber(fd)
	n = uint_of(n)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or n == nil or addr == nil then return -1 end
	if n < 1 or n > bus.MERGED_READ_MAX then return -1 end
	local rb = ffi.new("uint8_t[?]", n)
	local msgs = ffi.new("cj_i2c_msg[1]")
	msgs[0].addr = addr; msgs[0].flags = HAL.I2C_M_RD; msgs[0].len = n; msgs[0].buf = rb
	if i2c_rdwr(fd, msgs, 1) < 0 then return -1 end
	return be_merge(rb, n)
end

------------------------------------------------------------------
-- 16 位寄存器指针器件（ST VL53Lx ToF 族：寄存器地址 16 位、值大端多字节）。
-- 「写 2 字节指针 + 读数据」合并为单次 I2C_RDWR 原子事务——若拆成两段事务，
-- 会重新引入同器件并发访问时的寄存器指针竞态（见本文件 I2C_RDWR 注释）。
------------------------------------------------------------------

-- 写 16 位指针寄存器 + 数据字节（单条写消息，原子），返回 0 = 成功，-1 = 失败
-- 用法：i2c_write_reg16(fd, 0x0087, 0x21) 单字节；
--       i2c_write_reg16(fd, 0x002D, b1, b2, ...) 连续流写（器件指针自增）
function i2c_write_reg16(fd, reg16, ...)
	fd = tonumber(fd)
	reg16 = uint_of(reg16)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg16 == nil or reg16 > 0xFFFF or addr == nil then return -1 end
	local data = {}
	for _, v in ipairs({ ... }) do
		local b = byte_of(v)
		if b == nil then return -1 end
		data[#data + 1] = b
	end
	local buf = ffi.new("uint8_t[?]", #data + 2)
	buf[0] = math.floor(reg16 / 256) % 256
	buf[1] = reg16 % 256
	for i, b in ipairs(data) do buf[i + 1] = b end
	local msgs = ffi.new("cj_i2c_msg[1]")
	msgs[0].addr = addr; msgs[0].flags = 0; msgs[0].len = #data + 2; msgs[0].buf = buf
	if i2c_rdwr(fd, msgs, 1) >= 0 then return 0 end
	return -1
end

-- 写 16 位指针后读 n 字节（合并为单次原子事务），返回大端合并值；失败返回 -1。
-- n 有效范围 1~6（2^53 精度上限，同 i2c_read_multi；本族寄存器最长 dword=4），
-- 越界显式返回 -1（静默钳制危害同 i2c_read_multi）
function i2c_read_reg16(fd, reg16, n)
	fd = tonumber(fd)
	reg16 = uint_of(reg16)
	n = uint_of(n)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or reg16 == nil or reg16 > 0xFFFF or n == nil or addr == nil then return -1 end
	if n < 1 or n > bus.MERGED_READ_MAX then return -1 end
	local wb = ffi.new("uint8_t[2]", math.floor(reg16 / 256) % 256, reg16 % 256)
	local rb = ffi.new("uint8_t[?]", n)
	local msgs = ffi.new("cj_i2c_msg[2]")
	msgs[0].addr = addr; msgs[0].flags = 0;            msgs[0].len = 2; msgs[0].buf = wb
	msgs[1].addr = addr; msgs[1].flags = HAL.I2C_M_RD; msgs[1].len = n; msgs[1].buf = rb
	if i2c_rdwr(fd, msgs, 2) < 0 then return -1 end
	return be_merge(rb, n)
end

-- 纯读 n 字节（单条读消息），逐字节返回 1 基表 { b1, b2, ... }；失败返回 nil
-- 与 i2c_read 的区别：不按大端合并——合并值超过 2^53（约 7 字节）后 double
-- 精度不保，命令式器件的长应答（如 MLX90393 的 XYZT+status 共 9 字节）
-- 必须逐字节取。n 钳制 1~16（对齐 i2c_read_multi_bytes；字节表版保留钳制
-- 的理由同 0.3.1 决策——表长对调用方可见。原实现 n<1 反向钳到 16，
-- 请求 0 字节反而读出 16 字节超长表）
function i2c_read_bytes(fd, n)
	fd = tonumber(fd)
	n = uint_of(n)
	local addr = HAL.i2c_addr[fd]
	if fd == nil or n == nil or addr == nil then return nil end
	if n < 1 then n = 1 end
	if n > bus.BYTES_READ_MAX then n = bus.BYTES_READ_MAX end
	local rb = ffi.new("uint8_t[?]", n)
	local msgs = ffi.new("cj_i2c_msg[1]")
	msgs[0].addr = addr; msgs[0].flags = HAL.I2C_M_RD; msgs[0].len = n; msgs[0].buf = rb
	if i2c_rdwr(fd, msgs, 1) < 0 then return nil end
	local b = {}
	for i = 0, n - 1 do b[i + 1] = tonumber(rb[i]) end
	return b
end

------------------------------------------------------------------
-- 延时
------------------------------------------------------------------
-- 毫秒级延时（usleep，零 fork）
function milli_sleep(ms)
	ms = tonumber(ms)
	if ms == nil or ms <= 0 then return end
	-- 审计 #10：usleep 形参是 unsigned int（usec），ms 超过 4294967 时
	-- ms*1000 回绕成短睡/长睡错乱——钳制到 uint32 上限内并显式提示
	if ms > 4294967 then
		print("HAL: milli_sleep " .. tostring(ms) .. "ms exceeds uint32 usec range, clamped to 4294967ms")
		ms = 4294967
	end
	HAL.sys.usleep(math.floor(ms * 1000))
end

------------------------------------------------------------------
-- SPI（spidev ioctl；fd 为真实内核句柄）
------------------------------------------------------------------

-- 打开 SPI 设备，成功返回 fd（>=0），失败返回 -1
function spi_open(path)
	if type(path) ~= "string" then return -1 end
	if not bus.spi_path_ok(path) then
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
	-- 诊断打印用归一化前的实参（否则越界值会显示成 nil，丢失定位信息）
	local raw_mode, raw_bits, raw_speed = mode, bits, speed
	-- mode/bits/speed 用 int_of（bus_contract 单点）：**拒绝非整数**，不再静默
	-- 取整——与 virtbus 同判据（旧实现走 uint_of 的 math.floor，spi_setup(fd,
	-- 1.9, 8, 1e6) 真机上会变成 mode=1 并成功，仿真侧却直接失败）。
	mode = int_of(mode, 0, 3)
	bits = int_of(bits, 7, 16)
	speed = int_of(speed, 1)
	if fd == nil or mode == nil or bits == nil or speed == nil then
		print("HAL: spi_setup invalid args: mode=" .. tostring(raw_mode) ..
		      " bits=" .. tostring(raw_bits) .. " speed=" .. tostring(raw_speed))
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
	val = int_of(val, 0, 255)	-- 严格整数（拒绝小数，不再静默 floor；同 virtbus）
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
	-- 严格整数（拒绝小数，不再静默 floor；同 virtbus）
	b0, b1, b2 = int_of(b0, 0, 255), int_of(b1, 0, 255), int_of(b2, 0, 255)
	if fd == nil or b0 == nil or b1 == nil or b2 == nil then return -1 end
	local buf = ffi.new("uint8_t[3]", b0, b1, b2)
	if spi_xfer(fd, buf, nil, 3, delay_us) then return 0 end
	return -1
end

-- 一次写一帧任意长度字节（单次传输，CS 保持低），返回 0 = 成功，-1 = 失败；
-- delay_us 可选。data 为 1 基字节值表（如 {0x21, 0x00}）——以表传参避免与
-- spi_write3 的尾参 delay_us 语义冲突。16 位字帧器件（AD9833 等）用此接口。
function spi_write_multi(fd, data, delay_us)
	fd = tonumber(fd)
	if fd == nil or type(data) ~= "table" or #data == 0 then return -1 end
	if #data > bus.SPI_IO_MAX_BYTES then return -1 end
	local buf = ffi.new("uint8_t[?]", #data)
	for i = 1, #data do
		local b = byte_of(data[i])
		if b == nil then return -1 end
		buf[i - 1] = b
	end
	if spi_xfer(fd, buf, nil, #data, delay_us) then return 0 end
	return -1
end

-- 一次读 n 字节（单次传输），返回大端合并值；失败返回 -1（n 有效范围 1~6，
-- 2^53 精度上限同 i2c_read_multi——7 字节起合并值可能被静默截断；越界显式
-- 返回 -1，静默钳制危害同 i2c_read_multi）；更长的全双工帧请用 spi_xfer_full
-- （MOSI 命令与 MISO 数据同帧、返回字节表）；delay_us 可选
function spi_read_multi(fd, n, delay_us)
	fd = tonumber(fd)
	n = uint_of(n)
	if fd == nil or n == nil then return -1 end
	if n < 1 or n > bus.MERGED_READ_MAX then return -1 end
	local buf = ffi.new("uint8_t[?]", n)
	if not spi_xfer(fd, nil, buf, n, delay_us) then return -1 end
	local v = 0
	for i = 0, n - 1 do
		v = v * 256 + tonumber(buf[i])
	end
	return v
end

-- 全双工一帧任意长度（单次传输，CS 保持低）：MOSI 发 data 的同时逐位移入
-- MISO。data 为 1 基字节值表（1~64 字节，同 spi_write_multi），返回等长
-- 1 基接收字节表；任何参数/传输失败返回 nil。delay_us 可选（CS 拉高前等待）。
-- 用于「MOSI 命令段与 MISO 数据段同帧」的协议：spi_read_multi 只发 dummy，
-- 拿不回与命令同帧移位的回传数据（MT6826S 的 24 位读帧：前两字节发
-- 命令+地址，MISO 数据在第 3 字节）。
function spi_xfer_full(fd, data, delay_us)
	fd = tonumber(fd)
	if fd == nil or type(data) ~= "table" or #data == 0 then return nil end
	if #data > bus.SPI_IO_MAX_BYTES then return nil end
	local n = #data
	local txbuf = ffi.new("uint8_t[?]", n)
	for i = 1, n do
		local b = int_of(data[i], 0, 255)	-- 严格整数（拒绝小数；同 virtbus）
		if b == nil then return nil end
		txbuf[i - 1] = b
	end
	local rxbuf = ffi.new("uint8_t[?]", n)
	if not spi_xfer(fd, txbuf, rxbuf, n, delay_us) then return nil end
	local out = {}
	for i = 0, n - 1 do out[i + 1] = tonumber(rxbuf[i]) end
	return out
end

------------------------------------------------------------------
-- GPIO（gpiod uAPI ABI v1：/dev/gpiochipN ioctl）
------------------------------------------------------------------
-- 内核标准接口（>= 4.8），跨 SoC 通用：树莓派/瑞芯微/全志等仅路径与
-- 线号不同。结构体全部定宽字段，x86_64/aarch64 布局一致。
-- ioctl 编号经 test/test_ioctl_nums.c 与系统头文件实证。

HAL.GPIO_GET_LINEHANDLE         = 0xC16CB403	-- _IOWR(0xB4, 0x03, gpiohandle_request)
HAL.GPIOHANDLE_GET_LINE_VALUES  = 0xC040B408	-- _IOWR(0xB4, 0x08, gpiohandle_data)
HAL.GPIOHANDLE_SET_LINE_VALUES  = 0xC040B409	-- _IOWR(0xB4, 0x09, gpiohandle_data)
-- 注意：HAL 运行于 LuaJIT（5.1），无 << 运算符，标志位用字面量（INPUT=1<<0 等）
HAL.GPIOHANDLE_REQUEST_INPUT      = 1
HAL.GPIOHANDLE_REQUEST_OUTPUT     = 2
HAL.GPIOHANDLE_REQUEST_ACTIVE_LOW = 4

ffi.cdef[[
/* /usr/include/linux/gpio.h（ABI v1，deprecated 但全内核支持；uAPI v2 结构含
 * __u64 对齐字段与位掩码语义，对 Lua 侧过重，基础输入输出用 v1 足够） */
typedef struct {
	unsigned int lineoffsets[64];
	unsigned int flags;
	unsigned char default_values[64];
	char consumer_label[32];
	unsigned int lines;
	int fd;
} cj_gpiohandle_request;
typedef struct {
	unsigned char values[64];
} cj_gpiohandle_data;
]]

-- 结构体尺寸自检（防 ABI 静默漂移；字段全定宽，两架构同值）
assert(ffi.sizeof("cj_gpiohandle_request") == 364,
	"gpiohandle_request size mismatch: expect 364, got " .. ffi.sizeof("cj_gpiohandle_request"))
assert(ffi.sizeof("cj_gpiohandle_data") == 64,
	"gpiohandle_data size mismatch: expect 64, got " .. ffi.sizeof("cj_gpiohandle_data"))

-- 请求一条 GPIO 线，返回句柄 fd（供 set/get 使用）；失败返回 -1
-- direction: "out" | "in"；default_value：输出模式的上电值（0/1）
-- active_low：true = 低电平有效（可选）
function gpio_request_line(chip_path, offset, direction, default_value, active_low)
	if type(chip_path) ~= "string" then return -1 end
	-- 前缀取平台差异单点（阶段 A-2）；前缀如引入正则元字符需先转义
	if string.match(chip_path, "^" .. HAL.platform.gpio_prefix .. "%d+$") == nil then
		print("HAL: invalid gpiochip path: " .. tostring(chip_path))
		return -1
	end
	offset = tonumber(offset)
	-- 必须为非负整数：小数（如 17.9）写入 uint32 字段会被静默截断成另一个线号，
	-- 配置错误显式拒绝而非将错就错
	if offset == nil or offset < 0 or offset > 0xFFFFFFFF or offset ~= math.floor(offset) then
		print("HAL: invalid gpio offset: " .. tostring(offset))
		return -1
	end

	local chipfd = tonumber(HAL.sys.open(chip_path, O_RDWR))
	if chipfd == nil or chipfd < 0 then
		print("HAL: failed to open gpiochip: " .. chip_path)
		return -1
	end

	local flags
	if direction == "out" then
		flags = HAL.GPIOHANDLE_REQUEST_OUTPUT
	else
		flags = HAL.GPIOHANDLE_REQUEST_INPUT
	end
	if active_low then
		flags = flags + HAL.GPIOHANDLE_REQUEST_ACTIVE_LOW		-- 位不重叠，算术加即可（HAL 不加载位库）
	end

	local req = ffi.new("cj_gpiohandle_request")	-- ffi.new 零初始化
	req.lineoffsets[0] = offset
	req.flags = flags
	if direction == "out" then
		-- C2（2026-09-20 审计）：默认电平走 byte_of 归一化——旧的
		-- `tonumber(default_value) or 0` 挡不住 NaN（NaN 是 truthy），
		-- 而 `(NaN ~= 0)` 为真 → 配置写 "nan" 会把线**驱动到高电平**。
		-- byte_of 对非有限值返回 nil，回落 0（非有效电平 = 安全侧）。
		local dv = byte_of(default_value) or 0
		req.default_values[0] = (dv ~= 0) and 1 or 0	-- 内核语义：非 0 即有效电平
	end
	req.consumer_label = "bot4cj"
	req.lines = 1

	if HAL.sys.ioctl_p(chipfd, HAL.GPIO_GET_LINEHANDLE, req) ~= 0 or req.fd < 0 then
		print("HAL: failed to request gpio line " .. tostring(offset) ..
		      " on " .. chip_path)
		HAL.sys.close(chipfd)
		return -1
	end

	local hfd = tonumber(req.fd)
	HAL.sys.close(chipfd)		-- 取得句柄后 chip fd 即可关闭（内核 uAPI 约定）
	return hfd
end

-- 设置输出值（非 0 归一为 1，内核把非 0 一律当有效电平），成功返回 true
function gpio_set_value(hfd, v)
	hfd = tonumber(hfd)
	if hfd == nil or hfd < 0 then return false end
	local data = ffi.new("cj_gpiohandle_data")
	-- C2 审计同族（2026-09-20 评估）：与上面的 default_values 同口径——旧的
	-- `tonumber(v) or 0` 挡不住 NaN（NaN 是 truthy，`(NaN ~= 0)` 为真会把线
	-- **驱动到高电平**）。非有限值回落 0（安全侧）；有限值保持「非 0 归一为 1」
	-- 的既有语义（不用 byte_of：它把 300 这类越界值判非法，语义比这里更窄）。
	local n = bus.finite_of(v)
	data.values[0] = (n ~= nil and n ~= 0) and 1 or 0
	return HAL.sys.ioctl_p(hfd, HAL.GPIOHANDLE_SET_LINE_VALUES, data) == 0
end

-- 读取输入值，返回 0/1；失败返回 -1
function gpio_get_value(hfd)
	hfd = tonumber(hfd)
	if hfd == nil or hfd < 0 then return -1 end
	local data = ffi.new("cj_gpiohandle_data")
	if HAL.sys.ioctl_p(hfd, HAL.GPIOHANDLE_GET_LINE_VALUES, data) ~= 0 then return -1 end
	return tonumber(data.values[0])
end

-- 释放 GPIO 句柄（关闭句柄 fd 即释放线占用）
function gpio_free_line(hfd)
	hfd = tonumber(hfd)
	if hfd ~= nil and hfd >= 0 then HAL.sys.close(hfd) end
end

------------------------------------------------------------------
-- UART（termios raw 8N1：/dev/tty*、/dev/serial*；非阻塞轮询读）
-- 生命周期：open 时快照原始 termios（HAL.uart_orig[fd]），close 时先
-- tcsetattr 恢复再关闭——板载 UART 的 termios 跨 close 保留在 tty 上，
-- 残留 raw 模式/波特率会影响后续使用者（getty、其他程序）。
------------------------------------------------------------------
-- 常量经 test/test_uart_consts.c 与系统头文件实证（gpio 段同款方法）：
-- sizeof(struct termios)=60、c_line 偏移 16、c_cc 偏移 17、VMIN=6/VTIME=5、
-- O_RDWR=2 O_NOCTTY=256 O_NONBLOCK=2048、TCSANOW=0、TCIFLUSH=0、
-- CREAD=128 CLOCAL=2048 CS8=48。
-- 【波特率必须传当版 glibc 的 B* 常量编码，不能想当然】编码随 glibc 版本
-- 分叉（glibc 2025-06 commit 5cf101a8，2.42 起生效）：
--   < 2.42：B* 是旧 ABI 编号（B9600=13、B115200=0x1002）——cfsetispeed/
--           cfsetospeed 对字面 Hz（9600/115200/…）返回 EINVAL 且不修改
--           结构体，旧实现「传 Hz」看似成功实际波特率从未生效（真机
--           /dev/serial0 实证：请求 115200 后 B 字段仍为原值）；
--   ≥ 2.42：B* 即字面 Hz（B9600=9600）——反向地，旧编号 13 会被当成
--           「13 波特」静默生效。
-- 因此 UART_B_CONST 按运行时 glibc 版本（gnu_get_libc_version）二选一；
-- test_uart_consts.c 以 #if 分支与两套表同步断言（审计 P0-3）。

ffi.cdef[[
int  tcgetattr(int fd, void *termios_p);
int  tcsetattr(int fd, int act, const void *termios_p);
void cfmakeraw(void *termios_p);
int  cfsetispeed(void *termios_p, unsigned int speed);
int  cfsetospeed(void *termios_p, unsigned int speed);
int  tcflush(int fd, int queue_selector);

typedef struct {
	unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
	unsigned char c_line;
	unsigned char c_cc[32];
	unsigned int c_ispeed, c_ospeed;
} cj_termios;
]]

-- 结构体尺寸自检：glibc x86_64/aarch64 均为 60 字节，不符则立即失败
-- （防止分架构布局静默漂移导致 c_cc 下标越界写）
assert(ffi.sizeof("cj_termios") == 60,
	"cj_termios size mismatch: expect 60, got " .. ffi.sizeof("cj_termios"))

HAL.UART_O_FLAGS  = 2306		-- O_RDWR(2) | O_NOCTTY(256) | O_NONBLOCK(2048)
HAL.UART_TCSANOW  = 0
HAL.UART_TCIFLUSH = 0
HAL.UART_VMIN_IDX = 6			-- c_cc 下标（VMIN）
HAL.UART_VTIME_IDX = 5			-- c_cc 下标（VTIME）
HAL.UART_CREAD_CLOCAL = 2176	-- CREAD(128) | CLOCAL(2048)
HAL.EAGAIN = 11					-- Linux errno：非阻塞读暂无数据
HAL.EINTR = 4					-- Linux errno：被信号打断
HAL.UART_READ_MAX_MS = bus.UART_READ_MAX_MS	-- uart_read 轮询上限：iters=timeout_ms+1 按 1ms 空转，
								-- 超长值（配置笔误）必须立即失败而非阻塞数小时

HAL.uart_baud = {}				-- fd -> 波特率（调试/生命周期摘要）
HAL.uart_orig = {}				-- fd -> open 前的原始 termios（close 恢复用）

-- UART 波特率白名单与长度上限均取自总线契约单点（bus_contract.lua，N-25）

-- Hz → speed_t 常量：旧 ABI 编码表（glibc < 2.42，test_uart_consts.c 旧分支断言）。
-- 白名单与映射表必须同步维护：白名单通过而映射缺失时 uart_open 明确失败，
-- 不会退回「静默不生效」的旧行为。
local UART_B_CONST_LEGACY = {
	[1200] = 9, [2400] = 11, [4800] = 12, [9600] = 13, [19200] = 14, [38400] = 15,
	[57600] = 0x1001, [115200] = 0x1002, [230400] = 0x1003, [460800] = 0x1004,
	[500000] = 0x1005, [921600] = 0x1007, [1000000] = 0x1008,
}
-- Hz → speed_t 常量：字面 Hz 编码表（glibc ≥ 2.42，test_uart_consts.c 新分支断言）
local UART_B_CONST_HZ = {
	[1200] = 1200, [2400] = 2400, [4800] = 4800, [9600] = 9600, [19200] = 19200,
	[38400] = 38400, [57600] = 57600, [115200] = 115200, [230400] = 230400,
	[460800] = 460800, [500000] = 500000, [921600] = 921600, [1000000] = 1000000,
}
-- 运行时探测：glibc ≥ 2.42 用 Hz 表，否则旧 ABI 编号表（见段首注释）。
-- 探测失败（非常规 libc 等）按旧编码处理：旧编码在 <2.42 上正确，在 ≥2.42
-- 上会静默设错——故本探测与 test_uart_baud.lua 回归、test_uart_consts.c
-- 构建期断言三重把关，编码分叉时尽可能早地暴露。
-- 阶段 A-1：版本探测经 HAL.sys.version()（后端接口；非 glibc 后端返 nil
-- 时按旧编码处理，musl 等平台行为与 glibc<2.42 一致）。
local function glibc_uses_hz_baud()
	local ok, ver = pcall(function()
		local v = HAL.sys.version()
		local major, minor = string.match(v ~= nil and v or "", "(%d+)%.(%d+)")
		if major == nil then return 0 end
		return tonumber(major) * 1000 + tonumber(minor)
	end)
	return ok == true and ver >= 2042	-- 2042 == glibc 2.42
end
local UART_B_CONST = glibc_uses_hz_baud() and UART_B_CONST_HZ or UART_B_CONST_LEGACY
HAL.UART_B_CONST = UART_B_CONST		-- 导出供 test_uart_baud.lua 回归核对

-- 打开串口（raw 8N1 + 波特率 + 非阻塞），成功返回 fd，失败返回 -1
function uart_open(path, baud)
	if type(path) ~= "string" then return -1 end
	baud = tonumber(baud)
	if not bus.baud_ok(baud) then
		print("HAL: invalid uart baud: " .. tostring(baud))
		return -1
	end
	local bconst = UART_B_CONST[baud]
	if bconst == nil then
		print("HAL: uart baud has no termios constant: " .. tostring(baud))
		return -1
	end
	-- 路径白名单：常规 tty 命名 + /dev/serialN（树莓派软链惯例）
	if not bus.uart_path_ok(path) then
		print("HAL: invalid uart path: " .. tostring(path))
		return -1
	end
	local fd = tonumber(HAL.sys.open(path, HAL.UART_O_FLAGS))
	if fd == nil or fd < 0 then
		print("HAL: failed to open uart: " .. path)
		return -1
	end
	local t = ffi.new("cj_termios[1]")	-- ffi.new 零初始化
	if HAL.sys.tcgetattr(fd, t) ~= 0 then
		print("HAL: tcgetattr failed: " .. path .. " (是串口设备吗?)")
		HAL.sys.close(fd)
		return -1
	end
	-- 快照 open 前的原始 termios（整 60 字节按值拷贝；t 随后被 cfmakeraw
	-- 就地改写）。仅在内核侧配置成功后登记（见函数尾），失败路径不产生
	-- 残留快照。
	local orig = ffi.new("cj_termios[1]")
	ffi.copy(orig, t, ffi.sizeof("cj_termios"))
	HAL.sys.cfmakeraw(t)				-- raw 模式（无回显/无行缓冲/8N1 基础）
	-- CREAD|CLOCAL 用真位或置位（cfmakeraw 不清这两位，加法会进位污染高位；
	-- HAL 仅支持 LuaJIT，内建 bit 库可用）
	local bitlib = require("bit")
	t[0].c_cflag = bitlib.bor(tonumber(t[0].c_cflag) or 0, HAL.UART_CREAD_CLOCAL)
	t[0].c_cc[HAL.UART_VMIN_IDX] = 0	-- 非阻塞读：VMIN=0
	t[0].c_cc[HAL.UART_VTIME_IDX] = 1	-- 100ms 字间计时器（驱动层另有轮询超时）
	-- 传 glibc 常量而非 Hz（见段首注释）；返回码必查：EINVAL 时结构体未被
	-- 修改，放行等于「以为设了波特率其实没设」
	if HAL.sys.cfsetispeed(t, bconst) ~= 0 or HAL.sys.cfsetospeed(t, bconst) ~= 0 then
		print("HAL: failed to set uart speed: " .. path .. " @" .. tostring(baud) .. " baud")
		HAL.sys.close(fd)
		return -1
	end
	if HAL.sys.tcsetattr(fd, HAL.UART_TCSANOW, t) ~= 0 then
		print("HAL: tcsetattr failed: " .. path)
		HAL.sys.close(fd)
		return -1
	end
	HAL.uart_orig[fd] = orig
	HAL.uart_baud[fd] = baud
	return fd
end

-- 关闭串口：先恢复 open 前快照的 termios（销毁重置），再关闭 fd。
-- 恢复失败（设备已拔出等）告警但不阻断关闭，返回值仍是 close 的结果。
-- 注：多个变量共用同一物理串口时各自独立快照/恢复（每变量独立 lua_State），
-- 后关闭者以自己 open 时看到的配置为准，不做跨变量引用计数。
function uart_close(fd)
	fd = tonumber(fd)
	if fd == nil or fd < 0 then return -1 end
	HAL.uart_baud[fd] = nil
	local orig = HAL.uart_orig[fd]
	if orig ~= nil then
		HAL.uart_orig[fd] = nil
		if HAL.sys.tcsetattr(fd, HAL.UART_TCSANOW, orig) ~= 0 then
			print("HAL: failed to restore uart termios on close (fd " .. tostring(fd) .. ")")
		end
	end
	return HAL.sys.close(fd)
end

-- 清空输入缓冲（丢弃未读字节，请求-应答协议发帧前用）
function uart_flush(fd)
	fd = tonumber(fd)
	if fd == nil or fd < 0 then return -1 end
	return HAL.sys.tcflush(fd, HAL.UART_TCIFLUSH)
end

-- 写一帧字节（data 为 1 基字节值表），返回写入字节数或 -1
function uart_write(fd, data)
	fd = tonumber(fd)
	if fd == nil or type(data) ~= "table" or #data == 0 then return -1 end
	if #data > bus.UART_IO_MAX_BYTES then return -1 end
	local buf = ffi.new("uint8_t[?]", #data)
	for i = 1, #data do
		local b = byte_of(data[i])
		if b == nil then return -1 end
		buf[i - 1] = b
	end
	local r = tonumber(HAL.sys.write(fd, buf, #data))
	if r == nil or r < 0 then return -1 end
	return r
end

-- 读最多 n 字节（非阻塞轮询，1ms 步进直至 timeout_ms），返回 1 基字节表
-- （可能为空表 = 超时无数据）；fd 读取错误（EAGAIN/EINTR 之外的 errno）
-- 立即返回 nil——原实现把一切 read 失败当"无数据"空转到超时，错误被吞。
-- timeout_ms 超 UART_READ_MAX_MS 显式失败返回 nil（诊断打印）：iters 与参数
-- 线性增长，超长值会按 1ms 空转阻塞调用线程数小时，配置笔误必须立即暴露
function uart_read(fd, n, timeout_ms)
	fd = tonumber(fd)
	n = uint_of(n)
	-- C2（2026-09-20 审计）：非有限超时不得被静默当成"不等待"。旧写法
	-- `tonumber(timeout_ms) or 0` 让 NaN 穿过上限判定，随后 math.floor(NaN)=NaN
	-- 使 `for it = 1, NaN` 一次都不执行 —— 调用方看到的是"立即超时无数据"，
	-- 与配置里写坏一个字面量的真实成因完全脱节。
	if timeout_ms ~= nil then
		timeout_ms = bus.finite_of(timeout_ms)
		if timeout_ms == nil then
			print("HAL: uart_read timeout is not a finite number: "
				.. tostring(timeout_ms))
			return nil
		end
	else
		timeout_ms = 0
	end
	if fd == nil or n == nil or n < 1 then return nil end
	if timeout_ms > HAL.UART_READ_MAX_MS then
		print("HAL: uart_read timeout " .. tostring(timeout_ms) .. "ms exceeds max "
			.. tostring(HAL.UART_READ_MAX_MS) .. "ms")
		return nil
	end
	if n > bus.UART_IO_MAX_BYTES then n = bus.UART_IO_MAX_BYTES end
	local buf = ffi.new("uint8_t[?]", n)
	local out = {}
	local iters = math.max(0, math.floor(timeout_ms)) + 1
	local eof_streak = 0
	for it = 1, iters do
		local want = n - #out
		local r = tonumber(HAL.sys.read(fd, buf, want))
		if r ~= nil and r > 0 then
			eof_streak = 0
			for i = 0, r - 1 do out[#out + 1] = tonumber(buf[i]) end
			if #out >= n then break end
		elseif r == 0 then
			-- 审计 #10：read()==0 是 EOF（USB 串口拔出/对端关闭），不是
			-- "暂无数据"——旧实现当作空转满超时，拔线后错误被吞。连续
			-- 3 次 EOF 即判定设备已消失，立即失败。
			eof_streak = eof_streak + 1
			if eof_streak >= 3 then
				print("HAL: uart_read hit EOF (device gone?) fd=" .. tostring(fd))
				return nil
			end
		elseif r == nil or r < 0 then
			-- EAGAIN=暂无数据、EINTR=被信号打断：继续轮询；其余错误显式失败
			local e = HAL.sys.errno()
			if e ~= HAL.EAGAIN and e ~= HAL.EINTR then return nil end
		end
		if it < iters then HAL.sys.usleep(1000) end
	end
	return out
end

------------------------------------------------------------------
-- PWM（Linux sysfs：/sys/class/pwm/pwmchipN/pwmM）
------------------------------------------------------------------
-- 波形由 SoC（Pi 5 为 RP1）的 PWM 控制器自行产生，Lua 侧只写
-- period / duty_cycle / enable 三个属性——无 CPU 占用、无软件抖动。
-- 这是本框架唯一可行的 PWM 路径：软件 bit-bang 要么阻塞主线程的动作播放，
-- 要么在守护线程里 ffi usleep 自旋耗尽 schd-worker 池（README 已记死锁）。
--
-- 【为何用 ffi 而不是 io 库】Lua.cj 的 sandbox_lua 中和了 io.open/io.lines，
-- 驱动写不了 sysfs；这里走 HAL.sys.open/write（ffi 直调 libc），
-- 既保持沙箱不变，也与各总线段的写法一致。
--
-- 【sysfs 已 deprecated 但可用】树莓派 OS（Bookworm）默认挂载 /sys/class/pwm；
-- 将来若需多通道原子配置可换 /dev/pwmchip* 的字符设备 uAPI——届时按
-- test/test_ioctl_nums.c 的方法实证 ioctl 编号与结构体尺寸，别硬编码。
--
-- 接口：pwm_exported / pwm_export / pwm_config / pwm_set_polarity /
--       pwm_enable / pwm_unexport
-- chip、channel 必须是非负整数（防路径穿越），其余一律拒绝并打印诊断。
-- 典型：CM0 / Pi Zero 2 W 的 PWM1 复用 GPIO19（ALT5）→ chip=0 channel=1。

-- 平台差异单点见本文件顶部 HAL.platform（pwm_sysfs_root）：下面 pwm_chip_path /
-- pwm_attr_path 直接取 HAL.platform.pwm_sysfs_root，此处**不再**重复赋值
-- ——曾有一行 `HAL.PWM_SYSFS_ROOT = "/sys/class/pwm"` 把它重置回字面量，
-- 使移植/仿真桩改 HAL.platform.pwm_sysfs_root 时静默失效。
HAL.pwm_state = {}			-- "chip/channel" -> {freq, duty, period_ns, duty_ns, enabled}

local O_WRONLY = 1
local O_RDONLY = 0

-- sysfs 写失败几乎总是这两种原因，给出人话提示（errno 取自 Linux 通用值）
HAL.ENOENT = 2			-- 节点不存在：overlay 未启用 / chip 编号不对
HAL.EACCES = 13			-- 权限不足：export 通常仅 root 可写
HAL.EINVAL = 22			-- 参数被内核拒绝（如越界通道号、重复 export）

local function errno_hint(e)
	if e == HAL.ENOENT then
		return "ENOENT(节点不存在：PWM overlay 未启用，或 pwmchip 编号不对?)"
	elseif e == HAL.EACCES then
		return "EACCES(权限不足：export/unexport 默认仅 root 可写，需 udev 规则或 sudo -E)"
	elseif e == HAL.EINVAL then
		return "EINVAL(参数被内核拒绝)"
	end
	return "errno=" .. tostring(e)
end

-- 规范化非负整数 id（拒绝负数、小数、"../"、超范围值），非法返回 nil。
-- 上限 65535：既覆盖真实 pwmchip 编号，也避免 tostring 对大数输出科学
-- 计数法（如 1e+15）污染路径。
local function pwm_id(v)
	local n = tonumber(v)
	if n == nil or n < 0 or n > 65535 or n ~= math.floor(n) then return nil end
	return tostring(math.floor(n))
end

-- pwmchip 级属性路径（export / unexport）
local function pwm_chip_path(chip, attr)
	local c = pwm_id(chip)
	if c == nil then
		print("HAL: invalid pwm chip: " .. tostring(chip))
		return nil
	end
	return HAL.platform.pwm_sysfs_root .. "/pwmchip" .. c .. "/" .. attr
end

-- 通道级属性路径（period / duty_cycle / enable / polarity）
local function pwm_attr_path(chip, channel, attr)
	local c, h = pwm_id(chip), pwm_id(channel)
	if c == nil or h == nil then
		print("HAL: invalid pwm chip/channel: " .. tostring(chip) .. "/" .. tostring(channel))
		return nil
	end
	return HAL.platform.pwm_sysfs_root .. "/pwmchip" .. c .. "/pwm" .. h .. "/" .. attr
end

-- 向任意 sysfs 文件写入字符串；成功 true，失败打印路径并 false
local function sysfs_write(path, val)
	if type(path) ~= "string" then return false end
	local fd = tonumber(HAL.sys.open(path, O_WRONLY))
	if fd == nil or fd < 0 then
		print("HAL: pwm open failed: " .. path .. " " .. errno_hint(HAL.sys.errno()))
		return false
	end
	local s = tostring(val)
	-- ffi.copy 从 Lua 字符串拷贝（不显式给 len）会连同结尾 NUL 一起拷
	-- （#s+1 字节）；缓冲只开 #s 会固定越界 1 字节、写坏相邻 GC 对象，
	-- 下一次 GC 扫描时段错误（崩溃点与写操作相隔甚远，极难查）。
	-- 缓冲留 1 字节给 NUL；实际写计数仍用 #s，NUL 不发给内核。
	local buf = ffi.new("char[?]", #s + 1)
	ffi.copy(buf, s)
	local n = tonumber(HAL.sys.write(fd, buf, #s))
	HAL.sys.close(fd)
	if n == nil or n < #s then
		print("HAL: pwm write failed: " .. path .. " <- " .. s .. " " .. errno_hint(HAL.sys.errno()))
		return false
	end
	return true
end

-- 读 pwmchip 的通道数（npwm）；读不到返回 nil
-- 通道号必须小于该值，export 越界通道时内核静默不生成目录，
-- 故失败诊断必须带上它（只说"没生效"无法区分越界与权限）。
function pwm_npwm(chip)
	local p = pwm_chip_path(chip, "npwm")
	if p == nil then return nil end
	local fd = tonumber(HAL.sys.open(p, O_RDONLY))
	if fd == nil or fd < 0 then return nil end
	local buf = ffi.new("char[16]")
	local n = tonumber(HAL.sys.read(fd, buf, 16))
	HAL.sys.close(fd)
	if n == nil or n <= 0 then return nil end
	return tonumber(ffi.string(buf, n))
end

-- 通道是否已导出（以 enable 属性能否打开为准）
-- 用 O_RDONLY 探测：存在性判断不该受写权限影响——
-- enable 默认 0644 root:root，非 root 用 O_WRONLY 探测会拿到 EACCES 误判为"未导出"
function pwm_exported(chip, channel)
	local path = pwm_attr_path(chip, channel, "enable")
	if path == nil then return false end
	local fd = tonumber(HAL.sys.open(path, O_RDONLY))
	if fd == nil or fd < 0 then return false end
	HAL.sys.close(fd)
	return true
end

-- 导出通道；已导出则直接成功（重复 export 会被内核以 EINVAL 拒绝）
function pwm_export(chip, channel)
	if pwm_exported(chip, channel) then return true end
	local p = pwm_chip_path(chip, "export")
	local h = pwm_id(channel)
	if p == nil or h == nil then
		print("HAL: invalid pwm channel: " .. tostring(channel))
		return false
	end
	-- 前置校验：越界通道号内核只回一个 EINVAL，错误信息本身无法自解释。
	-- 最常见的误用是把 channel 当成 GPIO 引脚号填（如 17）——
	-- 引脚由 config.txt 的 dtoverlay 决定，与 channel 无关。
	local n = pwm_npwm(chip)
	if n ~= nil and tonumber(h) >= n then
		print("HAL: pwm channel " .. h .. " out of range on pwmchip" .. tostring(chip) ..
			": 该 chip 只有 " .. tostring(n) .. " 个通道（合法值 0~" .. tostring(n - 1) .. "）。" ..
			"channel 是 PWM 硬件通道号，不是 GPIO 引脚号——引脚由 dtoverlay 决定。")
		return false
	end
	if not sysfs_write(p, h) then return false end
	if not pwm_exported(chip, channel) then
		-- 写入成功却没生成目录，最常见是通道号越界（内核静默忽略）
		local n = pwm_npwm(chip)
		if n ~= nil then
			print("HAL: pwm export did not take effect: pwmchip" .. tostring(chip) ..
				" pwm" .. tostring(h) .. " — 该 chip 只有 " .. tostring(n) ..
				" 个通道，channel 须为 0~" .. tostring(n - 1) ..
				"（要双通道请用 dtoverlay=pwm-2chan 并重启）")
		else
			print("HAL: pwm export did not take effect: pwmchip" .. tostring(chip) ..
				" pwm" .. tostring(h) .. "（无法读取 npwm，检查 /sys/class/pwm 挂载与权限）")
		end
		return false
	end
	return true
end

-- 设载波频率与占空比（duty 为百分比 0~100）
-- 返回 (true, period_ns, duty_ns) 或 false
function pwm_config(chip, channel, freq_hz, duty_pct)
	local f = tonumber(freq_hz)
	local d = tonumber(duty_pct)
	if f == nil or f <= 0 then
		print("HAL: invalid pwm frequency: " .. tostring(freq_hz))
		return false
	end
	if d == nil or d < 0 or d > 100 then
		print("HAL: invalid pwm duty: " .. tostring(duty_pct) .. " (expect 0~100)")
		return false
	end
	local period_ns = math.floor(1e9 / f + 0.5)
	if period_ns < 1 then period_ns = 1 end
	local duty_ns = math.floor(period_ns * d / 100 + 0.5)
	if duty_ns > period_ns then duty_ns = period_ns end

	-- 写序：内核要求 duty_cycle <= period，故先把 duty 归零再改 period。
	-- 否则「新 period 小于旧 duty」会被内核以 EINVAL 拒绝——降频必踩。
	-- 代价是切换瞬间多一个低电平周期，对电机/LED 无实际影响。
	if not sysfs_write(pwm_attr_path(chip, channel, "duty_cycle"), 0) then return false end
	if not sysfs_write(pwm_attr_path(chip, channel, "period"), period_ns) then return false end
	if not sysfs_write(pwm_attr_path(chip, channel, "duty_cycle"), duty_ns) then return false end

	local key = tostring(pwm_id(chip)) .. "/" .. tostring(pwm_id(channel))
	local st = HAL.pwm_state[key] or {}
	st.freq, st.duty = f, d
	st.period_ns, st.duty_ns = period_ns, duty_ns
	HAL.pwm_state[key] = st
	return true, period_ns, duty_ns
end

-- 极性：inversed = 输出反相（低电平有效的驱动板用）
function pwm_set_polarity(chip, channel, inversed)
	return sysfs_write(pwm_attr_path(chip, channel, "polarity"),
		inversed and "inversed" or "normal")
end

-- 使能 / 关闭输出
function pwm_enable(chip, channel, on)
	local ok = sysfs_write(pwm_attr_path(chip, channel, "enable"), (on and 1 or 0))
	if ok then
		local key = tostring(pwm_id(chip)) .. "/" .. tostring(pwm_id(channel))
		local st = HAL.pwm_state[key] or {}
		st.enabled = on and true or false
		HAL.pwm_state[key] = st
	end
	return ok
end

-- 取消导出（先停输出，避免取消后引脚停在某个电平上继续驱动负载）
function pwm_unexport(chip, channel)
	local p = pwm_chip_path(chip, "unexport")
	local h = pwm_id(channel)
	if p == nil or h == nil then return false end
	pwm_enable(chip, channel, false)
	local ok = sysfs_write(p, h)
	if ok then HAL.pwm_state[tostring(pwm_id(chip)) .. "/" .. tostring(h)] = nil end
	return ok
end

return HAL
