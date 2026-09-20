-- llm_wire_deepseek.lua —— DeepSeek 协议的整形规则（在 OpenAI 兼容骨架上覆写）
--
-- 三处相对 OpenAI 兼容协议的**实质差异**（据 DeepSeek 官方《思考模式》文档）：
--   1. 思考模式开关：请求体 {"thinking":{"type":"enabled"}}，强度用 reasoning_effort；
--   2. reasoning_content：与 content 同级返回。**请求带 tools 时，历史轮次的
--      reasoning_content 必须完整回传，否则 API 直接 400** —— 有状态要求；
--      宿主把该字段放进 assistant 消息的 extras 逐轮原样带回，本库负责回填；
--   3. 采样参数：思考模式不支持 temperature（传了也被静默忽略）、top_p 生效但
--      下限被抬到 0.95；非思考模式 top_p 恒为 1.0。
--
-- 拆成独立库（而不是塞在驱动里）是为了让这些规则能离线单测：
-- test/test_llm_drivers.lua 直接断言 build_request 的产物，不碰网络。
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local openai = dofile(mpath.resolve("lib/llm_wire_openai.lua"))

local M = {}

M.DEFAULTS = {
	endpoint = "https://api.deepseek.com/chat/completions",
	model = "deepseek-chat",
}

-- 思考模式缺省开启（DeepSeek 默认即开启）；data.thinking 传 disabled/off/false 关闭
function M.thinking_enabled(data)
	return not (data.thinking == "disabled" or data.thinking == "off" or data.thinking == "false")
end

-- 该协议特有的整形开关（供 openai.build_request 使用）
function M.plan(data)
	local thinking = M.thinking_enabled(data)
	local effort = data.reasoning_effort
	if effort == nil or effort == "" then effort = os.getenv("BOT4CJ_LLM_REASONING_EFFORT") end
	return {
		-- 思考模式下必须回传历史 reasoning_content（见文件头第 2 条）
		inject_reasoning_content = thinking,
		-- 思考模式不支持 temperature；非思考模式 top_p 被强制为 1.0
		omit_temperature = thinking,
		omit_top_p = not thinking,
		extra = function(req)
			if thinking then
				req.thinking = { type = "enabled" }
				if effort ~= nil and effort ~= "" then req.reasoning_effort = effort end
			else
				req.thinking = { type = "disabled" }
			end
		end,
	}
end

function M.build_request(data, model)
	local plan = M.plan(data)
	local req = openai.build_request(data, model, plan)
	-- 思考模式 top_p 下限 0.95：低于该值不会生效，按 0.95 发以免误导调用方
	if plan.inject_reasoning_content and req.top_p ~= nil then
		local tp = tonumber(req.top_p)
		if tp ~= nil and tp < 0.95 then req.top_p = 0.95 end
	end
	return req
end

function M.request(data, plan)
	local D = M.DEFAULTS
	local endpoint = data.endpoint
	if endpoint == nil or endpoint == "" then endpoint = D.endpoint end
	local model = data.model
	if model == nil or model == "" then model = D.model end
	return { endpoint = endpoint, req = M.build_request(data, model, plan) }
end

M.parse_response = openai.parse_response
M.perform = openai.perform
M.api_key = openai.api_key

return M
