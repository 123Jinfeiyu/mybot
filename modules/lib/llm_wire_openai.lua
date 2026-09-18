-- llm_wire_openai.lua —— OpenAI 兼容报文的编解码（LlmOpenAI / LlmDeepSeek 共用）
--
-- 输入输出都是 **agent 的规范化协议**（见 docs/agent.md），provider 侧的字段差异
-- 全部收敛在这里与各驱动里：
--   规范化请求：{ model, messages[], tools[], max_tokens, temperature, top_p, ... }
--   规范化响应：{ result="OK", content, tool_calls[], extras{} } / { result="ERR", message }
--
-- messages 的规范化形态（role 决定字段）：
--   system/user : { role, content }
--   assistant   : { role, content?, tool_calls[]?, extras? }
--   tool        : { role, tool_call_id, content }
--
-- 重复加载保护（与 HAL.lua 同一做法）
if _G.__BOT4CJ_LLM_WIRE_OPENAI ~= nil then return _G.__BOT4CJ_LLM_WIRE_OPENAI end

local mpath = dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua")
local json = dofile(mpath.resolve("lib/json.lua"))
local http = dofile(mpath.resolve("lib/llm_http.lua"))

local M = {}

-- 本协议的缺省接入点（「当前协议」：moma 网关 + zhipu/glm）。宿主可用
-- --llm-endpoint/--llm-model 覆盖；放在库里是为了让协议整形可离线单测。
M.DEFAULTS = {
	endpoint = "https://moma.cmecloud.cn/v1/chat/completions",
	model = "zhipu/glm-5.3-flash",
}

local function snippet(s, n)
	s = s or ""
	if #s > n then return s:sub(1, n) .. "...(截断)" end
	return s
end

-- 规范化消息 → provider 消息
local function to_wire_message(msg, inject_reasoning)
	local role = msg.role
	if role == "tool" then
		return { role = "tool", tool_call_id = msg.tool_call_id, content = msg.content or "" }
	end
	if role == "assistant" then
		local out = { role = "assistant" }
		-- 带 tool_calls 且无过渡语时 content 必须是 null（空串会被部分实现当成
		-- 「有文本」而拒收 tool_calls 组合）
		if msg.content ~= nil and msg.content ~= "" then
			out.content = msg.content
		else
			out.content = json.null
		end
		local tcs = msg.tool_calls
		if tcs ~= nil and #tcs > 0 then
			local wire_tcs = {}
			for i, tc in ipairs(tcs) do
				wire_tcs[i] = {
					id = tc.id,
					type = "function",
					["function"] = { name = tc.name, arguments = tc.arguments },
				}
			end
			out.tool_calls = wire_tcs
		end
		-- DeepSeek 思考模式：带 tools 的请求必须回传历史 reasoning_content，
		-- 否则 API 返回 400。它随 extras 由宿主逐轮原样带回。
		if inject_reasoning and msg.extras ~= nil and msg.extras.reasoning_content ~= nil then
			out.reasoning_content = msg.extras.reasoning_content
		end
		return out
	end
	return { role = role, content = msg.content or "" }
end

-- opts:
--   inject_reasoning_content : 把 extras.reasoning_content 回填进 assistant 消息
--   omit_temperature / omit_top_p : 该 provider 不支持该采样参数时不发（而非发一个被忽略的值）
--   extra(req, data)         : provider 特有的顶层字段
function M.build_request(data, model, opts)
	opts = opts or {}
	local msgs = {}
	for i, m in ipairs(data.messages or {}) do
		msgs[i] = to_wire_message(m, opts.inject_reasoning_content)
	end

	local req = { model = model, messages = msgs, stream = false }
	if data.max_tokens ~= nil then req.max_tokens = data.max_tokens end
	if data.temperature ~= nil and not opts.omit_temperature then req.temperature = data.temperature end
	if data.top_p ~= nil and not opts.omit_top_p then req.top_p = data.top_p end

	local tools = data.tools
	if tools ~= nil and #tools > 0 then
		local wire_tools = {}
		for i, t in ipairs(tools) do
			wire_tools[i] = {
				type = "function",
				["function"] = { name = t.name, description = t.description, parameters = t.parameters },
			}
		end
		req.tools = wire_tools
		req.tool_choice = "auto"
	end

	if opts.extra ~= nil then opts.extra(req, data) end
	return req
end

-- provider 响应 → 规范化响应（只取本项目用得上的字段）
function M.parse_response(decoded)
	local choice = decoded.choices and decoded.choices[1]
	local msg = choice and choice.message
	if msg == nil then return nil, "响应缺少 choices[1].message" end

	local out = { result = "OK", content = msg.content or "" }

	local tcs = msg.tool_calls
	if tcs ~= nil then
		local calls = {}
		for i, tc in ipairs(tcs) do
			local fn = tc["function"] or {}
			calls[i] = {
				id = tc.id or ("call_" .. i),
				name = fn.name or "",
				arguments = fn.arguments or "{}",
			}
		end
		-- 无条件表编码为对象，故只在非空时挂上；空数组等价于「没有工具调用」
		if #calls > 0 then out.tool_calls = calls end
	end

	-- extras：provider 特有的附加字段，宿主原样逐轮带回，驱动决定怎么用
	local extras = {}
	if msg.reasoning_content ~= nil and msg.reasoning_content ~= "" then
		extras.reasoning_content = msg.reasoning_content
	end
	if next(extras) ~= nil then out.extras = extras end

	return out
end

-- API key：优先宿主传入（--llm-key），否则按环境变量链取
function M.api_key(data)
	if data.api_key ~= nil and data.api_key ~= "" then return data.api_key end
	for _, name in ipairs({ "BOT4CJ_LLM_KEY", "ROBOT_AGENT_API_KEY", "DEEPSEEK_API_KEY" }) do
		local v = os.getenv(name)
		if v ~= nil and v ~= "" then return v end
	end
	return nil
end

-- 从规范化请求组装出「接入点 + provider 报文」。
-- 驱动只调这一个入口；DeepSeek 库覆写 plan 以换缺省值与整形规则。
function M.request(data, plan)
	local D = M.DEFAULTS
	local endpoint = data.endpoint
	if endpoint == nil or endpoint == "" then endpoint = D.endpoint end
	local model = data.model
	if model == nil or model == "" then model = D.model end
	return { endpoint = endpoint, req = M.build_request(data, model, plan) }
end

-- 编码 → 发出 → 状态检查 → 解码 → 规范化。任何一步失败都返回 ERR 契约表
-- （由宿主折成可读报错回传给 agent 循环，不打断会话）。
function M.perform(endpoint, data, req)
	local ok, body = pcall(json.encode, req)
	if not ok then
		return { result = "ERR", message = "请求编码失败: " .. tostring(body) }
	end

	local headers = { "Content-Type: application/json" }
	local key = M.api_key(data)
	if key ~= nil then headers[#headers + 1] = "Authorization: Bearer " .. key end

	local res = http.post({
		url = endpoint,
		headers = headers,
		body = body,
		timeout = tonumber(data.timeout) or 180,
	})
	if not res.ok then
		return { result = "ERR", message = res.err }
	end
	if res.status ~= 200 then
		return { result = "ERR", message = "HTTP " .. tostring(res.status) .. ": " .. snippet(res.body, 500) }
	end

	local decoded_ok, decoded = pcall(json.decode, res.body)
	if not decoded_ok then
		return { result = "ERR", message = "响应不是合法 JSON: " .. tostring(decoded)
			.. " | " .. snippet(res.body, 300) }
	end
	if type(decoded) ~= "table" then
		return { result = "ERR", message = "响应不是 JSON 对象 | " .. snippet(res.body, 300) }
	end
	if decoded.error ~= nil then
		local e = decoded.error
		local em
		if type(e) == "table" then
			em = e.message or json.encode(e)
		else
			em = tostring(e)
		end
		return { result = "ERR", message = "API 错误: " .. em }
	end

	local canonical, perr = M.parse_response(decoded)
	if canonical == nil then
		return { result = "ERR", message = perr .. " | " .. snippet(res.body, 300) }
	end
	return canonical
end

_G.__BOT4CJ_LLM_WIRE_OPENAI = M
return M
