-- test_multipath_layout.lua - 非缺省多路径布局冒烟（fix-plan 3-3）
-- 背景：HAL.lua / virtbus.lua / drv_common.lua 定位 lib/bus_contract.lua、
-- lib/virtbus.lua 原先只取 BOT4CJ_MODULE_PATH 首段——多路径布局下首段没有
-- 对应文件时，HAL 引导直接失败、virt_prepare 误报 cannot load。现逐段探测。
-- 本用例以「首段不存在、次段为仓库 modules」的布局验证三处收敛：
--   1) virtbus.lua 可加载且带 bus_contract；
--   2) drv_common.virt_prepare 不再误报 cannot load（旧实现在此布局必失败，
--      是本次修复的判别性用例）；
--   3) 全部路径段都不存在时仍按原契约报 cannot load（fail 可诊断）。
-- 依赖 ffi 设环境变量（os.setenv 不存在）→ 仅 LuaJIT 可跑，run_all.sh 默认即 luajit。
-- 用法（项目根目录）: luajit test/test_multipath_layout.lua（run_all.sh 自动纳入）
local tester = dofile("test/lib/tester.lua")
local ffi = require("ffi")
ffi.cdef[[
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
]]

local function with_env(value, fn)
	local old = os.getenv("BOT4CJ_MODULE_PATH")
	ffi.C.setenv("BOT4CJ_MODULE_PATH", value, 1)
	local ok, err = pcall(fn)
	if old == nil then ffi.C.unsetenv("BOT4CJ_MODULE_PATH")
	else ffi.C.setenv("BOT4CJ_MODULE_PATH", old, 1) end
	if not ok then error(err, 0) end
end

-- 1) 首段不存在：virtbus 仍可加载，且拿到 bus_contract 单点
with_env("test/mpath_p2:./modules", function()
	local vbus = dofile("modules/lib/virtbus.lua")
	tester.check("多路径布局 virtbus 可加载", type(vbus) == "table")
	tester.check("virtbus 携带 bus_contract", type(vbus.bus_contract) == "table")
	tester.check("bus_contract 提供 byte_of", type(vbus.bus_contract.byte_of) == "function")
end)

-- 2) 判别性用例：旧实现在此布局 virt_prepare 必报 cannot load，现应成功
with_env("test/mpath_p2:./modules", function()
	dofile("modules/lib/drv_common.lua")
	-- 无 address/fd 的最小数据行：不走挂载，只验证 virtbus 定位与 inject 通路
	local why = virt_prepare({ proto = "virt", name = "smoke" })
	tester.check("virt_prepare 多路径布局成功（返回 nil）", why == nil)
	-- 还原全局 i2c_*/spi_*/uart_*，不给后续用例留下注入态
	if __BOT4CJ_VIRTBUS then __BOT4CJ_VIRTBUS.reset() end
end)

-- 3) 全部段都不存在：保持 cannot load 报错契约（可诊断、不静默）
with_env("test/mpath_p1:test/mpath_p2", function()
	dofile("modules/lib/drv_common.lua")
	local why = virt_prepare({ proto = "virt", name = "smoke" })
	tester.check("全段无 virtbus 报 cannot load", why == "cannot load lib/virtbus.lua")
end)

tester.summary("test_multipath_layout")
return 0
