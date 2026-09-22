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

local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local json = dofile(mpath.resolve("lib/json.lua"))
local http = dofile(mpath.resolve("lib/llm_http.lua"))

local M = {}

-- 本协议的缺省模型。宿主可用 --llm-endpoint/--llm-model 覆盖；放在库里是为了
-- 让协议整形可离线单测。
-- B2（2026-09-22）：**endpoint 不再有缺省**。原缺省指向特定第三方网关——「什么
-- 都没配就跑起来」的会话会把含结构脚本、硬件清单、摄像头画面的上下文静默发往
-- 该网关，stderr 注记只是缓解不是防线。改为 fail-closed：没配就在组包时拒绝
-- （驱动把它折成 ERR 契约回给宿主，上电后的第一次模型调用即可见、可读）。
-- 真要用原网关的人显式配置即可（见 require_endpoint 的报错文案）。
M.DEFAULTS = {
	model = "zhipu/glm-5.3-flash",
}

-- 接入点必填判据（B2）：显式给出（参数表）原样返回；缺失/空串即抛错——
-- 驱动 call() 里的 pcall 把它折成「请求组包失败: …」的 ERR 契约表，点名
-- 三种配置途径。放在 M 上是为了离线单测（perform 需要 HTTP，测不到）。
function M.require_endpoint(data)
	local e = data.endpoint
	if e ~= nil and e ~= "" then return e end
	error("未配置 endpoint：本驱动不内置缺省接入点（内置缺省会把上下文发往特定"
		.. "第三方网关）。请用 --llm-endpoint、环境变量 BOT4CJ_LLM_ENDPOINT 或"
		.. "配置文件 endpoint 字段显式指定接入点")
end

local function snippet(s, n)
	s = s or ""
	if #s > n then return s:sub(1, n) .. "...(截断)" end
	return s
end

-- 必填字段显式断言（2026-09-21 审计 AG-15）：json.lua 里那条「字段为 nil 即报错」
-- 是死代码（Lua 表不可能有「键在值为 nil」的项），于是必填字段缺失时会被**静默
-- 丢掉**——真机只回一个含混的 400，看不出是哪个字段、哪条消息。JSON 本身不是
-- 自描述的，唯一能给出可读报错的位置就是组装这些字段的这里。
-- 报错字符串会经宿主的 ERR 折成模型可见文本，故必须点名 what（字段）与 where（消息）。
function M.require_wire_string(v, what, where)
	if type(v) ~= "string" or v == "" then
		error(string.format("请求字段 %s 缺失或非字符串（%s）", what, where))
	end
	return v
end

-- 规范化消息 → provider 消息
-- B3：宿主在 image_url.url 里放 {"__bot_img": N} 游标（图片字节不穿 Json/Lua
-- 边界，否则参数合并与压栈会对同一 base64 各拷一份）。这里在组包期经宿主
-- 注册的全局 __bot_img(N) 落值一次——这是字节从宿主进 Lua 的唯一一次拷贝，
-- 之后 json.encode 把它拼进请求体（最后一次拷贝），POST 用指针零拷贝。
-- fail-closed：宿主未注册通道（版本不匹配/窗口外）、取值报错、游标结果不是
-- 字符串，都响亮报错——折成 ERR 契约回宿主，不发出半截报文。
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

local function to_wire_message(msg, inject_reasoning)
	local role = msg.role
	if role == "tool" then
		-- tool_call_id 是与 assistant.tool_calls 配对的唯一凭据：丢了它真机 400，
		-- 且报错不会告诉你是哪一条 tool 消息出了问题
		return {
			role = "tool",
			tool_call_id = M.require_wire_string(msg.tool_call_id, "tool_call_id", "role=tool 的消息"),
			content = msg.content or "",
		}
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
					id = M.require_wire_string(tc.id, "tool_calls[].id",
						"assistant 消息的第 " .. i .. " 个工具调用"),
					type = "function",
					["function"] = {
						name = M.require_wire_string(tc.name, "tool_calls[].function.name",
							"assistant 消息的第 " .. i .. " 个工具调用"),
						arguments = tostring(tc.arguments or "{}"),
					},
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
	-- 多模态 content（B3）：宿主给的是部件数组，图片 url 可能是游标表——
	-- 在这里落值。其余部件原样透传（宿主的结构即 provider 的结构）。
	if type(msg.content) == "table" then
		local parts = {}
		for i, p in ipairs(msg.content) do
			if type(p) == "table" and p.type == "image_url" and type(p.image_url) == "table" then
				parts[i] = {
					type = "image_url",
					image_url = { url = resolve_image_url(p.image_url.url,
						"第 " .. i .. " 个消息部件") },
				}
			else
				parts[i] = p
			end
		end
		return { role = role, content = parts }
	end
	return { role = role, content = msg.content or "" }
end

-- JSON Schema 的 required 必须是**数组**。宿主传来的 canonical 工具声明里，
-- 无参工具的 required 是空数组；它经 Json→Lua 转换后与空对象无法区分，再编码就
-- 成了 {}。DeepSeek 会严格校验并直接 400：
--   Invalid schema for function 'x': {} is not of type "array"
-- 这里在 schema 语义已知的位置补成显式空数组（缺失也补，语义等价且更明确）。
local function normalize_schema(params)
	params = params or { type = "object" }
	local req = params.required
	if req == nil or (type(req) == "table" and next(req) == nil) then
		params.required = json.empty_array
	end
	return params
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
				["function"] = {
					name = t.name,
					description = t.description,
					parameters = normalize_schema(t.parameters),
				},
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
	-- PL-6：形态非法时点名返回，而不是直接交给 ipairs。
	local function type_err(field, want, got)
		return nil, "响应字段 " .. field .. " 形态非法（期望 " .. want
			.. "，实际 " .. got .. "）"
	end
	if type(decoded.choices) ~= "table" then
		return type_err("choices", "数组", type(decoded.choices))
	end
	local choice = decoded.choices[1]
	if choice == nil then return nil, "响应缺少 choices[1].message" end
	if type(choice) ~= "table" then
		return type_err("choices[1]", "对象", type(choice))
	end
	local msg = choice.message
	if msg == nil then return nil, "响应缺少 choices[1].message" end
	if type(msg) ~= "table" then
		return type_err("choices[1].message", "对象", type(msg))
	end

	-- PL-3（2026-09-22）：content 归一化。新版 OpenAI 协议允许 content 为
	-- [{type="text",text=...},{type="image_url",...}] 的数组形态，部分兼容网关
	-- 也这么返回。宿主只认字符串（src/agentproto.cpp 的 content->isStr()），
	-- 原样透传会让整轮以「既没有返回文本也没有工具调用」告终——报错完全不指向
	-- 真因。这里把 text 段按序拼接；**非 text 段丢弃但计数**并记进 extras，
	-- 让「模型给了别的内容而我没看见」在审计里可见（不谎报状态）。
	local parts_dropped = 0
	local function flatten_content(c)
		-- SAN-11：非表、非字符串的 content（数字/布尔）**归一成空串**。
		-- 原样透传会让宿主只认字符串的判据把它读成"没有文本"，最终报
		-- 「既没有返回文本也没有工具调用」——报错完全不指向真因（同一个坑
		-- PL-3 已经踩过一次：数组形态 content）。归一后至少文本是空的、
		-- tool_calls 照常解析。
		if type(c) ~= "table" then
			if type(c) == "string" or c == nil then return c end
			return ""
		end
		-- 数组形态照常逐段取 text；**非数组但有键**的表（如网关直接回单个
		-- part 对象 {type="text",text=...}，或某种畸形表）当作一段处理，
		-- 否则 ipairs 一个都遍历不到，会静默变成空文本。
		local segs = c
		if #c == 0 and next(c) ~= nil then segs = { c } end
		local parts = {}
		for _, seg in ipairs(segs) do
			if type(seg) == "table" and seg.type == "text" and type(seg.text) == "string" then
				parts[#parts + 1] = seg.text
			else
				parts_dropped = parts_dropped + 1
			end
		end
		return table.concat(parts, "")
	end

	local out = { result = "OK", content = flatten_content(msg.content) or "" }

	local tcs = msg.tool_calls
	-- PL-6（2026-09-22）：形态非法时点名返回，而不是直接交给 ipairs——对非表值
	-- ipairs 抛的是 "bad argument #1 to 'ipairs'"，经 pcall 浮上来只剩一句 Lua
	-- 内部报错，既不点名字段也不给报文片段，与上一行「响应缺少 choices[1].message」
	-- 的诚实口径不一致。调用点（M.perform :338-341）已把 nil+原因 折成
	-- ERR 并附报文片段，这里只需给出点名原因。
	if tcs ~= nil and type(tcs) ~= "table" then
		return nil, "响应字段 tool_calls 形态非法（期望数组，实际 " .. type(tcs) .. "）"
	end
	local bad_tool_calls = 0
	if tcs ~= nil then
		local calls = {}
		for i, tc in ipairs(tcs) do
			-- SAN-6（2026-09-22）：**元素**也要设防。PL-6 只守了容器类型，
			-- `[42]` 或 `[{"function":42}]` 仍会抛裸 Lua 错误
			-- （attempt to index local 'tc'/'fn'），逃出驱动"返 ERR 表、绝不抛"
			-- 的契约。元素不是表时**跳过并计数**（与 PL-3 丢弃非 text 段同口径：
			-- 丢弃但如实记账，不静默），而不是让整轮失败。
			if type(tc) ~= "table" then
				bad_tool_calls = bad_tool_calls + 1
			else
				local fn = tc["function"]
				if fn ~= nil and type(fn) ~= "table" then
					bad_tool_calls = bad_tool_calls + 1
				else
					fn = fn or {}
					-- SAN-6：**不再合成 call_<i>**。宿主已经有轮次相关的合成方案
					-- （agentloop 的 call_<turn>_<i>，专为"漏 id 的网关"而设），
					-- 而驱动的 call_<i> 每轮从 1 重来，会把宿主那条路径遮住并
					-- 造成跨轮重复 id（真机 400 answered more than once）。
					-- 这里只透传：id 为空时由宿主填。
					calls[#calls + 1] = {
						id = type(tc.id) == "string" and tc.id or "",
						name = type(fn.name) == "string" and fn.name or "",
						arguments = fn.arguments ~= nil and tostring(fn.arguments) or "{}",
					}
				end
			end
		end
		-- 丢弃计数先存起来：extras 表在本函数末尾统一构造（SAN-6）
		if #calls > 0 then out.tool_calls = calls end
	end

	-- extras：provider 特有的附加字段，宿主原样逐轮带回，驱动决定怎么用
	local extras = {}
	if msg.reasoning_content ~= nil and msg.reasoning_content ~= "" then
		extras.reasoning_content = msg.reasoning_content
	end
	-- PL-3：被丢弃的非 text content 段计数（只有真丢了才挂，保持既有响应形态不变）
	if parts_dropped > 0 then extras.content_parts_dropped = parts_dropped end
	-- SAN-6：被丢弃的畸形工具调用元素计数（同上，丢弃但如实记账）
	if bad_tool_calls > 0 then extras.dropped_bad_tool_calls = bad_tool_calls end
	if next(extras) ~= nil then out.extras = extras end
	-- SAN-9：provider 的 usage **原样透传**（宿主只挑 prompt_tokens /
	-- completion_tokens 记账，不推算、不计费）。只在存在时挂键，
	-- 保持既有响应形态不变。
	if type(decoded.usage) == "table" then out.usage = decoded.usage end

	return out
end

-- API key：先看调用参数表（宿主注入），否则按环境变量链取
-- （2026-09-20 评估：命令行已不提供密钥选项，宿主注入的值同样来自环境变量/配置文件）
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
-- OpenAI 库已无缺省接入点（B2，fail-closed），本注记现只由 DeepSeek 库使用：
-- 其缺省端点指向厂商官方 API（api.deepseek.com），每次进程首次回退时向
-- stderr 注记一次（io.stderr 未被沙箱中和，驱动注记惯例见 lib/daemon_tid.lua；
-- 不打印密钥与报文内容，只提示「正在用哪个端点」），让「什么都没配」的会话
-- 可见自己的数据流向；走显式配置（CLI/配置文件/环境链）不注记。
--
-- 放在 M 上而不是文件内局部（2026-09-21 审计 AG-13②）：DeepSeek 库有自己的
-- DEFAULTS.endpoint，之前用的是**文件内局部**函数，于是只有 OpenAI 有这行注记，
-- 而 DeepSeek 的缺省端点恰恰指向外部厂商 API——最该提示的那条链路反而静默。
-- 提到模块上让两个库共用同一份「每次进程只注记一次」的状态与同一条文案。
local noted_default_endpoint = false
function M.note_default_endpoint(url)
	if noted_default_endpoint then return end
	noted_default_endpoint = true
	io.stderr:write("[llm-wire] 未配置 endpoint，使用驱动内置缺省 " .. url
		.. "；可经 --llm-endpoint 或配置文件 endpoint 字段覆盖\n")
	io.stderr:flush()
end

function M.request(data, plan)
	-- B2：接入点必填（fail-closed），缺省模型仍可用
	local endpoint = M.require_endpoint(data)
	local model = data.model
	if model == nil or model == "" then model = M.DEFAULTS.model end
	return { endpoint = endpoint, req = M.build_request(data, model, plan) }
end

-- 密钥字符集判据（2026-09-21 审计 AG-14）：控制字符（<0x20 与 0x7F）一律拒绝。
-- 与宿主侧 src/llmconfig.cpp 的 validateApiKeyChars 同判据——两边必须一致，
-- 否则「宿主放行的密钥被驱动拒掉」会表现成一个只在真机出现的怪错。
-- 单独成函数是为了能被离线用例直接钉住（perform 需要 HTTP，测不到这一行）。
function M.api_key_has_control_chars(key)
	return type(key) == "string" and key:find("[%z\1-\31\127]") ~= nil
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
	if key ~= nil then
		-- 密钥含控制字符即拒绝：libcurl 对 slist 里的 CR/LF **不做拒绝**，一个
		-- 多行密钥文件会把换行后的内容变成额外的请求头发给网关（实测复现）。
		-- 宿主侧已在三条来源汇合处校验，这里是「站在建头这一行」的第二道——
		-- 报文一旦建好就没有别的地方能拦住它，且本层不能假设调用方一定校验过
		-- （.mt 脚本可以直接调驱动）。
		if M.api_key_has_control_chars(key) then
			return { result = "ERR",
				message = "API key 含控制字符（换行/回车等）：会被原样拼进 "
					.. "Authorization 头并变成额外请求头，拒绝发送" }
		end
		headers[#headers + 1] = "Authorization: Bearer " .. key
	end

	local res = http.post({
		url = endpoint,
		headers = headers,
		body = body,
		-- 只给缺省；越界/非法值由 llm_http.clamp_timeout 单点钳到 [1, 600]
		-- （libcurl 的 timeout=0 是不限时，不能让配置把它透传下去）
		timeout = tonumber(data.timeout) or 180,
	})
	if not res.ok then
		-- SAN-3（2026-09-22）：传输层失败带上**结构化判据**。宿主此前靠
		-- 匹配人话子串（CURLE_* / "curl error" / "网络"）判"能不能重试"，
		-- 而真实错误长这样：`curl rc=7: Could not connect to server`
		-- （curl_easy_strerror 返回的是英文散文）——一个子串都不匹配，
		-- 于是 max_retries 在生产路径上从未生效。这里把数值退出码透出去，
		-- 由宿主的 classifyDriverError 判类；message 原样保留（人读）。
		return { result = "ERR", message = res.err, curl_rc = res.curl_rc }
	end
	if res.status ~= 200 then
		-- 429/5xx 是"稍后再试"，4xx 其余是"别再试了"——两者必须能分开，
		-- 否则要么限流不重试（现状），要么把 401 也重试（烧配额）。
		-- retriable 由本层显式给出（它知道 HTTP 语义），retry_after_ms 来自
		-- llm_http 的头回调（服务端说了等多久就等多久，钳制由宿主做）。
		local st = tonumber(res.status)
		local retriable = (st == 429) or (st >= 500 and st <= 599)
		return { result = "ERR", message = "HTTP " .. tostring(res.status) .. ": " .. snippet(res.body, 500),
			http_status = st, retriable = retriable, retry_after_ms = res.retry_after_ms }
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
