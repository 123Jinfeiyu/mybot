-- daemon_probe.lua - 模拟传感器守护 chunk（D 方案：setup + 全局 daemon_tick 单次采样）
-- daemon_body 先 pcall 本 chunk 一次完成 setup，随后逐拍调用 daemon_tick，拍间 Cangjie sleep 让出。
if jit then jit.off() end

local dir = os.getenv("BOT4CJ_MODULE_PATH") or "./modules"
local P = dofile(dir .. "/lib/pthread.lua")
local slot = P.slot_attach(DAEMON_SLOT_ADDR)

local t = 0
-- 逐拍发布 {1.5, 2.5, 3.5, sin}；v0=1.5 作跨语言布局校验锚点
function daemon_tick()
	t = t + 1
	P.publish(slot, { 1.5, 2.5, 3.5, math.sin(t * 0.1) * 100 })
end
