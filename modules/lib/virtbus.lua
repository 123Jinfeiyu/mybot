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
-- 语义对齐 HAL（返回值契约逐函数与 HAL.lua 相同，驱动用 `if not` / `==0` 等
-- 不同判定时行为才不会在虚拟总线上失真）：
--   i2c_write_reg       → dev.write(...)，返回布尔 true=成功/false=NACK（HAL 契约）
--   i2c_write_multi/16  → dev.write(...)，返回 0=成功/-1=NACK
--   指针式读族（i2c_read_reg/16/multi/multi_bytes）→ 先 dev.write(指针字节表)
--     再 dev.read(n)，复刻 HAL I2C_RDWR 的「写指针+读」原子事务——模型据此
--     知道读的是哪个寄存器（regmap 类器件模型的前提）；指针写 NACK 则整笔读失败
--   纯读族（i2c_read/read_bytes，无指针命令式器件用）→ 仅 dev.read(n)
--   长度约束同 HAL：read/read_multi/read_reg16 越界（合并值精度上限）显式失败；
--   read_bytes/read_multi_bytes 钳制 n 1~16（表长对调用方可见）
--   返回值按 HAL 原语义组装（大端合并或字节表）；无器件地址命中按 NACK 处理
--   （写 false/-1、读 -1/nil），与真机"设备不在总线"行为一致，可测负路径
--
-- 器件模型故障注入契约：
--   dev.write(bytes) 显式 return false → 本笔写按 NACK 处理（无返回/return nil = 成功）
--   dev.read(n) 返回 nil               → 本笔读按 NACK 处理
--
-- 设备模型库：modules/lib/virtbus/（regmap.lua 通用寄存器组、bh1750.lua 等现成模型）。
-- 加载真实驱动建议用 vbus.load_driver(path)：驱动首次 dofile HAL 会覆盖注入的全局
-- i2c_*，本助手自动重注入（test_virtbus_bh1750 早期手动 restore+inject 的封装）。
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

-- 挂载器件模型：name 仅日志用；dev.write(bytes)/dev.read(n) 缺省为空实现。
-- 保存模型引用并在每笔事务时动态取函数：测试既可以替换模型字段（如 dev.nack=true，
-- 函数闭包读到新值），也可以直接换函数（fake_dev.write = ...），attach 后均生效。
function virtbus.attach(name, addr, dev)
	devs[addr] = {
		name = name,
		model = dev,
		write = function(bytes)
			if dev.write then return dev.write(bytes) end
		end,
		read = function(n)
			if dev.read then return dev.read(n) end
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
-- 模型可显式 return false 模拟本笔写 NACK（返回 nil/无返回按成功）
local function do_write(fd, bytes)
	log_tx("w", fd_addr[fd], bytes)
	local dev = dev_of(fd)
	if dev == nil then return -1 end
	if dev.write(bytes) == false then return -1 end
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

-- 注意：HAL 的 i2c_write_reg 返回布尔（`i2c_rdwr(...) >= 0`），驱动普遍用
-- `if not i2c_write_reg(...)` 判定；不能像 write_multi 那样返回 0/-1
-- （Lua 中 -1 为真，NACK 会被驱动当成功——假绿）。
function virtbus.v_i2c_write_reg(fd, reg, val)
	return do_write(fd, { reg, val }) == 0
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

-- ---- 读族：n 由调用方按 HAL 契约校验/钳制后传入 ----
local function do_read(fd, n)
	local dev = dev_of(fd)
	log_tx("r", fd_addr[fd], { n = n })
	if dev == nil then return nil end
	return dev.read(n)
end

-- 正整数校验（合并值类接口用）：非法/越界返回 nil
local function read_n(n, maxv)
	n = tonumber(n)
	if n == nil or n ~= math.floor(n) or n < 1 or n > maxv then return nil end
	return n
end

-- 字节表类接口：钳到 1~16（对齐 HAL i2c_read_multi_bytes/read_bytes）
local function clamp_n16(n)
	n = tonumber(n)
	if n == nil then n = 1 end
	n = math.floor(n)
	if n < 1 then n = 1 end
	if n > 16 then n = 16 end
	return n
end

-- ---- 指针式读：先把寄存器指针作为一笔写事务发给器件，再读 ----
-- 对齐 HAL：真机上 i2c_read_reg/multi/reg16 是 I2C_RDWR 的「写指针消息 +
-- 读消息」单原子事务；旧实现把指针参数丢弃，模型无从知道读哪个寄存器，
-- 寄存器指针器件（MPU6050/BME280…）无法建模。指针写 NACK（无器件/模型
-- 注入故障）则整笔读失败且不产生读事务——与真机 ioctl 失败一致。
local function do_read_ptr(fd, ptr_bytes, n)
	if do_write(fd, ptr_bytes) ~= 0 then return nil end
	return do_read(fd, n)
end

local function be_merge(t)
	local v = 0
	for i = 1, #t do v = v * 256 + t[i] end
	return v
end

function virtbus.v_i2c_read_reg(fd, reg)
	reg = tonumber(reg)
	if reg == nil then return -1 end
	local t = do_read_ptr(fd, { math.floor(reg) % 256 }, 1)
	if t == nil or #t < 1 then return -1 end
	return t[1]
end

-- 纯读大端合并：HAL 限定 n 1~8，越界显式 -1（不静默钳制）
function virtbus.v_i2c_read(fd, n)
	n = read_n(n, 8)
	if n == nil then return -1 end
	local t = do_read(fd, n)
	if t == nil or #t < 1 then return -1 end
	return be_merge(t)
end

-- 指针读大端合并：HAL 限定 n 1~6（2^53 精度上限）
function virtbus.v_i2c_read_multi(fd, reg, n)
	reg = tonumber(reg)
	n = read_n(n, 6)
	if reg == nil or n == nil then return -1 end
	local t = do_read_ptr(fd, { math.floor(reg) % 256 }, n)
	if t == nil or #t < 1 then return -1 end
	return be_merge(t)
end

-- 指针读字节表：HAL 钳制 n 1~16
function virtbus.v_i2c_read_multi_bytes(fd, reg, n)
	reg = tonumber(reg)
	if reg == nil then return nil end
	n = clamp_n16(n)
	local t = do_read_ptr(fd, { math.floor(reg) % 256 }, n)
	if t == nil then return nil end
	return t
end

-- 16 位指针读大端合并：HAL 限定 n 1~6
function virtbus.v_i2c_read_reg16(fd, reg16, n)
	reg16 = tonumber(reg16)
	n = read_n(n, 6)
	if reg16 == nil or reg16 < 0 or reg16 > 0xFFFF or n == nil then return -1 end
	local r = math.floor(reg16)
	local t = do_read_ptr(fd, { math.floor(r / 256) % 256, r % 256 }, n)
	if t == nil or #t < 1 then return -1 end
	return be_merge(t)
end

-- 纯读字节表：HAL 钳制 n 1~16
function virtbus.v_i2c_read_bytes(fd, n)
	n = clamp_n16(n)
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

-- 加载真实驱动文件并确保虚拟总线处于注入态。
-- 驱动顶部首次 dofile HAL.lua 会用真实 ffi 函数定义全局 i2c_*，把先前的
-- 虚拟注入覆盖掉；故加载后强制重注入。先加载再注入时，originals 捕获的是
-- 真实 HAL 函数，restore()/reset() 能真正还原 HAL（而不是把全局置空）。
-- 约定：一个 Lua 状态机内只走一次「加载驱动 + 注入」流程——多驱动并存请先
-- dofile 全部驱动再统一注入（run_all.sh 每个用例本就是独立 luajit 进程）。
function virtbus.load_driver(path)
	dofile(path)
	virtbus._injected = false
	virtbus.inject()
	return virtbus
end

return virtbus