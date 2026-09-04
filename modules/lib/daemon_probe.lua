-- daemon_probe.lua - 守护采样 chunk（模拟三轴传感器，槽版 IPC）
-- 在【独立 lua_State】上由 r.cj 的 daemon_body（spawn 线程）loadfilex + pcall 一次；
-- while 循环写在此处，pcall 阻塞至停止标志置位（主线程 free 时置位）。
--
-- 经 pthread.lua 附着宿主注入的共享槽（DAEMON_SLOT_ADDR），每次采样 publish 一组值；
-- 主线程 play() 加锁读取。v0 固定 1.5 用于校验 Cangjie↔Lua 的 double 偏移一致，
-- 第 4 个值随时间变化用于验证活性。

if jit then jit.off() end		-- 关 JIT：独立状态机跨线程安全的前提

local dir = os.getenv("BOT4CJ_MODULE_PATH") or "./modules"
local P = dofile(dir .. "/lib/pthread.lua")	-- 共享槽 IPC 原语
dofile(dir .. "/lib/HAL.lua")			-- milli_sleep（节拍）

local slot = P.slot_attach(DAEMON_SLOT_ADDR)

local t = 0
while not P.should_stop(slot) do
	t = t + 1
	-- 模拟三轴 + 时变第4值：v0 恒为 1.5（跨语言布局校验锚点）
	P.publish(slot, { 1.5, 2.5, 3.5, math.sin(t * 0.1) * 100 })
	milli_sleep(2)				-- ~500Hz
end
