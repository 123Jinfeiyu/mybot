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

return P
