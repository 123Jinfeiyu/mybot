-- virtbus.lua —— 虚拟 I2C 总线（issue #18：设计虚拟硬件用于无真机驱动链路验证）
-- bot4cj 仓颉语言机器人框架
--
-- 概念：inject 后替换 HAL 的全部全局 i2c_* 函数，真实驱动（经 HAL/驱动 dofile
-- 链路）在纯内存的虚拟总线上完成 build/call/destroy——无真机、无 /dev/i2c-*。
-- 器件行为由 attach 时提供的模型回调决定，事务序列记入 txlog 供断言。
--
-- 用法：
--   local vbus = dofile(modulePath .. "/lib/virtbus.lua")
--   vbus.attach("光感@0x23", 0x23, {
--       write = function(bytes) ... end,   -- 收命令/寄存器写（bytes 为 1 基表）
--       read  = function(n) return { 0x12, 0x34 } end,  -- 返回 n 字节（1 基表）
--   })
--   vbus.inject()          -- 替换全局 i2c_*，此后驱动调用全走虚拟总线
--   -- ... 正常 dofile 驱动、传 build/call/destroy 数据 ...
--   vbus.txlog             -- { {f="open",dir="w",addr=,bytes=}, ... } 事务序列（断言用）
--   vbus.reset()           -- 清空器件/日志/线索并 restore
--   vbus.restore()         -- 还原原始 i2c_* 全局函数
--
-- 语义对齐 HAL：
--   写族（i2c_write_reg/16/multi）→ dev.write(完整字节表)，返回 0=成功/-1=NACK
--   读族（i2c_read_reg/16/multi/multi_bytes/read/read_bytes）→ dev.read(n)，
--     按 HAL 原语义组返回值（大端合并或字节表）；无器件地址命中按 NACK 处理
--   （写 -1、读 -1/nil），与真机"设备不在总线"的行为一致，可测负路径
--
-- 本文件为纯 Lua（不依赖 ffi），可用 lua5.4/luajit 离线跑。
local virtbus = {}
virtbus._VERSION = "0.1.0"

local devs = {}          -- addr -> { name=, write=, read= } 器件模型
local txlog = {}         -- 事务序列
local originals = {}     -- 原始全局函数（restore 用）
local next_fd = 3
local fd_addr = {}       -- fd -> addr（i2c_set_addr 记录）
local open_paths = {}    -- fd -> path
virtbus.txlog = txlog
virtbus.devs = devs

-- 复位：清空器件/日志/线索并还原全局 i2c_*
function virtbus.reset()
	virtbus.restore()
	devs = {}
	txlog = {}
	fd_addr = {}
	open_paths = {}
	next_fd = 3
	virtbus.txlog = txlog
	virtbus.devs = devs
end

-- 挂载器件模型：name 仅日志用；dev.write(bytes)/dev.read(n) 缺省为空实现
function virtbus.attach(name, addr, dev)
	devs[addr] = {
		name = name,
		write = dev.write or function() end,
		read = dev.read or function(n)
			local t = {}
			for i = 1, n do t[i] = 0 end
			return t
		end,
	}
end

-- ---- 内部：事务记录 ----
local function log_tx(dir, addr, bytes)
	txlog[#txlog + 1] = { dir = dir, addr = addr, bytes = bytes }
end

local function dev_of(fd)
	local addr = fd_addr[fd]
	if addr == nil or devs[addr] == nil then return nil end
	return devs[addr]
end

-- ---- 写族：组合字节表后交给器件，返回 0 = 成功，-1 = NACK ----
local function do_write(fd, bytes)
	log_tx("w", fd_addr[fd], bytes)
	local dev = dev_of(fd)
	if dev == nil then return -1 end
	dev.write(bytes)
	return 0
end

function virtbus.v_i2c_open(path)
	-- 与 HAL 相同的路径白名单校验；合法即分配虚拟 fd
	if type(path) ~= "string" or string.match(path, "^/dev/i2c%-%d+$") == nil then
		return -1
	end
	local fd = next_fd
	next_fd = next_fd + 1
	open_paths[fd] = path
	log_tx("open", nil, { raw = path })
	return fd
end

function virtbus.v_i2c_close(fd)
	fd = tonumber(fd)
	if fd ~= nil and open_paths[fd] ~= nil then
		open_paths[fd] = nil
		fd_addr[fd] = nil
	end
end

function virtbus.v_i2c_set_addr(fd, addr)
	fd = tonumber(fd)
	addr = tonumber(addr)
	if fd == nil or open_paths[fd] == nil or addr == nil or addr > 0x7F then
		return false
	end
	fd_addr[fd] = addr
	log_tx("set", addr, {})
	return true
end

function virtbus.v_i2c_write_reg(fd, reg, val)
	return do_write(fd, { reg, val })
end

function virtbus.v_i2c_write_multi(fd, reg, ...)
	local bytes = { reg }
	for _, v in ipairs({ ... }) do bytes[#bytes + 1] = v end
	return do_write(fd, bytes)
end

function virtbus.v_i2c_write_reg16(fd, reg16, ...)
	local bytes = { math.floor(reg16 / 256) % 256, reg16 % 256 }
	for _, v in ipairs({ ... }) do bytes[#bytes + 1] = v end
	return do_write(fd, bytes)
end

-- ---- 读族：调器件 read(n)，再按 HAL 语义组返回值 ----
local function do_read(fd, n)
	n = tonumber(n)
	if n == nil or n < 1 then n = 1 end
	local dev = dev_of(fd)
	log_tx("r", fd_addr[fd], { n = n })
	if dev == nil then return nil end
	return dev.read(n)
end

local function be_merge(t)
	local v = 0
	for i = 1, #t do v = v * 256 + t[i] end
	return v
end

function virtbus.v_i2c_read_reg(fd, reg)
	local t = do_read(fd, 1)
	if t == nil or #t < 1 then return -1 end
	return t[1]
end

function virtbus.v_i2c_read(fd, n)
	local t = do_read(fd, n)
	if t == nil or #t < 1 then return -1 end
	return be_merge(t)
end

function virtbus.v_i2c_read_multi(fd, reg, n)
	local t = do_read(fd, n)
	if t == nil or #t < 1 then return -1 end
	return be_merge(t)
end

function virtbus.v_i2c_read_multi_bytes(fd, reg, n)
	local t = do_read(fd, n)
	if t == nil then return nil end
	return t
end

function virtbus.v_i2c_read_reg16(fd, reg16, n)
	local t = do_read(fd, n)
	if t == nil or #t < 1 then return -1 end
	return be_merge(t)
end

function virtbus.v_i2c_read_bytes(fd, n)
	local t = do_read(fd, n)
	if t == nil then return nil end
	return t
end

-- 替换全局 i2c_* 函数为虚拟实现（幂等：已注入则跳过）
local REPLACED = {
	"i2c_open", "i2c_close", "i2c_set_addr",
	"i2c_write_reg", "i2c_write_multi", "i2c_write_reg16",
	"i2c_read_reg", "i2c_read", "i2c_read_multi", "i2c_read_multi_bytes",
	"i2c_read_reg16", "i2c_read_bytes",
}

function virtbus.inject()
	if virtbus._injected then return end
	for _, name in ipairs(REPLACED) do
		originals[name] = _G[name]
	end
	_G.i2c_open = virtbus.v_i2c_open
	_G.i2c_close = virtbus.v_i2c_close
	_G.i2c_set_addr = virtbus.v_i2c_set_addr
	_G.i2c_write_reg = virtbus.v_i2c_write_reg
	_G.i2c_write_multi = virtbus.v_i2c_write_multi
	_G.i2c_write_reg16 = virtbus.v_i2c_write_reg16
	_G.i2c_read_reg = virtbus.v_i2c_read_reg
	_G.i2c_read = virtbus.v_i2c_read
	_G.i2c_read_multi = virtbus.v_i2c_read_multi
	_G.i2c_read_multi_bytes = virtbus.v_i2c_read_multi_bytes
	_G.i2c_read_reg16 = virtbus.v_i2c_read_reg16
	_G.i2c_read_bytes = virtbus.v_i2c_read_bytes
	virtbus._injected = true
end

function virtbus.restore()
	if not virtbus._injected then return end
	for _, name in ipairs(REPLACED) do
		_G[name] = originals[name]
	end
	originals = {}
	virtbus._injected = false
end

return virtbus