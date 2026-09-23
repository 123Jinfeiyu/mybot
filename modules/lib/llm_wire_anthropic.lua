-- llm_wire_anthropic.lua —— Anthropic Messages 报文的编解码（LlmAnthropic 专用）
--
-- 输入输出都是 **agent 的规范化协议**（见 docs/agent.md），Anthropic 侧的字段
-- 差异全部收敛在这里：
--   规范化请求：{ model, messages[], tools[], max_tokens, temperature, top_p, ... }
--   规范化响应：{ result="OK", content, tool_calls[], extras{}, usage{} } / { result="ERR", message }
--
-- messages 的规范化形态（role 决定字段）：
--   system/user : { role, content }            -- content 可以是字符串或部件数组
--   assistant   : { role, content?, tool_calls[]?, extras? }
--   tool        : { role, tool_call_id, content }
--
-- 与 OpenAI 系的**四处形状差异**（都是本层要抹平的）：
--   ① system 提示词是顶层 `system` 字段，不进 messages；且 Anthropic 要求
--      messages 严格 user/assistant 交替——规范化历史里的 system 必须抽走；
--   ② 工具调用是 assistant 的 content 块 {type="tool_use", id, name, input}，
--      input 是**对象**（规范化侧给的是 arguments JSON 字符串，要解码）；
--   ③ 工具结果是**一条 user 消息里的 tool_result 块**，且连续的多个 tool 消息
--      必须并进同一条 user 消息（否则违反交替约束，直接 400）；
--   ④ 鉴权头是 `x-api-key`（不是 Authorization: Bearer），另需
--      `anthropic-version`。
-- 图片同样是 content 块，但形态不同：{type="image", source={type="base64",
-- media_type, data}}（data URL 要拆成 media_type + base64 数据）。
--
-- 重复加载保护（与 HAL.lua 同一做法）
if _G.__BOT4CJ_LLM_WIRE_ANTHROPIC ~= nil then return _G.__BOT4CJ_LLM_WIRE_ANTHROPIC end

local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local json = dofile(mpath.resolve("lib/json.lua"))
local http = dofile(mpath.resolve("lib/llm_http.lua"))

local M = {}

-- 缺省模型（可用 --llm-model 覆盖）。与 LlmOpenAI 同款做法：放在库里便于离线单测。
M.DEFAULTS = {
	model = "claude-sonnet-4-5",
}

-- Anthropic 要求显式带协议版本；写死在库里（换个版本需要客户端改代码，不是配置项）
M.API_VERSION = "2023-06-01"

-- 接入点必填判据（B2 同口径 fail-closed）：本驱动同样不内置缺省接入点——
-- 内置缺省会把含结构脚本、硬件清单、摄像头画面的上下文静默发往某个网关。
function M.require_endpoint(data)
	local e = data.endpoint
	if e ~= nil and e ~= "" then return e end
	error("未配置 endpoint：本驱动不内置缺省接入点（内置缺省会把上下文发往特定"
		.. "第三方网关）。请用 --llm-endpoint、环境变量 BOT4CJ_LLM_ENDPOINT 或"
		.. "配置文件 endpoint 字段显式指定接入点（Anthropic 形如 "
		.. "https://api.anthropic.com/v1/messages）")
end

local function snippet(s, n)
	s = s or ""
	if #s > n then return s:sub(1, n) .. "...(截断)" end
	return s
end

function M.require_wire_string(v, what, where)
	if type(v) ~= "string" or v == "" then
		error(string.format("请求字段 %s 缺失或非字符串（%s）", what, where))
	end
	return v
end

-- data URL → {media_type, data}。Anthropic 不接受 data URL 整串，必须拆开。
-- 形状非法时显式报错（而不是发出一个必然 400 的报文）——与 B3 的图片游标
-- 解析失败同口径：把「我们这边拼错了」与「provider 拒了」分开。
local function split_data_url(url, where)
	if type(url) ~= "string" then
		error("图片 url 不是字符串（" .. where .. "），Anthropic 需要 data URL 形态")
	end
	local media, b64 = url:match("^data:([^;]+);base64,(.*)$")
	if media == nil or b64 == nil then
		error("图片 url 不是 data URL（" .. where .. "）：Anthropic 只接受 base64 内联图片"
			.. "（形如 data:image/png;base64,...）")
	end
	return media, b64
end

-- 图片游标（B3）：宿主在 url 里放 {"__bot_img": N}，在请求组装窗口内经
-- __bot_img 落值一次。语义与 llm_wire_openai.resolve_image_url 逐字对齐。
local function resolve_image_url(url, where)
	if type(url) ~= "table" or url.__bot_img == nil then return url end
	local fetch = rawget(_G, "__bot_img")
	if fetch == nil then
		error("图片游标无法解析（" .. where .. "）：宿主未注册 __bot_img 通道"
			.. "（驱动与宿主版本不匹配，或不在模型请求组包窗口内）")
	end
	local ok, s = pcall(fetch, url.__bot_img)
	if not ok or type(s) ~= "string" then
		error("图片游标解析失败（" .. where .. "，游标 " .. tostring(url.__bot_img)
			.. "）: " .. tostring(s))
	end
	return s
end

-- 规范化 content（字符串 / 部件数组）→ Anthropic content 块数组
local function to_content_blocks(content, where)
	if type(content) == "string" then
		if content == "" then return nil end
		return { { type = "text", text = content } }
	end
	if type(content) ~= "table" then
		error("content 形态非法（" .. where .. "）：期望字符串或部件数组，实际 "
			.. type(content))
	end
	local blocks = {}
	for i, p in ipairs(content) do
		if type(p) == "table" and p.type == "image_url" and p.image_url ~= nil then
			local url = p.image_url
			if type(url) == "table" then url = url.url end
			local resolved = resolve_image_url(url, where .. " 的第 " .. i .. " 个图片部件")
			local media, b64 = split_data_url(resolved, where .. " 的第 " .. i .. " 个图片部件")
			blocks[#blocks + 1] = {
				type = "image",
				source = { type = "base64", media_type = media, data = b64 },
			}
		elseif type(p) == "table" and p.type == "text" and type(p.text) == "string" then
			blocks[#blocks + 1] = { type = "text", text = p.text }
		else
			-- 不认识的部件不静默丢弃：它意味着宿主与驱动对形状的理解不一致，
			-- 发出去只会拿一个含混的 400。与 sanitize 的诚实口径一致。
			error("content 部件形态非法（" .. where .. " 的第 " .. i .. " 个部件）："
				.. "只支持 text / image_url（Anthropic 侧映射为 text / image）")
		end
	end
	if #blocks == 0 then return nil end
	return blocks
end

-- 规范化消息序列 → Anthropic messages[]（+ 顶层 system）。
-- 逐条消费：assistant 带 tool_calls 时转 tool_use 块；紧随其后的连续 tool 消息
-- 并成**一条** user 消息的 tool_result 块（③ 交替约束）。
function M.build_messages(messages)
	local system = nil
	local out = {}
	local i = 1
	while i <= #messages do
		local m = messages[i]
		local role = m.role
		if role == "system" then
			-- ① system 抽到顶层：多条 system 按序拼接（宿主只会给一条，但驱动
			-- 不该假设调用方只有一条）
			local text = type(m.content) == "string" and m.content or ""
			system = (system == nil) and text or (system .. "\n" .. text)
			i = i + 1
		elseif role == "tool" then
			-- ③ 连续 tool 消息并进一条 user 消息
			local blocks = {}
			while i <= #messages and messages[i].role == "tool" do
				local t = messages[i]
				blocks[#blocks + 1] = {
					type = "tool_result",
					tool_use_id = M.require_wire_string(t.tool_call_id, "tool_call_id",
						"role=tool 的消息"),
					content = type(t.content) == "string" and t.content or "",
				}
				i = i + 1
			end
			out[#out + 1] = { role = "user", content = blocks }
		elseif role == "assistant" then
			local blocks = {}
			if type(m.content) == "string" and m.content ~= "" then
				blocks[#blocks + 1] = { type = "text", text = m.content }
			elseif type(m.content) == "table" then
				local b = to_content_blocks(m.content, "assistant 消息")
				if b ~= nil then
					for _, x in ipairs(b) do blocks[#blocks + 1] = x end
				end
			end
			local tcs = m.tool_calls
			if tcs ~= nil and #tcs > 0 then
				for k, tc in ipairs(tcs) do
					local where = "assistant 消息的第 " .. k .. " 个工具调用"
					-- ② arguments 是 JSON 字符串，Anthropic 的 input 要**对象**
					local raw = tc.arguments
					if raw == nil or raw == "" then raw = "{}" end
					local input
					if type(raw) == "table" then
						input = raw
					elseif type(raw) == "string" then
						local ok, decoded = pcall(json.decode, raw)
						if not ok or type(decoded) ~= "table" then
							error("工具调用参数不是合法 JSON 对象（" .. where .. "）："
								.. snippet(tostring(raw), 120)
								.. "；Anthropic 的 input 需要对象形态")
						end
						input = decoded
					else
						error("工具调用参数形态非法（" .. where .. "，实际 "
							.. type(raw) .. "）")
					end
					blocks[#blocks + 1] = {
						type = "tool_use",
						id = M.require_wire_string(tc.id, "tool_calls[].id", where),
						name = M.require_wire_string(tc.name, "tool_calls[].name", where),
						input = input,
					}
				end
			end
			-- 空 assistant（无文本无调用）在 Anthropic 侧是非法的空 content：
			-- 显式拒绝而不是发一个必然 400 的报文
			if #blocks == 0 then
				error("assistant 消息既没有文本也没有工具调用，Anthropic 不接受空 content"
					.. "（第 " .. i .. " 条消息）")
			end
			out[#out + 1] = { role = "assistant", content = blocks }
			i = i + 1
		elseif role == "user" then
			local blocks = to_content_blocks(m.content, "role=user 的消息（第 " .. i .. " 条）")
			if blocks == nil then
				error("user 消息 content 为空，Anthropic 不接受空 content（第 "
					.. i .. " 条消息）")
			end
			-- 相邻 user 消息并块（同样受交替约束；宿主的多模态图片消息就是
			-- 紧跟 tool_result 之后的第二条 user）
			local prev = out[#out]
			if prev ~= nil and prev.role == "user" then
				for _, b in ipairs(blocks) do prev.content[#prev.content + 1] = b end
			else
				out[#out + 1] = { role = "user", content = blocks }
			end
			i = i + 1
		else
			error("未知 role '" .. tostring(role) .. "'（第 " .. i .. " 条消息）")
		end
	end
	return system, out
end

-- 出站形状自检（A1b 同口径）：OpenAI 的 check_tool_pairing 判据在这里不适用
-- （形态不同），Anthropic 侧的等价硬要求是**严格 user/assistant 交替**——
-- 相邻同角色消息会被服务端 400。构建后自查一遍，报错点名第几条，比网关那句有用。
function M.check_alternation(msgs)
	for i = 2, #msgs do
		if msgs[i].role == msgs[i - 1].role then
			return "第 " .. i .. " 条消息与前一条同为 role=" .. tostring(msgs[i].role)
				.. "，Anthropic 要求 user/assistant 严格交替"
		end
	end
	return nil
end

-- JSON Schema 的 required 必须是**数组**（与 llm_wire_openai.normalize_schema
-- 同款、同因）：宿主传来的 canonical 声明里无参工具的 required 是空数组，经
-- Json→Lua 转换后与空对象无法区分，再编码就成了 {}。真网关（以及本仓的严格
-- 夹具）会以 `{} is not of type "array"` 直接 400——这条在批次 3 的 E2E 首跑
-- 就被夹具抓了出来（正是「夹具与真机一样严格」的价值）。缺失同样补成显式空数组。
local function normalize_schema(params)
	params = params or { type = "object" }
	local req = params.required
	if req == nil or (type(req) == "table" and next(req) == nil) then
		params.required = json.empty_array
	end
	return params
end

function M.build_request(data, model)
	local system, msgs = M.build_messages(data.messages or {})
	local altErr = M.check_alternation(msgs)
	if altErr ~= nil then
		error("出站报文自检失败: " .. altErr)
	end

	local req = { model = model, messages = msgs, stream = false }
	-- system 是顶层字段（①）；空则不挂
	if system ~= nil and system ~= "" then req.system = system end
	-- max_tokens 在 Anthropic 是**必填**
	if data.max_tokens == nil then
		error("Anthropic 要求显式 max_tokens（宿主未提供）")
	end
	req.max_tokens = data.max_tokens
	-- 区间差异：Anthropic 只接受 [0, 1]。宿主允许到 2（为 OpenAI 系），
	-- 越界在这里**响亮拒绝**而不是静默钳制——静默改值会让操作者以为
	-- 自己调的参数生效了（与全仓「不谎报状态」同口径）。
	if data.temperature ~= nil then
		local t = tonumber(data.temperature)
		if t == nil or t < 0 or t > 1 then
			error("Anthropic 的 temperature 需要 [0, 1] 区间，当前配置为 "
				.. tostring(data.temperature) .. "（请改配置文件/命令行，"
				.. "本驱动不静默钳制）")
		end
		req.temperature = t
	end
	if data.top_p ~= nil then req.top_p = data.top_p end

	local tools = data.tools
	if tools ~= nil and #tools > 0 then
		local wire_tools = {}
		for i, t in ipairs(tools) do
			wire_tools[i] = {
				name = t.name,
				description = t.description,
				input_schema = normalize_schema(t.parameters),
			}
		end
		req.tools = wire_tools
		-- Anthropic 没有 tool_choice="auto" 这一档：给 tools 即自动择取，
		-- 需要强制时用 {type="tool"}（本驱动不强制）
	end
	return req
end

-- 规范化响应解析：Anthropic 的 content 是块数组，text 段拼接、tool_use 段转
-- 规范化 tool_calls（input 对象 → arguments JSON 字符串）。非文本非工具块
-- （如扩展思考的 thinking 块）**丢弃但计数**并入 extras——与 PL-3 的诚实口径一致：
-- 「模型给了别的内容而我没看见」必须在审计里可见。
function M.parse_response(decoded)
	if type(decoded.content) ~= "table" then
		return nil, "响应字段 content 形态非法（期望数组，实际 "
			.. type(decoded.content) .. "）"
	end
	local texts = {}
	local calls = {}
	local dropped = 0
	for i, blk in ipairs(decoded.content) do
		if type(blk) ~= "table" then
			dropped = dropped + 1
		elseif blk.type == "text" and type(blk.text) == "string" then
			texts[#texts + 1] = blk.text
		elseif blk.type == "tool_use" then
			calls[#calls + 1] = {
				id = blk.id,
				name = blk.name,
				-- arguments 在规范化侧是**字符串**（宿主按字符串解析；对象会被
				-- toString 兜住，但字符串更贴近 provider 原始表达）
				arguments = json.encode(blk.input or {}),
			}
		else
			dropped = dropped + 1
		end
	end

	local out = { result = "OK", content = table.concat(texts, ""), tool_calls = calls }
	local extras = {}
	if dropped > 0 then extras.dropped_content_blocks = dropped end
	-- stop_reason 进 extras：宿主不理解、只回传，但排障时「是不是被
	-- max_tokens 截断了」全靠它（Anthropic 用 stop_reason="max_tokens" 表示）
	if decoded.stop_reason ~= nil then extras.stop_reason = decoded.stop_reason end
	if next(extras) ~= nil then out.extras = extras end
	-- usage：Anthropic 用 input_tokens/output_tokens，规范化的审计只认
	-- prompt_tokens/completion_tokens——在这里映射，宿主零改。
	if type(decoded.usage) == "table" then
		out.usage = {
			prompt_tokens = decoded.usage.input_tokens,
			completion_tokens = decoded.usage.output_tokens,
		}
	end
	return out
end

-- API key 读取链（与 OpenAI 系同一顺序，便于同一份环境变量跨驱动复用）
function M.api_key(data)
	if data.api_key ~= nil and data.api_key ~= "" then return data.api_key end
	for _, name in ipairs({ "BOT4CJ_LLM_KEY", "ROBOT_AGENT_API_KEY", "ANTHROPIC_API_KEY" }) do
		local v = os.getenv(name)
		if v ~= nil and v ~= "" then return v end
	end
	return nil
end

function M.api_key_has_control_chars(key)
	return type(key) == "string" and key:find("[%z\1-\31\127]") ~= nil
end

function M.request(data)
	local endpoint = M.require_endpoint(data)
	local model = data.model
	if model == nil or model == "" then model = M.DEFAULTS.model end
	return { endpoint = endpoint, req = M.build_request(data, model) }
end

function M.perform(endpoint, data, req)
	local ok, body = pcall(json.encode, req)
	if not ok then
		return { result = "ERR", message = "请求编码失败: " .. tostring(body) }
	end

	local headers = {
		"Content-Type: application/json",
		"anthropic-version: " .. M.API_VERSION,
	}
	local key = M.api_key(data)
	if key ~= nil then
		-- 密钥含控制字符即拒绝（第二道防线，理由同 llm_wire_openai.perform：
		-- libcurl 对 slist 里的 CR/LF 不做拒绝）
		if M.api_key_has_control_chars(key) then
			return { result = "ERR",
				message = "API key 含控制字符（换行/回车等）：会被原样拼进 x-api-key "
					.. "头并变成额外请求头，拒绝发送" }
		end
		-- ④ Anthropic 用 x-api-key，不是 Authorization: Bearer
		headers[#headers + 1] = "x-api-key: " .. key
	end

	local res = http.post({
		url = endpoint,
		headers = headers,
		body = body,
		timeout = tonumber(data.timeout) or 180,
	})
	if not res.ok then
		return { result = "ERR", message = res.err, curl_rc = res.curl_rc }
	end
	if res.status ~= 200 then
		-- 与 OpenAI 系同款结构化判据：429/5xx 可重试，其余不再试
		local st = tonumber(res.status)
		local retriable = (st == 429) or (st == 529) or (st >= 500 and st <= 599)
		return { result = "ERR",
			message = "HTTP " .. tostring(res.status) .. ": " .. snippet(res.body, 500),
			http_status = st, retriable = retriable, retry_after_ms = res.retry_after_ms }
	end

	local dok, decoded = pcall(json.decode, res.body)
	if not dok then
		return { result = "ERR", message = "响应不是合法 JSON: " .. snippet(res.body, 200) }
	end
	local parsed, perr = M.parse_response(decoded)
	if parsed == nil then
		return { result = "ERR", message = perr .. "；报文片段: " .. snippet(res.body, 300) }
	end
	return parsed
end

return M
