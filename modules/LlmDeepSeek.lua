-- LlmDeepSeek.lua —— DeepSeek 协议的 LLM 驱动
--
-- 表名即驱动名：结构脚本里 `CREATE TABLE LlmDeepSeek (name String)` 即挂载本文件；
-- 也可用 `--llm-driver LlmDeepSeek` 让宿主自动注入。
--
-- 协议差异（思考模式开关、reasoning_content 回传、采样参数限制）全部在
-- modules/lib/llm_wire_deepseek.lua 里，本文件只负责接线与构建期自检。
local mpath = dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua")
local http = dofile(mpath.resolve("lib/llm_http.lua"))
local wire = dofile(mpath.resolve("lib/llm_wire_deepseek.lua"))

function build(data)
	local ok, err = http.load()
	if not ok then
		return { result = "ERR", message = err }
	end
	return { result = "OK" }
end

function destroy(data)
	return { result = "OK" }
end

function call(data)
	local r = wire.request(data)
	return wire.perform(r.endpoint, data, r.req)
end
