-- daemon_probe.lua - 守护线程地基探针 chunk
-- 在【独立 lua_State】上由 r.cj 的 daemon_probe @C 入口 loadfilex + pcall 一次；
-- while 循环写在此处，pcall 阻塞至 stop 标志置位（主线程 free 时置位）。
--
-- 目的：验证 Cangjie @C 入口在 foreign pthread 上创建并驱动 lua_State 的运行时安全性，
-- 以及独立状态机与主线程状态机并发跑 Lua（含 GC）不崩溃。

if jit then jit.off() end		-- 关 JIT：独立状态机跨线程安全的前提

local ffi = require("ffi")
local stop    = ffi.cast("uint8_t *", ffi.cast("uint64_t", DAEMON_STOP_ADDR))
local counter = ffi.cast("int64_t *", ffi.cast("uint64_t", DAEMON_COUNT_ADDR))

local n = 0
while stop[0] == 0 do
	n = n + 1
	counter[0] = n				-- 跨线程写入，主线程 play() 读取
	-- GC 压力：建表 + 字符串分配，最大化暴露并发竞争
	local t = {}
	for i = 1, 50 do
		t[i] = ('x'):rep(i % 8) .. tostring(i)
	end
end
