-- test_driver_contract.lua — 驱动入口契约 sweep（2026-09-20 重构阶段 4.5）
--
-- 背景：驱动入口的约定此前只有一条 **grep lint**（run_all.sh 的 lint 段断言
-- build/call 里不出现内联 `type(data) ~= "table"`）。grep 只能抓一种写法，
-- 抓不到「漏写 destroy」「build 恒返回非表」「非表入参不返 ERR 契约」等形态；
-- 而 59 个模块是全仓最大的一个「靠约定维系」的面。
--
-- 本 sweep 把约定变成可执行断言：**逐个真的加载**每个 modules/*.lua，然后
--   ① 可加载（顺带守住模块路径引导 —— 阶段 2 的 __mpath 迁移若写错，
--      这一步就会红，无需等到 E2E）；
--   ② 驱动入口三件套 build/call/destroy 均为函数（纯 Lua 库除外，见豁免表）；
--   ③ 非表入参必须返回 {result="ERR", message=...} 的 ERR 契约，而不是抛 Lua
--      error、也不是静默成功（这是 arg_table 的语义出口，跨语言边界上
--      「抛」与「返 ERR」对宿主的处理路径完全不同）。
--
-- 豁免（都在文件头注声明过理由，此处集中列举，避免 grep 式的隐式豁免）：
--   bitcompat  纯 Lua 库：只 return 一个表，无驱动入口
--   Camera     全仓唯一不挂 arg_table 样板的驱动（可选外设，build 不碰设备，
--              入参走内联类型守卫；见 modules/Camera.lua 头注与 run_all.sh lint 段）
--
-- 用法（项目根目录）：luajit test/test_driver_contract.lua（run_all.sh 自动纳入）

local tester = dofile("test/lib/tester.lua")
local check = tester.check

local LIB_ONLY = { bitcompat = true }
local NO_ARG_TABLE = { Camera = true }

-- 枚举模块（测试进程不在驱动沙箱内，io.popen 可用；run_all.sh 亦依赖 coreutils）
local function listModules()
	local out = {}
	local p = io.popen("ls modules/*.lua 2>/dev/null")
	if not p then return out end
	for line in p:lines() do out[#out + 1] = line end
	p:close()
	return out
end

local mods = listModules()
check("sweep 枚举到全部 modules/*.lua", #mods >= 59, "枚举到 " .. tostring(#mods))

for _, path in ipairs(mods) do
	local name = path:match("([^/]+)%.lua$") or path
	-- 驱动以全局 build/call/destroy 暴露入口——同一进程里逐个加载时会互相
	-- 继承上一份定义，故每次加载前先清空，断言才真的对应本模块（否则
	-- 「漏写 build」会被上一个驱动掩盖成通过）。
	build, call, destroy = nil, nil, nil
	local ok, err = pcall(dofile, path)
	check(name .. " 可加载", ok, tostring(err))
	if ok then
		if LIB_ONLY[name] then
			check(name .. " 为纯库（不定义驱动入口）", type(build) ~= "function")
		else
			check(name .. " 定义 build", type(build) == "function")
			check(name .. " 定义 call", type(call) == "function")
			check(name .. " 定义 destroy", type(destroy) == "function")
			if not NO_ARG_TABLE[name] then
				-- 非表入参：必须是 ERR 契约表（不是抛错、不是 nil）。
				-- 用 pcall 包住：一旦某驱动抛 Lua error，sweep 必须把它记成
				-- 失败并继续扫下一个模块，而不是整个用例中途夭折
				--（首版就踩过：LlmDeepSeek.call(nil) 抛错，后面的模块全没扫到）。
				local pok, rb = pcall(build, nil)
				check(name .. ".build 非表入参返 ERR 契约（不抛错）",
					pok and type(rb) == "table" and rb.result == "ERR",
					pok and (type(rb) == "table" and tostring(rb.result) or type(rb))
						or ("raised: " .. tostring(rb)))
				local cok, rc = pcall(call, nil)
				check(name .. ".call 非表入参返 ERR 契约（不抛错）",
					cok and type(rc) == "table" and rc.result == "ERR",
					cok and (type(rc) == "table" and tostring(rc.result) or type(rc))
						or ("raised: " .. tostring(rc)))
			end
		end
	end
end

tester.summary("test_driver_contract")
return 0
