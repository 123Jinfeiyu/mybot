-- hal_backend_glibc.lua - HAL 系统调用后端：glibc（LuaJIT ffi 直调）
-- fix-plan 2026-09-20 阶段 A-1（外部评估 2026-09-20 扣分点「平台与运行时绑定」整改）：
-- 原 HAL.lua 内联的 HAL.sys 函数表原样迁入，成为可替换后端；
-- 换后端（仿真桩 / 其它 libc）= 新增 modules/lib/hal_backend_<名>.lua，
-- HAL 总线函数与驱动零改动。
--
-- 【后端接口契约】BOT4CJ_HAL_BACKEND 指定的后端必须返回实现以下函数集的表
--（HAL.lua 加载时会逐项断言，缺函数即 fail-fast）：
--   open(path, flags[, mode])  -> fd(>=0)；失败 -1
--   close(fd)                  -> 0 成功 / -1 失败
--   ioctl_i(fd, req, val)      -> int（val 按 unsigned long cast，JIT/解释 ABI 一致）
--   ioctl_p(fd, req, ptr)      -> int（ptr 为 cdata 指针）
--   read(fd, buf, n)           -> 读到字节数(>=0) / -1
--   write(fd, buf, n)          -> 写入字节数(>=0) / -1
--   usleep(us)                 -> nil（调用方不依赖返回值）
--   errno()                    -> 最近一次系统调用的 errno
--   version()                  -> libc 版本串（如 "2.42"）；不可知返回 nil
--   tcgetattr/tcsetattr/cfmakeraw/cfsetispeed/cfsetospeed/tcflush
--                              （UART termios 六件，同 glibc 语义；termios 以
--                               void* 传递避免分架构布局差异）
--
-- 【加载时序】本文件由 HAL.lua 在 ffi.cdef 声明（glibc + spidev/i2c 结构体）
-- 之后经 dofile 加载，glibc 后端直接使用 HAL 已声明的 cdef——不经 HAL.lua
-- 单独加载本文件不保证可用。
--
-- 【返回码契约】HAL 层「失败 = -1 / nil / false」三种风格并存是既有契约
--（i2c_write_reg 返布尔、write_multi 返 0/-1……virtbus.lua:319-322 已记载
-- 「-1 在 Lua 中为真值」的陷阱），后端不得私自统一口径，以免静默改变
-- 上层判据——这正是 bus_contract 单点（N-25）要防的同类漂移。
local has_ffi, ffi = pcall(require, "ffi")
if not has_ffi then
	error("hal_backend_glibc requires LuaJIT ffi")
end

-- glibc 版本探测：gnu_get_libc_version 是 glibc 专有符号（musl 没有），
-- 非 glibc 平台 version() 返 nil，上层波特率编码探测按旧 ABI 编码处理
-- （HAL.lua UART 段与 test_uart_consts.c 三重把关）。
-- 2026-09-25 musl 实证修正：cdef 只是声明、永远成功，符号缺失是在 **ffi.C 取值时**
-- 抛「Symbol not found」——此前 pcall 只包住 cdef，musl 下 version() 直接崩，
-- 与本文件顶部契约「不可知返回 nil」相悖。故解析符号的那一步也放进 pcall。
pcall(ffi.cdef, [[ const char* gnu_get_libc_version(void); ]])
local function libc_version()
	local ok, p = pcall(function() return ffi.C.gnu_get_libc_version() end)
	if not ok or p == nil then return nil end
	return ffi.string(p)
end

local M = {
	open    = function(path, flags, mode)
		if mode ~= nil then
			return ffi.C.open(path, flags, ffi.cast("unsigned int", mode))
		end
		return ffi.C.open(path, flags)
	end,
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
	-- UART termios（glibc 直接调用；termios 以 void* 传递避免分架构布局差异）
	tcgetattr = function(fd, ptr)   return ffi.C.tcgetattr(fd, ptr) end,
	tcsetattr = function(fd, act, ptr) return ffi.C.tcsetattr(fd, act, ptr) end,
	cfmakeraw = function(ptr)       ffi.C.cfmakeraw(ptr) end,
	cfsetispeed = function(ptr, baud) return ffi.C.cfsetispeed(ptr, baud) end,
	cfsetospeed = function(ptr, baud) return ffi.C.cfsetospeed(ptr, baud) end,
	tcflush   = function(fd, q)     return ffi.C.tcflush(fd, q) end,
	version   = libc_version,
}
return M
