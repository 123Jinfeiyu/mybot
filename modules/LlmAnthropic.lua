-- LlmAnthropic.lua —— Anthropic Messages 协议的 LLM 驱动（第三个协议实例）
--
-- 表名即驱动名：结构脚本里 `CREATE TABLE LlmAnthropic (name String)` 即挂载本文件，
-- 或由 --llm-driver LlmAnthropic 自动注入。
--
-- 本驱动是「换模型 = 换 lua 文件」这条承诺的**验证实例**：Anthropic 的报文形状与
-- OpenAI 系差别最大（system 是顶层字段而非消息、tool_calls 表达为 content 块、
-- tool 结果必须并进一条 user 消息、鉴权头不是 Bearer）——协议整形全部落在
-- 本文件与 lib/llm_wire_anthropic.lua 里，**宿主一行未改**（验收线即
-- `git diff --stat src/` 为空）。
--
-- 规范化协议见 docs/agent.md 与 lib/llm_wire_anthropic.lua 顶部注释；
-- 入口契约与其它驱动一致：非表入参返回 ERR 契约表（不抛 Lua error），
-- call() 返回 {result="OK", content, tool_calls?, extras?, usage?} 或
-- {result="ERR", message, ...}。
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local http = dofile(mpath.resolve("lib/llm_http.lua"))
local wire = dofile(mpath.resolve("lib/llm_wire_anthropic.lua"))
dofile(mpath.resolve("lib/drv_common.lua"))

-- 构建期：把「运行环境不具备」变成上电前的响亮失败（与其余驱动同一口径）
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
	-- 释放池化 HTTP 会话（与 LlmOpenAI 同款，AG-20）
	http.close_pooled()
	return { result = "OK" }
end

function call(data)
	local ok, e = arg_table(data)
	if not ok then return e end
	-- 组包阶段的失败折成 ERR 契约（AG-15 同口径）：跨语言边界上「抛」会被宿主
	-- 折成一条带 Lua 栈帧的 ERR，可读性远不如这里点名到字段的那句。
	local rok, r = pcall(wire.request, data)
	if not rok then
		return err("请求组包失败: " .. tostring(r))
	end
	return wire.perform(r.endpoint, data, r.req)
end
