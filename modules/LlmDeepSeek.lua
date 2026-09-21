-- LlmDeepSeek.lua —— DeepSeek 协议的 LLM 驱动
--
-- 表名即驱动名：结构脚本里 `CREATE TABLE LlmDeepSeek (name String)` 即挂载本文件；
-- 也可用 `--llm-driver LlmDeepSeek` 让宿主自动注入。
--
-- 协议差异（思考模式开关、reasoning_content 回传、采样参数限制）全部在
-- modules/lib/llm_wire_deepseek.lua 里，本文件只负责接线与构建期自检。
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local http = dofile(mpath.resolve("lib/llm_http.lua"))
local wire = dofile(mpath.resolve("lib/llm_wire_deepseek.lua"))
-- 入口契约同 LlmOpenAI：非表入参返 ERR 契约表而非抛 Lua error
dofile(mpath.resolve("lib/drv_common.lua"))

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
