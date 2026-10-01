-- test_sim.lua - Sim 仿真桥接驱动守门（modules/Sim.lua）
-- 验证四件事：
--   ① ERR 契约：nil / 缺参 / 非法动作码 / 非法速度都返 ERR 表（不抛错、不静默成功）；
--   ② 仿真器缺席 = 静默成功：socket 没人听时 call 仍返 OK——可视化从端不该绑架
--      动作脚本；
--   ③ 线上协议：本用例自己起一个 AF_UNIX 监听端，逐字核对 Sim.lua 发出来的行
--      （动作名 + 速度 + \n），与 sim/src/bridge.cpp 的解析口径互证；
--   ④ 对端断开后自动重连，destroy 之后再 call 也能重连。
-- 全程无真实硬件；监听端用独立路径，与开发会话里可能开着的真仿真器互不干扰。
dofile("test/lib/tester.lua")
local tester = _G.tester
local ffi = require("ffi")

-- Sim.lua 的 socket 路径读 MYBOT_SIM_SOCK；LuaJIT 没有 os.setenv，经 ffi 设，
-- 指到一条确定没人用的路径。必须在 dofile Sim.lua 之前设好（路径首次 call 时缓存）。
ffi.cdef[[
typedef unsigned short t_sa_family_t;
struct t_sockaddr_un { t_sa_family_t sun_family; char sun_path[108]; };
int setenv(const char *name, const char *value, int overwrite);
int unlink(const char *path);
int bind(int fd, const struct t_sockaddr_un *addr, unsigned int addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct t_sockaddr_un *addr, unsigned int *addrlen);
long recv(int fd, void *buf, unsigned long len, int flags);
int fcntl(int fd, int cmd, ...);
]]

local TEST_SOCK = "/tmp/mybot-test-sim.sock"
ffi.C.setenv("MYBOT_SIM_SOCK", TEST_SOCK, 1)
ffi.C.unlink(TEST_SOCK)

dofile("modules/Sim.lua")

local function is_err(res)
	return type(res) == "table" and res.result == "ERR" and type(res.message) == "string"
end

local function call_of(data)
	local ok, res = pcall(call, data)
	return ok, res
end

-- ==================== 1. ERR 契约 ====================
do
	local ok, res = pcall(build, nil)
	tester.check("build(nil) 返 ERR 契约（不抛错）", ok and is_err(res), tostring(res))
	local okd, resd = pcall(destroy, nil)
	tester.check("destroy(nil) 返 ERR 契约（不抛错）", okd and is_err(resd), tostring(resd))

	local bad = {
		{ "call(nil)", nil },
		{ "缺 action", {} },
		{ "action 是动作名字符串（矩阵只收 Float64）", { action = "forward" } },
		{ "action 越界上", { action = 9 } },
		{ "action 越界下", { action = -1 } },
		{ "action 非整数", { action = 1.5 } },
		{ "action 是 NaN", { action = 0 / 0 } },
		{ "speed 非数值", { action = 1, speed = "abc" } },
		{ "speed 是 NaN", { action = 1, speed = 0 / 0 } },
	}
	for _, c in ipairs(bad) do
		local cok, cres = call_of(c[2])
		tester.check(c[1] .. " 返 ERR 契约（不抛错）", cok and is_err(cres),
			cok and tostring(cres and cres.result) or ("raised: " .. tostring(cres)))
	end
end

-- ==================== 2. 仿真器缺席 = 静默成功 ====================
do
	-- TEST_SOCK 此刻没有监听端
	local ok, res = call_of({ action = 1 })
	tester.check("无监听端时 call 静默成功", ok and type(res) == "table" and res.result == "OK",
		tostring(res and res.result))
	local ok2, res2 = call_of({ action = 0, speed = 0.5 })
	tester.check("无监听端时带速度的 call 亦静默成功",
		ok2 and type(res2) == "table" and res2.result == "OK", tostring(res2 and res2.result))
end

-- ==================== 3. 线上协议逐字核对 ====================
local AF_UNIX, SOCK_STREAM = 1, 1
local F_GETFL, F_SETFL, O_NONBLOCK = 3, 4, 2048

local function recv_line(cfd)
	local buf = ffi.new("char[256]")
	local n = ffi.C.recv(cfd, buf, 255, 0)
	if n <= 0 then return nil end
	return ffi.string(buf, n)
end

local lfd = ffi.C.socket(AF_UNIX, SOCK_STREAM, 0)
tester.check("测试监听端 socket()", lfd >= 0, tostring(lfd))

local addr = ffi.new("struct t_sockaddr_un")
addr.sun_family = AF_UNIX
ffi.copy(addr.sun_path, TEST_SOCK)
-- addrlen = family(2) + 路径字节 + NUL，与 Sim.lua 侧口径一致
tester.check("测试监听端 bind", ffi.C.bind(lfd, addr, 2 + #TEST_SOCK + 1) == 0, "bind 失败")
tester.check("测试监听端 listen", ffi.C.listen(lfd, 4) == 0, "listen 失败")

-- 监听端非阻塞：Sim 没连上来时 accept 返 EAGAIN，用例干净地红而不是挂到超时
local bit = require("bit")
local fl = ffi.C.fcntl(lfd, F_GETFL)
ffi.C.fcntl(lfd, F_SETFL, bit.bor(fl, O_NONBLOCK))

local cfd = -1
do
	local ok, res = call_of({ action = 1, speed = 0.5 })
	tester.check("有监听端时 call 成功", ok and type(res) == "table" and res.result == "OK",
		tostring(res and res.result))
	cfd = ffi.C.accept(lfd, nil, nil)
	tester.check("Sim 连上测试监听端", cfd >= 0, "accept 无连接：Sim 没连上来")
end

if cfd >= 0 then
	local line1 = recv_line(cfd)
	tester.check("首行逐字 = 'forward 0.50\\n'", line1 == "forward 0.50\n", tostring(line1))

	call({ action = 3 })
	local line2 = recv_line(cfd)
	tester.check("速度缺省时不写速度字段 = 'left\\n'", line2 == "left\n", tostring(line2))

	call({ action = 0 })
	local line3 = recv_line(cfd)
	tester.check("stop 行 = 'stop\\n'", line3 == "stop\n", tostring(line3))

	call({ action = 4, speed = 1 })
	local line4 = recv_line(cfd)
	tester.check("满速也走缺省口径 = 'right\\n'", line4 == "right\n", tostring(line4))

	-- ==================== 4. 对端断开后重连 ====================
	ffi.C.close(cfd)
	-- 第一条打在死 fd 上：EPIPE，按「仿真器缺席」口径静默丢弃
	call({ action = 2 })
	-- 第二条重连后才真的发出去
	call({ action = 2 })
	local cfd2 = ffi.C.accept(lfd, nil, nil)
	tester.check("对端断开后自动重连", cfd2 >= 0, "accept 无连接：没重连")
	if cfd2 >= 0 then
		local line5 = recv_line(cfd2)
		tester.check("重连后首行 = 'backward\\n'", line5 == "backward\n", tostring(line5))
		ffi.C.close(cfd2)
	end

	-- destroy 关掉连接后再 call 仍能重连
	local okd, resd = pcall(destroy, {})
	tester.check("destroy() 返 OK", okd and type(resd) == "table" and resd.result == "OK",
		tostring(resd and resd.result))
	call({ action = 4 })
	local cfd3 = ffi.C.accept(lfd, nil, nil)
	tester.check("destroy 后 call 重连", cfd3 >= 0, "accept 无连接：destroy 后没重连")
	if cfd3 >= 0 then
		local line6 = recv_line(cfd3)
		tester.check("destroy 后首行 = 'right\\n'", line6 == "right\n", tostring(line6))
		ffi.C.close(cfd3)
	end
end

ffi.C.close(lfd)
ffi.C.unlink(TEST_SOCK)

tester.summary("Sim 仿真桥接驱动")
