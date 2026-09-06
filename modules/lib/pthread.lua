-- pthread.lua - 守护线程 IPC 原语（纯 LuaJIT ffi）
-- bot4cj 仓颉语言机器人框架
--
-- 用途：@Daemon 机制的数据通路。守护线程在【独立 lua_State】上采样，
-- 经共享内存槽 publish 结果；主线程侧 latest 读取。线程本身的创建在仓颉
-- 宿主侧（r.cj 经 pthread4cj），本库只负责跨状态机的共享内存 + 互斥，
-- 全部经 ffi 直调 libc，无仓颉 cfunc（守住 issue #14 的纯 Lua 数据通路）。
--
-- 【共享槽布局】必须与 r.cj 的 @C struct DaemonSlot 严格一致（同一 C ABI）：
--   offset 0   char    mutex[64]    pthread_mutex_t（aarch64=48B/x86_64=40B，预留 64）
--   offset 64  int32   count        已 publish 次数
--   offset 68  int32   nvals        本次值个数
--   offset 72  double  values[16]   采样值
--   offset 200 uint8   stop         停止标志（1=请求停止）
--
-- 【cdef 约定】只声明 mutex/slot 专有类型，不重复声明 HAL.lua 已定义的
-- open/ioctl/read/write/usleep（守护 chunk 会同时加载 HAL，重复 cdef 会报错）。
--
-- 用法（守护侧，独立状态机）：
--   local P = dofile(.../lib/pthread.lua)
--   local slot = P.slot_attach(DAEMON_SLOT_ADDR)   -- 宿主注入的地址全局
--   while not P.should_stop(slot) do
--       local ax, ay, az = 采样()                   -- 经 HAL
--       P.publish(slot, {ax, ay, az})
--       milli_sleep(interval_ms)                    -- HAL 提供的节拍
--   end
-- 用法（主线程侧读取，通常由仓颉直接读槽；Lua 侧亦可用 latest）：
--   local vals, count = P.latest(slot)

local P = {}

-- 重复加载保护
if _G.__BOT4CJ_PTHREAD ~= nil then return _G.__BOT4CJ_PTHREAD end
_G.__BOT4CJ_PTHREAD = P

local has_ffi, ffi = false, nil
if rawget(_G, "jit") ~= nil then has_ffi, ffi = pcall(require, "ffi") end
if not has_ffi then
	error("pthread.lua requires LuaJIT with ffi")
end

------------------------------------------------------------------
-- ffi 声明（仅 mutex/slot 专有，避开与 HAL 的重复符号）
------------------------------------------------------------------
ffi.cdef[[
typedef struct { char __m[64]; } cj_pthread_mutex_t;
int pthread_mutex_init(cj_pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_lock(cj_pthread_mutex_t *mutex);
int pthread_mutex_unlock(cj_pthread_mutex_t *mutex);
int pthread_mutex_destroy(cj_pthread_mutex_t *mutex);

typedef struct {
	char     mutex[64];
	int32_t  count;
	int32_t  nvals;
	double   values[16];
	uint8_t  stop;
} daemon_slot_t;
]]

P.SLOT_MAXVALS = 16

------------------------------------------------------------------
-- 地址 ↔ 指针
------------------------------------------------------------------
-- 整数地址（宿主以 Lua number 注入）→ 类型化指针。
-- 两步 cast：number → uint64_t → 目标指针类型（直接 number→pointer 不被允许）。
function P.ptr(addr, ctype)
	local u = ffi.cast("uint64_t", addr)
	return ffi.cast(ctype or "void *", u)
end

-- 指针 → 整数地址（用于把 ffi 分配的槽地址交给宿主/日志）
function P.addr(ptr)
	return tonumber(ffi.cast("uint64_t", ptr))
end

------------------------------------------------------------------
-- 共享槽
------------------------------------------------------------------
-- 附着到宿主分配的槽（addr 为整数地址）
function P.slot_attach(addr)
	return ffi.cast("daemon_slot_t *", ffi.cast("uint64_t", addr))
end

-- 本库自行分配一个槽（仅供 Lua 侧测试/纯 Lua 场景；生产中由宿主 malloc 并注入地址）
function P.slot_new()
	local slot = ffi.new("daemon_slot_t[1]")
	slot[0].count = 0
	slot[0].nvals = 0
	slot[0].stop = 0
	P.mutex_init(slot)
	return slot
end

-- 取槽内 mutex 指针（slot 可能是数组[1]或指针，[0] 对两者都成立）
local function slot_mutex(slot)
	return ffi.cast("cj_pthread_mutex_t *", slot[0].mutex)
end

function P.mutex_init(slot)
	ffi.C.pthread_mutex_init(slot_mutex(slot), nil)
end

-- 守护侧：发布一组采样值（mutex 保护，count 自增）
function P.publish(slot, vals)
	local m = slot_mutex(slot)
	ffi.C.pthread_mutex_lock(m)
	local n = #vals
	if n > P.SLOT_MAXVALS then n = P.SLOT_MAXVALS end
	slot[0].nvals = n
	for i = 1, n do
		slot[0].values[i - 1] = vals[i]
	end
	slot[0].count = slot[0].count + 1
	ffi.C.pthread_mutex_unlock(m)
end

-- 主线程侧：读取最新一组值，返回 values 表（1 基）与 count
function P.latest(slot)
	local m = slot_mutex(slot)
	ffi.C.pthread_mutex_lock(m)
	local n = slot[0].nvals
	local t = {}
	for i = 0, n - 1 do
		t[i + 1] = slot[0].values[i]
	end
	local c = slot[0].count
	ffi.C.pthread_mutex_unlock(m)
	return t, c
end

-- 停止标志（守护循环轮询；宿主置位）
function P.should_stop(slot)
	return slot[0].stop ~= 0
end
function P.signal_stop(slot)
	slot[0].stop = 1
end

------------------------------------------------------------------
-- 跨语言槽布局核对（Low：槽布局人工维护）
-- 宿主侧 r.cj/Daemon.cj 手写 SLOT_*_OFF 与 @C struct DaemonSlot 一致；
-- 本库用 ffi 的运行时 struct 布局（编译器填充后的事实）逐项断言，
-- 把「仓颉定义 vs Lua cdef」的漂移在每次守护启动时自动暴露。
-- 仓颉侧基准（Daemon.cj:23-28，SLOT_SIZE=208）：
--   offset  0  char     mutex[64]     (pthread_mutex_t, 预留 64)
--   offset 64  int32    count
--   offset 68  int32    nvals
--   offset 72  double   values[16]
--   offset 200 uint8    stop
-- ffi.offsetof/sizeof 并非所有 arch 都可用——不可用时跳过断言（回退旧行为）。
-- ffi.offsetof/sizeof 并非所有 platform 都可用——不可用时跳过断言（回退旧行为）。
-- ffi 命名空间内未声明原生 pthread_mutex_t（HAL 只声明大小截断的别名），
-- 故真实 mutex 尺寸无法直接 sizeof；仅在可解析时做 ≤64B 预留校验。
------------------------------------------------------------------
local function layout_ok()
	if not ffi.offsetof then return true end
	local ok, to = pcall(function()
		-- P2: pthread_mutex_t 真实尺寸须 ≤ 预留 64B（aarch64=48、x86_64=40）。
		--     类型未声明则 pcall 捕获并跳过——无法断言比已声明的更多。
		local mutex_type_ok, msz = pcall(function() return ffi.sizeof("pthread_mutex_t") end)
		if mutex_type_ok and msz > 64 then
			error("pthread_mutex_t size " .. tostring(msz) .. " > 64B slot mutex reserve")
		end
		local checks = {
			{ "count",   64,  ffi.offsetof("daemon_slot_t", "count") },
			{ "nvals",   68,  ffi.offsetof("daemon_slot_t", "nvals") },
			{ "values",  72,  ffi.offsetof("daemon_slot_t", "values") },
			{ "stop",   200,  ffi.offsetof("daemon_slot_t", "stop") },
			{ "size",   208,  ffi.sizeof("daemon_slot_t") },
		}
		for _, c in ipairs(checks) do
			if c[3] ~= c[2] then
				error("slot layout mismatch: " .. c[1] .. "=" .. tostring(c[3]) .. " (expect " .. c[2] .. ")")
			end
		end
	end)
	if not ok then
		print("BOT4CJ slot layout check failed: " .. tostring(to))
		return false
	end
	return true
end
P.assert_layout = layout_ok

return P
