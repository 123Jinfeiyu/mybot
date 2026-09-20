-- test_hal_backend_swap.lua - HAL.sys 后端接口化守门（fix-plan 2026-09-20 阶段 A-4）
-- 验证：① 缺省加载 glibc 后端且契约函数集完整；② 整表替换 stub 后端即可
-- 跑通一条完整 i2c 事务（open → set_addr → write_reg → read_reg → close），
-- 证明接口契约完备——换后端不需要动 HAL 总线函数与驱动；③ 替换后恢复原样。
-- 全程无真实硬件（stub 内纯 Lua 记录）。
dofile("test/lib/tester.lua")
local tester = _G.tester

local HAL = dofile("modules/lib/HAL.lua")

-- ==================== 1. 缺省 glibc 后端 ====================
tester.check("ABI 类标记为 ffi", HAL.backend == "ffi", HAL.backend)
tester.check("后端模块名缺省 glibc", HAL.backend_name == "glibc", HAL.backend_name)
for _, k in ipairs({ "open", "close", "ioctl_i", "ioctl_p", "read", "write",
	"usleep", "errno", "version", "tcgetattr", "tcsetattr", "cfmakeraw",
	"cfsetispeed", "cfsetospeed", "tcflush" }) do
	tester.check("契约函数在位: " .. k, type(HAL.sys[k]) == "function", type(HAL.sys[k]))
end
local ver = HAL.sys.version()
tester.check("glibc 版本串形如 2.x（musl 等 nil 也接受）",
	ver == nil or tostring(ver):match("^%d+%.%d+$") ~= nil, tostring(ver))

-- ==================== 2. stub 后端整表替换 ====================
local calls = {}
local stub = {
	open    = function(path, flags)
		calls[#calls + 1] = { cmd = "open", path = path, flags = flags }
		return 7	-- 预置 fd
	end,
	close   = function(fd)
		calls[#calls + 1] = { cmd = "close", fd = fd }
		return 0
	end,
	ioctl_i = function() return -1 end,
	ioctl_p = function(fd, req, ptr)
		calls[#calls + 1] = { cmd = "ioctl_p", fd = fd, req = req }
		return 1	-- >=0 即事务成功
	end,
	read    = function() return -1 end,
	write   = function(_, _, n) return n end,
	usleep  = function() end,
	errno   = function() return 0 end,
	version = function() return "3.0" end,	-- 仿 musl：字面版本串
	tcgetattr = function() return 0 end,
	tcsetattr = function() return 0 end,
	cfmakeraw = function() end,
	cfsetispeed = function() end,
	cfsetospeed = function() end,
	tcflush   = function() end,
}
local saved_sys, saved_name = HAL.sys, HAL.backend_name
HAL.sys = stub

local fd = i2c_open("/dev/i2c-1")
tester.check("stub 后端下 i2c_open 返回预置 fd", fd == 7, tostring(fd))
tester.check("open 调用记录到 stub",
	calls[1] ~= nil and calls[1].cmd == "open" and calls[1].path == "/dev/i2c-1",
	calls[1] and tostring(calls[1].path))
tester.check("i2c_set_addr 走通", i2c_set_addr(fd, 0x68) == true)
tester.check("i2c_write_reg 原子事务成功（ioctl_p >= 0）", i2c_write_reg(fd, 0x6B, 0x00) == true)
tester.check("i2c_read_reg 读缓冲默认 0（事务成功路径）", i2c_read_reg(fd, 0x75) == 0)
local ioctl_req_ok = false
for _, w in ipairs(calls) do
	if w.cmd == "ioctl_p" and w.fd == 7 and w.req == HAL.I2C_RDWR then ioctl_req_ok = true end
end
tester.check("事务请求带 I2C_RDWR 编号且 fd 正确", ioctl_req_ok)
i2c_close(fd)
tester.check("i2c_close 经 stub 收口", calls[#calls] ~= nil and calls[#calls].cmd == "close"
	and calls[#calls].fd == 7, calls[#calls] and tostring(calls[#calls].cmd))

-- ==================== 3. 恢复原后端 ====================
HAL.sys, HAL.backend_name = saved_sys, saved_name
tester.check("恢复后 HAL.backend_name 还原", HAL.backend_name == "glibc", HAL.backend_name)
tester.check("恢复后契约函数仍为原实现", type(HAL.sys.version) == "function")

tester.summary("hal_backend_swap")
