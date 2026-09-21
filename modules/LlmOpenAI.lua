-- LlmOpenAI.lua —— OpenAI 兼容协议的 LLM 驱动（「当前协议」：moma 网关 + zhipu/glm）
--
-- 表名即驱动名：结构脚本里 `CREATE TABLE LlmOpenAI (name String)` 即挂载本文件。
-- agent 模式未显式声明该变量时，宿主会按 --llm-driver 自动注入同名表。
--
-- 规范化协议见 modules/lib/llm_wire_openai.lua 与 docs/agent.md：
-- 宿主把 {model, messages, tools, max_tokens, temperature, top_p, timeout,
-- endpoint, api_key} 作为参数传进 call()，本驱动返回
-- {result="OK", content, tool_calls?, extras?} 或 {result="ERR", message}。
--
-- API key 由本驱动自己从环境变量读取（宿主不把密钥写进参数表——否则
-- BOT4CJ_DEBUG 的参数转储会把密钥打进日志）。
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local http = dofile(mpath.resolve("lib/llm_http.lua"))
local wire = dofile(mpath.resolve("lib/llm_wire_openai.lua"))
-- 入口契约（2026-09-20 全量 arg_table 收敛）：非表入参返回 ERR 契约表而不是抛
-- Lua error——跨语言边界上「抛」与「返 ERR」对宿主的处理路径完全不同。
-- 协议驱动同样纳入（此前只有器件驱动走了这条统一入口）。
dofile(mpath.resolve("lib/drv_common.lua"))

-- 构建期：把「运行环境不具备」变成上电前的响亮失败，而不是第一次调模型才炸
function build(data)
	local ok, e = arg_table(data)
	if not ok then return e end
	local loaded, loadErr = http.load()
	if not loaded then
		return err(loadErr)
	end
	return { result = "OK" }
end

function destroy(data)
	local ok, e = arg_table(data)
	if not ok then return e end
	return { result = "OK" }
end

function call(data)
	local ok, e = arg_table(data)
	if not ok then return e end
	-- 组包阶段的失败也要走 ERR 契约（2026-09-21 审计 AG-15）：wire.request 会为
	-- 缺失/类型不符的必填字段抛错（如 tool 消息缺 tool_call_id），而本驱动的契约
	-- 是「返 ERR 表，不抛 Lua error」——跨语言边界上「抛」会被宿主折成一条带 Lua
	-- 栈帧的 ERR，文案可读性远不如这里点名到字段与消息的那句。perform 阶段自己
	-- 已经返回 ERR 表，故只需包住 request。
	local rok, r = pcall(wire.request, data)
	if not rok then
		return err("请求组包失败: " .. tostring(r))
	end
	return wire.perform(r.endpoint, data, r.req)
end
