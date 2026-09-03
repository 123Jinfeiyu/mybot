-- HAL_shell.lua - 基层硬件抽象层（shell-out 降级后端）
-- bot4cj 仓颉语言机器人框架
--
-- 由 HAL.lua 在非 LuaJIT 环境（无 ffi）自动 dofile 加载；
-- LuaJIT 环境下 HAL.lua 使用 ffi 直调 glibc，不经过本文件。
--
-- issue #14：完全解耦的基层硬件应用层驱动——不再依赖仓颉注册的 cfunc。
-- I2C 经 i2c-tools（i2cset/i2cget/i2ctransfer）实现，
-- SPI 经 spi-tools（spi-config 持久化 mode/bits；spi-pipe 单次事务，speed 每次传递）。
-- 对外函数名与原 cfunc 完全一致，驱动模块的调用方式不变。
--
-- 【句柄设计】每个硬件变量拥有独立的 Lua 状态机，句柄表无法跨状态机共享，
-- 因此 fd 直接编码总线身份（无状态解码，任何状态机都能用）：
--   I2C:  fd = 1000000 + bus            （/dev/i2c-N 的 N）
--   SPI:  fd = 2000000 + B*100 + C      （/dev/spidevB.C）
-- 从设备地址由 i2c_set_addr 在各状态机内本地记录（驱动的惯例是每次操作前先 set_addr）。
-- SPI 的 speed 无法编码进 fd，由 spi_setup 写入 /tmp/bot4cj-hal/ 下的配置文件跨状态机传递
-- （mode/bits 经实测由内核持久化，无需传递）。
--
-- 【安全】所有数值参数经 tonumber + 范围校验后才拼入命令；设备路径一律由解码后的
-- 数字重构，绝不直接拼接调用方传入的字符串，杜绝 shell 注入。
--
-- 【可测试性】命令执行统一走 HAL.exec / HAL.popen_r / HAL.popen_w 间接层，
-- 离线单测（test/test_hal_shell.lua）注入 mock 即可验证命令构造与输出解析。
--
-- 【已知限制】
--   1. spi_setup 的 delay 参数无 spi-tools 对应物，被忽略（原 cfunc 设置 transfer 延时）
--   2. 每次寄存器操作 fork 一个外部进程，密集写场景（快步行×多舵机）延迟高于 ioctl
--   3. milli_sleep 经 sleep 命令实现，精度约 ±数 ms

local HAL = {}

-- 重复加载保护：同一状态机内只初始化一次
if _G.__BOT4CJ_HAL ~= nil then return _G.__BOT4CJ_HAL end
_G.__BOT4CJ_HAL = HAL

------------------------------------------------------------------
-- 工具路径探测（i2c-tools 装在 /usr/sbin，通常不在服务进程 PATH 中）
------------------------------------------------------------------
local function find_tool(name)
	for _, dir in ipairs({ "/usr/sbin", "/sbin", "/usr/bin", "/bin" }) do
		local f = io.open(dir .. "/" .. name, "r")
		if f then
			f:close()
			return dir .. "/" .. name
		end
	end
	return name		-- 退回 PATH 查找
end

HAL.i2cset      = find_tool("i2cset")
HAL.i2cget      = find_tool("i2cget")
HAL.i2ctransfer = find_tool("i2ctransfer")
HAL.spi_config  = find_tool("spi-config")
HAL.spi_pipe    = find_tool("spi-pipe")
HAL.sleep_bin   = find_tool("sleep")
HAL.od_bin      = find_tool("od")

------------------------------------------------------------------
-- 命令执行间接层（离线单测在此注入 mock）
------------------------------------------------------------------
-- 执行命令（经 /bin/sh），返回 ok:Bool, code:Int
function HAL.exec(cmd)
	local ok, kind, code = os.execute(cmd)
	if ok == true then return true, 0 end
	if kind == "exit" then return false, code or -1 end
	return false, -1			-- 被信号终止等情况
end

-- 只读管道（取命令 stdout），返回文件句柄
function HAL.popen_r(cmd)
	return io.popen(cmd, "r")
end

-- 只写管道（向命令 stdin 写入），返回文件句柄
function HAL.popen_w(cmd)
	return io.popen(cmd, "w")
end

------------------------------------------------------------------
-- 参数归一化
------------------------------------------------------------------
-- 归一化为 0~255 的整数，非法返回 nil（拼命令前必须过这一关）
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
-- 句柄编解码
------------------------------------------------------------------
local I2C_FD_BASE = 1000000
local SPI_FD_BASE = 2000000

local function i2c_encode(bus)  return I2C_FD_BASE + bus end
local function i2c_decode(fd)
	local n = uint_of(fd)
	-- 注意用 < 而非 <=：bus 0 编码后恰好等于 I2C_FD_BASE
	if n == nil or n < I2C_FD_BASE or n >= SPI_FD_BASE then return nil end
	return n - I2C_FD_BASE			-- bus
end

local function spi_encode(b, c)  return SPI_FD_BASE + b * 100 + c end
local function spi_decode(fd)
	local n = uint_of(fd)
	-- 注意用 < 而非 <=：spidev0.0 编码后恰好等于 SPI_FD_BASE
	if n == nil or n < SPI_FD_BASE then return nil end
	local v = n - SPI_FD_BASE
	return math.floor(v / 100), v % 100	-- B, C
end

-- 各状态机本地的从设备地址表：fd -> addr
local i2c_addrs = {}
-- 各状态机本地的 SPI 配置缓存：fd -> {speed=...}
local spi_conf_cache = {}

------------------------------------------------------------------
-- I2C
------------------------------------------------------------------

-- 打开 I2C 总线，成功返回句柄（>0），失败返回 -1（与 cfunc 契约一致）
function i2c_open(path)
	if type(path) ~= "string" then return -1 end
	local bus = tonumber(string.match(path, "^/dev/i2c%-(%d+)$"))
	if bus == nil then
		print("HAL: invalid i2c path: " .. tostring(path))
		return -1
	end
	-- 探测总线设备是否存在（只开即关，ioctl 由 i2c-tools 完成）
	local f = io.open(path, "rb")
	if f == nil then
		print("HAL: i2c bus not found: " .. path)
		return -1
	end
	f:close()
	return i2c_encode(bus)
end

function i2c_close(fd)
	i2c_addrs[uint_of(fd) or -1] = nil
end

-- 记录从设备地址（本状态机内有效；驱动惯例是每次操作序列前先调用）
function i2c_set_addr(fd, addr)
	local key = uint_of(fd)
	local a = byte_of(addr)
	if key == nil or a == nil or i2c_decode(key) == nil then
		print("HAL: i2c_set_addr invalid args: " .. tostring(fd) .. ", " .. tostring(addr))
		return
	end
	i2c_addrs[key] = a
end

-- 内部：解析目标（bus, addr），任一缺失返回 nil
local function i2c_target(fd)
	local key = uint_of(fd)
	if key == nil then return nil end
	local bus = i2c_decode(key)
	local addr = i2c_addrs[key]
	if bus == nil or addr == nil then
		print("HAL: i2c target not ready (bus=" .. tostring(bus) ..
		      ", addr=" .. tostring(addr) .. "); call i2c_set_addr first")
		return nil
	end
	return bus, addr
end

-- 写寄存器（原 cfunc 无返回值，失败仅打印；此处额外返回 ok 便于测试）
function i2c_write_reg(fd, reg, val)
	local bus, addr = i2c_target(fd)
	if bus == nil then return false end
	local r, v = byte_of(reg), byte_of(val)
	if r == nil or v == nil then return false end
	local ok = HAL.exec(string.format("%s -f -y %d 0x%02x 0x%02x 0x%02x",
		HAL.i2cset, bus, addr, r, v))
	return ok
end

-- 内部：解析 i2c-tools 的十六进制输出（"0x12 0x34"），返回字节数组
local function parse_hex_bytes(out)
	local bytes = {}
	if type(out) ~= "string" then return bytes end
	for h in string.gmatch(out, "0x(%x%x)") do
		bytes[#bytes + 1] = tonumber(h, 16)
	end
	return bytes
end

-- 内部：执行 i2ctransfer 并解析读回的字节；失败返回 nil
local function i2c_transfer_read(bus, desc_args)
	local cmd = HAL.i2ctransfer .. " -f -y " .. bus .. " " .. desc_args
	local p = HAL.popen_r(cmd .. " 2>/dev/null")
	if p == nil then return nil end
	local out = p:read("a")
	local ok = p:close()
	if ok ~= true and ok ~= 0 then return nil end
	return parse_hex_bytes(out)
end

-- 读寄存器，返回 0~255；失败返回 -1
function i2c_read_reg(fd, reg)
	local bus, addr = i2c_target(fd)
	if bus == nil then return -1 end
	local r = byte_of(reg)
	if r == nil then return -1 end
	local p = HAL.popen_r(string.format("%s -f -y %d 0x%02x 0x%02x 2>/dev/null",
		HAL.i2cget, bus, addr, r))
	if p == nil then return -1 end
	local out = p:read("a")
	local ok = p:close()
	if ok ~= true and ok ~= 0 then return -1 end
	local bytes = parse_hex_bytes(out)
	if #bytes < 1 then return -1 end
	return bytes[1]
end

-- 写寄存器+多字节数据（burst 写，原 cfunc 为 reg+2 字节，此处支持任意多字节）
-- 返回 0 = 成功，-1 = 失败
function i2c_write_multi(fd, reg, ...)
	local bus, addr = i2c_target(fd)
	if bus == nil then return -1 end
	local r = byte_of(reg)
	if r == nil then return -1 end

	local data = { r }
	for _, v in ipairs({ ... }) do
		local b = byte_of(v)
		if b == nil then return -1 end
		data[#data + 1] = b
	end

	local parts = {}
	for _, b in ipairs(data) do
		parts[#parts + 1] = string.format("0x%02x", b)
	end
	local ok = HAL.exec(string.format("%s -f -y %d w%d@0x%02x %s",
		HAL.i2ctransfer, bus, #data, addr, table.concat(parts, " ")))
	if ok then return 0 end
	return -1
end

-- 写指针后读 n 字节（burst 读），返回大端合并值；失败返回 -1（n 钳制到 1~8，与 cfunc 一致）
function i2c_read_multi(fd, reg, n)
	local bus, addr = i2c_target(fd)
	if bus == nil then return -1 end
	local r = byte_of(reg)
	local cnt = uint_of(n)
	if r == nil or cnt == nil then return -1 end
	if cnt < 1 or cnt > 8 then cnt = 1 end

	local bytes = i2c_transfer_read(bus,
		string.format("w1@0x%02x 0x%02x r%d", addr, r, cnt))
	if bytes == nil or #bytes < cnt then return -1 end

	local v = 0
	for i = 1, cnt do
		v = v * 256 + bytes[i]
	end
	return v
end

-- 纯读 n 字节（不写指针，SGP30 这类命令式传感器用），返回大端合并值；失败返回 -1
function i2c_read(fd, n)
	local bus, addr = i2c_target(fd)
	if bus == nil then return -1 end
	local cnt = uint_of(n)
	if cnt == nil then return -1 end
	if cnt < 1 or cnt > 8 then cnt = 1 end

	local bytes = i2c_transfer_read(bus, string.format("r%d@0x%02x", cnt, addr))
	if bytes == nil or #bytes < cnt then return -1 end

	local v = 0
	for i = 1, cnt do
		v = v * 256 + bytes[i]
	end
	return v
end

------------------------------------------------------------------
-- 延时
------------------------------------------------------------------
-- 毫秒级延时（原 cfunc 用仓颉 sleep，此处用 sleep 命令，精度 ±数 ms）
function milli_sleep(ms)
	local n = tonumber(ms)
	if n == nil or n <= 0 then return end
	HAL.exec(string.format("%s %.3f", HAL.sleep_bin, n / 1000.0))
end

------------------------------------------------------------------
-- SPI
------------------------------------------------------------------
local SPI_CONF_DIR = "/tmp/bot4cj-hal"
local SPI_DEFAULT_SPEED = 1000000	-- 配置文件缺失时的保守速率（1MHz）

local function spi_conf_path(b, c)
	return SPI_CONF_DIR .. "/spidev" .. b .. "." .. c .. ".conf"
end

-- 读取 SPI 配置（本状态机缓存；配置文件由 spi_setup 所在状态机写入）
local function spi_get_conf(fd)
	local key = uint_of(fd)
	if key == nil then return nil end
	if spi_conf_cache[key] ~= nil then return spi_conf_cache[key] end

	local b, c = spi_decode(key)
	if b == nil then return nil end

	local f = io.open(spi_conf_path(b, c), "r")
	if f == nil then
		print("HAL: spi conf not found for /dev/spidev" .. b .. "." .. c ..
		      " (spi_setup not called?), fallback speed " .. SPI_DEFAULT_SPEED)
		spi_conf_cache[key] = { speed = SPI_DEFAULT_SPEED }
		return spi_conf_cache[key]
	end
	local line = f:read("l")
	f:close()
	local speed = tonumber(line)
	if speed == nil or speed <= 0 then speed = SPI_DEFAULT_SPEED end
	spi_conf_cache[key] = { speed = math.floor(speed) }
	return spi_conf_cache[key]
end

-- 内部：解码句柄并重构设备路径（绝不使用调用方字符串）
local function spi_target(fd)
	local key = uint_of(fd)
	if key == nil then return nil end
	local b, c = spi_decode(key)
	if b == nil then return nil end
	local conf = spi_get_conf(key)
	if conf == nil then return nil end
	return "/dev/spidev" .. b .. "." .. c, conf.speed
end

-- 打开 SPI 设备，成功返回句柄（>0），失败返回 -1
function spi_open(path)
	if type(path) ~= "string" then return -1 end
	local b, c = string.match(path, "^/dev/spidev(%d+)%.(%d+)$")
	b, c = tonumber(b), tonumber(c)
	if b == nil or c == nil then
		print("HAL: invalid spi path: " .. tostring(path))
		return -1
	end
	local f = io.open(path, "rb")
	if f == nil then
		print("HAL: spi device not found: " .. path)
		return -1
	end
	f:close()
	return spi_encode(b, c)
end

function spi_close(fd)
	local key = uint_of(fd)
	if key == nil then return end
	spi_conf_cache[key] = nil
	local b, c = spi_decode(key)
	if b ~= nil then
		os.remove(spi_conf_path(b, c))
	end
end

-- 配置 SPI 设备，返回 0 = 成功，-1 = 失败
-- mode/bits 经 spi-config 写入内核（实测跨进程持久）；speed 不持久，
-- 写入配置文件供各状态机的传输函数取用；delay 无对应物，忽略。
function spi_setup(fd, mode, bits, speed, delay)
	local key = uint_of(fd)
	if key == nil then return -1 end
	local b, c = spi_decode(key)
	if b == nil then return -1 end

	local m = uint_of(mode)
	local bt = uint_of(bits)
	local sp = uint_of(speed)
	if m == nil or m > 3 or bt == nil or bt < 7 or sp == nil or sp <= 0 then
		print("HAL: spi_setup invalid args: mode=" .. tostring(mode) ..
		      " bits=" .. tostring(bits) .. " speed=" .. tostring(speed))
		return -1
	end
	if tonumber(delay) ~= nil and tonumber(delay) > 0 then
		print("HAL: spi_setup delay is not supported by spi-tools, ignored")
	end

	local dev = "/dev/spidev" .. b .. "." .. c
	local ok = HAL.exec(string.format("%s -d %s -m %d -b %d",
		HAL.spi_config, dev, m, bt))
	if not ok then return -1 end

	-- 写配置文件（跨状态机传递 speed）
	HAL.exec("mkdir -p " .. SPI_CONF_DIR)
	local f = io.open(spi_conf_path(b, c), "w")
	if f == nil then return -1 end
	f:write(tostring(sp), "\n")
	f:close()
	spi_conf_cache[key] = { speed = sp }
	return 0
end

-- 内部：单次 spi-pipe 事务。in_bytes 为要发送的字节串（nil 表示用 /dev/zero 填充），
-- n 为传输字节数；返回 stdout 文本（读事务）或 ok（写事务）
local function spi_xfer(dev, speed, n, in_bytes)
	local pipe = string.format("%s -d %s -s %d -b %d -n 1",
		HAL.spi_pipe, dev, speed, n)
	if in_bytes == nil then
		-- 读事务：stdin 用 /dev/zero 填充 dummy 字节，stdout 经 od 转十进制
		local p = HAL.popen_r(pipe .. " < /dev/zero | " .. HAL.od_bin .. " -An -tu1 2>/dev/null")
		if p == nil then return nil end
		local out = p:read("a")
		local ok = p:close()
		if ok ~= true and ok ~= 0 then return nil end
		return out
	else
		-- 写事务：字节经 stdin 写入，MISO 丢弃
		local p = HAL.popen_w(pipe .. " > /dev/null 2>/dev/null")
		if p == nil then return false end
		p:write(in_bytes)
		p:flush()
		local ok = p:close()
		return (ok == true or ok == 0)
	end
end

-- 内部：解析 od -An -tu1 输出为字节数组
local function parse_dec_bytes(out)
	local bytes = {}
	if type(out) ~= "string" then return bytes end
	for d in string.gmatch(out, "%d+") do
		bytes[#bytes + 1] = tonumber(d)
	end
	return bytes
end

-- 写 1 字节，返回 0 = 成功，-1 = 失败
function spi_write(fd, val)
	local dev, speed = spi_target(fd)
	if dev == nil then return -1 end
	local v = byte_of(val)
	if v == nil then return -1 end
	if spi_xfer(dev, speed, 1, string.char(v)) then return 0 end
	return -1
end

-- 读 1 字节（发一个 dummy 字节），返回 0~255；失败返回 -1
function spi_read(fd)
	local dev, speed = spi_target(fd)
	if dev == nil then return -1 end
	local out = spi_xfer(dev, speed, 1, nil)
	local bytes = parse_dec_bytes(out)
	if #bytes < 1 then return -1 end
	return bytes[1]
end

-- 一次写 3 字节（单次传输，CS 保持低），返回 0 = 成功，-1 = 失败
function spi_write3(fd, b0, b1, b2)
	local dev, speed = spi_target(fd)
	if dev == nil then return -1 end
	local v0, v1, v2 = byte_of(b0), byte_of(b1), byte_of(b2)
	if v0 == nil or v1 == nil or v2 == nil then return -1 end
	if spi_xfer(dev, speed, 3, string.char(v0, v1, v2)) then return 0 end
	return -1
end

-- 一次读 n 字节（单次传输），返回大端合并值；失败返回 -1（n 钳制到 1~8）
function spi_read_multi(fd, n)
	local dev, speed = spi_target(fd)
	if dev == nil then return -1 end
	local cnt = uint_of(n)
	if cnt == nil then return -1 end
	if cnt < 1 or cnt > 8 then cnt = 1 end
	local out = spi_xfer(dev, speed, cnt, nil)
	local bytes = parse_dec_bytes(out)
	if #bytes < cnt then return -1 end
	local v = 0
	for i = 1, cnt do
		v = v * 256 + bytes[i]
	end
	return v
end

return HAL
