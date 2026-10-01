-- Sim 仿真器桥接驱动模块
-- 把语义动作以行文本发给 mybot-sim（sim/ 下的 GLFW 蜘蛛仿真器）监听的
-- AF_UNIX socket。仿真器收到后让蜘蛛持续执行该动作，直到下一条指令。
--
-- 只传「动作 + 速度」，**不传关节角**：蜘蛛的腿部步态由仿真器自己按身体
-- 速度生成。上游（.mt 动作脚本 / agent）只表达「前进、后退、左转、右转、停」。
--
-- 协议（与 sim/src/bridge.cpp 逐字对应）：
--   forward | backward | left | right | stop   [速度 0.00~1.00]
-- 每条一行，以 \n 结尾；速度缺省 1.0。手测：
--   socat - UNIX-CONNECT:/tmp/mybot-sim.sock
--
-- .mt 侧用**数字动作码**而非动作名：矩阵单元格只解析 Float64
-- （matrix.cpp 对 'forward' 这类字符串直接报 cannot parse matrix cell），
-- 语义名字只存在于 socket 线上与下面的 CODES 表里：
--   0=stop  1=forward  2=backward  3=left  4=right
--
-- socket 路径取环境变量 MYBOT_SIM_SOCK，缺省 /tmp/mybot-sim.sock——与
-- sim/src/main.cpp 的取值同源，两侧不用各自配置。
--
-- 成败口径（刻意不对称）：
--   * 仿真器没开 / 中途断开 = **静默成功**。可视化从端缺席不该打断动作脚本，
--     真机路径也不该被一个调试窗口绑架。每次 call 都会重试连接，仿真器中途
--     起来即接上；BOT4CJ_DEBUG=1 时把丢弃动作打印出来。
--   * 动作名非法 = 响亮报错（ERR 契约）。那是脚本作者的笔误，不是环境问题，
--     静默丢弃会让 .mt 里的错字永远查不出来。
--
-- 标准前导：print_table 经 mpath 加载（modules/lib/）
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
dofile(mpath.resolve("lib/print_table.lua"))
dofile(mpath.resolve("lib/drv_common.lua"))

local ffi = require("ffi")

-- 结构体名带 cj_ 前缀：hal_backend_glibc.lua 在同一 Lua 状态里已 cdef 过一批
-- 类型，LuaJIT 对同名 struct 的重复 cdef 是硬错误（函数同签名重复则允许）。
ffi.cdef[[
typedef unsigned short cj_sa_family_t;
struct cj_sockaddr_un { cj_sa_family_t sun_family; char sun_path[108]; };
int socket(int domain, int type, int protocol);
int connect(int fd, const struct cj_sockaddr_un *addr, unsigned int addrlen);
long send(int fd, const void *buf, unsigned long len, int flags);
int close(int fd);
]]

local AF_UNIX = 1
local SOCK_STREAM = 1
-- 对端没了时 send 默认会发 SIGPIPE 直接打死整个 mybot 进程；MSG_NOSIGNAL
-- 把它折成 EPIPE 返回值，走下面的「断开重连」口径
local MSG_NOSIGNAL = 0x4000

local kDefaultSock = "/tmp/mybot-sim.sock"
-- sun_path 定长 108，留一位给 NUL；超长会被静默截断成另一个路径，显式拒绝
local kMaxPathLen = 107

-- .mt 动作码 → 线上动作名。下标即 .sql 注释里登记的动作码。
local CODES = {
	[0] = "stop",
	[1] = "forward",
	[2] = "backward",
	[3] = "left",
	[4] = "right",
}
local CODE_MAX = 4

-- 每个 Lua 状态（= 每个设备实例）一份连接，跨 call 复用
local fd = nil
local sock_path = nil

local function sockPath()
	if sock_path == nil then
		local p = os.getenv("MYBOT_SIM_SOCK")
		if p == nil or p == "" then
			p = kDefaultSock
		end
		sock_path = p
	end
	return sock_path
end

local function dropConnection()
	if fd ~= nil then
		ffi.C.close(fd)
		fd = nil
	end
end

-- 惰性连接：build 时不连——仿真器常常后起，连接失败也不该让结构脚本 build 红
local function ensureConnected()
	if fd ~= nil then
		return true
	end

	local path = sockPath()
	if #path > kMaxPathLen then
		return false
	end

	local s = ffi.C.socket(AF_UNIX, SOCK_STREAM, 0)
	if s < 0 then
		return false
	end

	local addr = ffi.new("struct cj_sockaddr_un")
	addr.sun_family = AF_UNIX
	ffi.copy(addr.sun_path, path)	-- 字符串形态的 ffi.copy 会带上结尾 NUL
	-- addrlen = family(2) + 路径字节 + NUL；按实际长度传，别传满 110
	local addrlen = 2 + #path + 1

	if ffi.C.connect(s, addr, addrlen) ~= 0 then
		ffi.C.close(s)
		return false
	end

	fd = s
	return true
end


-- 【配置脚本】不申请资源，连接惰性到首次 call
function build(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	return {  result = "OK"  }

end


-- 【销毁Sim】关掉可能还开着的 socket
function destroy(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dropConnection()

	return {  result = "OK"  }

end


-- 【调用Sim】发一条语义动作
-- 元数据列：无（socket 路径走 MYBOT_SIM_SOCK，见文件头）
-- 输入参数：action（forward/backward/left/right/stop）、speed（可选 0~1）
function call(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	local code = finite_of(tonumber(data.action))
	if code == nil then
		return err("Invalid Argument: action (missing or not a number)")
	end
	-- 动作码必须是 0..4 的整数：矩阵是 Float64，1.5 这种值只会是脚本笔误
	if code ~= math.floor(code) or code < 0 or code > CODE_MAX then
		return err("Invalid Argument: action " .. tostring(code)
			.. " (expected 0=stop 1=forward 2=backward 3=left 4=right)")
	end
	local action = CODES[code]

	local speed = 1.0
	if data.speed ~= nil then
		local s = finite_of(tonumber(data.speed))
		if s == nil then
			return err("Invalid Argument: speed (not a finite number)")
		end
		if s < 0 then s = 0 elseif s > 1 then s = 1 end
		speed = s
	end

	local line = action
	if speed ~= 1.0 then
		line = action .. " " .. string.format("%.2f", speed)
	end
	line = line .. "\n"

	if not ensureConnected() then
		if os.getenv("BOT4CJ_DEBUG") == "1" then
			print("Sim: simulator not listening on " .. sockPath()
				.. " (action '" .. action .. "' dropped)")
		end
		return {  result = "OK"  }
	end

	local n = ffi.C.send(fd, line, #line, MSG_NOSIGNAL)
	if n < 0 then
		-- 对端没了：断开并按「仿真器缺席」口径静默成功，下次 call 重连
		dropConnection()
		return {  result = "OK"  }
	end

	if os.getenv("BOT4CJ_DEBUG") == "1" then
		print("Sim: sent '" .. line:sub(1, -2) .. "' to " .. sockPath())
	end

	return {  result = "OK"  }

end
