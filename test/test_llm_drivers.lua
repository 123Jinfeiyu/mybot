-- llm_wire_*.lua 单元测试：两个协议驱动的「规范化 → provider 报文」整形规则
-- 用法（项目根目录）: luajit test/test_llm_drivers.lua
--
-- 完全离线：只断言请求整形与响应映射。真实 HTTP（curl + TLS + 超时）由
-- scripts/agent_e2e.sh 用本地假模型服务覆盖。

local tester = dofile("test/lib/tester.lua")
local check = tester.check

local json = dofile("modules/lib/json.lua")
local openai = dofile("modules/lib/llm_wire_openai.lua")
local deepseek = dofile("modules/lib/llm_wire_deepseek.lua")

local function sample()
	return {
		endpoint = "http://127.0.0.1:1/v1/chat/completions",
		model = "m0",
		max_tokens = 4096,
		temperature = 0.7,
		top_p = 0.9,
		messages = {
			{ role = "system", content = "sys" },
			{ role = "user", content = "hi" },
			{ role = "assistant", content = "",
			  tool_calls = { { id = "c1", name = "t", arguments = "{}" } },
			  extras = { reasoning_content = "上一轮思考" } },
			{ role = "tool", tool_call_id = "c1", content = "ok" },
		},
		tools = { { name = "t", description = "d",
			parameters = { type = "object", properties = {}, required = {} } } },
	}
end

-- ================= OpenAI 兼容（当前协议） =================

do
	local r = openai.request(sample())
	check("OpenAI: 用传入的接入点", r.endpoint == "http://127.0.0.1:1/v1/chat/completions")
	check("OpenAI: 用传入的模型", r.req.model == "m0")
	check("OpenAI: 非流式", r.req.stream == false)
	check("OpenAI: 保留 temperature", r.req.temperature == 0.7)
	check("OpenAI: 保留 top_p", r.req.top_p == 0.9)
	check("OpenAI: max_tokens 透传", r.req.max_tokens == 4096)
	check("OpenAI: 带 tools 时给 tool_choice=auto", r.req.tool_choice == "auto")
	check("OpenAI: 工具声明包成 function", r.req.tools[1].type == "function"
		and r.req.tools[1]["function"].name == "t")
	check("OpenAI: 不引入 thinking 字段", r.req.thinking == nil)
	check("OpenAI: 不回填 reasoning_content（非本协议语义）",
		r.req.messages[3].reasoning_content == nil)
	check("OpenAI: 带 tool_calls 且无过渡语时 content 为 null",
		r.req.messages[3].content == json.null)
	check("OpenAI: tool 消息带 tool_call_id", r.req.messages[4].tool_call_id == "c1")
	check("OpenAI: 4 条消息顺序不变", #r.req.messages == 4
		and r.req.messages[1].role == "system" and r.req.messages[4].role == "tool")
end

do
	local d = sample()
	d.endpoint, d.model = "", ""
	local r = openai.request(d)
	check("OpenAI: 缺省接入点用驱动自带值", r.endpoint == openai.DEFAULTS.endpoint)
	check("OpenAI: 缺省模型用驱动自带值", r.req.model == openai.DEFAULTS.model)
end

do
	local d = sample()
	d.tools = {}
	local r = openai.request(d)
	check("OpenAI: 无工具时不发 tools", r.req.tools == nil)
	check("OpenAI: 无工具时不发 tool_choice", r.req.tool_choice == nil)
end

-- ================= 工具 schema 的 required 必须是数组 =================
do
	-- 无参工具的 required 在 canonical 侧是空数组，经 Json→Lua 退化为空表，
	-- 直接编码会变成 {}——DeepSeek 会拒收：
	--   Invalid schema for function 'x': {} is not of type "array"
	local d = sample()
	d.tools = {
		{ name = "noargs", description = "无参",
		  parameters = { type = "object", properties = {}, required = {} } },
		{ name = "noRequiredKey", description = "缺 required 键",
		  parameters = { type = "object", properties = {} } },
		{ name = "withArgs", description = "有参",
		  parameters = { type = "object", properties = { a = { type = "string" } },
				 required = { "a" } } },
	}
	local r = openai.request(d)
	local tools = r.req.tools
	check("required 空表 → 显式空数组",
		tools[1]["function"].parameters.required == json.empty_array)
	check("缺失 required → 补成空数组",
		tools[2]["function"].parameters.required == json.empty_array)
	check("非空 required 原样保留",
		tools[3]["function"].parameters.required[1] == "a")
	check("properties 空表仍按对象编码",
		type(tools[1]["function"].parameters.properties) == "table")

	-- 光有哨兵不够，落到字节上必须真的是 []
	local encoded = json.encode(r.req)
	check("线上报文含 \"required\":[]", encoded:find('"required":[]', 1, true) ~= nil)
	check("非空 required 编码为数组", encoded:find('"required":["a"]', 1, true) ~= nil)
end

-- ================= DeepSeek =================

do
	local d = sample()
	d.endpoint = ""		-- 显式传入优先；只清空接入点看是否回落到驱动缺省
	local r = deepseek.request(d)
	check("DeepSeek: 缺省接入点", r.endpoint == deepseek.DEFAULTS.endpoint)
	check("DeepSeek: 缺省模型", r.req.model == "m0")	-- 传了 model 就用传入的
	check("DeepSeek: 思考模式开启", type(r.req.thinking) == "table"
		and r.req.thinking.type == "enabled")
	check("DeepSeek: 思考模式不发 temperature", r.req.temperature == nil)
	check("DeepSeek: top_p 抬到下限 0.95", r.req.top_p == 0.95)
	check("DeepSeek: 回传历史 reasoning_content",
		r.req.messages[3].reasoning_content == "上一轮思考")
	check("DeepSeek: 仍带 tools", r.req.tools[1]["function"].name == "t")
end

do
	local d = sample()
	d.thinking = "disabled"
	local r = deepseek.request(d)
	check("DeepSeek: 关闭思考 → thinking.disabled", r.req.thinking.type == "disabled")
	check("DeepSeek: 关闭思考 → 保留 temperature", r.req.temperature == 0.7)
	check("DeepSeek: 关闭思考 → 不发 top_p（该模式恒为 1.0）", r.req.top_p == nil)
	check("DeepSeek: 关闭思考 → 不回填 reasoning_content",
		r.req.messages[3].reasoning_content == nil)
end

do
	local d = sample()
	d.top_p = 0.5		-- 低于下限：应被抬到 0.95 而不是原样发出
	local r = deepseek.request(d)
	check("DeepSeek: 低于下限的 top_p 被抬升", r.req.top_p == 0.95)
end

do
	local d = sample()
	d.reasoning_effort = "low"
	local r = deepseek.request(d)
	check("DeepSeek: reasoning_effort 透传", r.req.reasoning_effort == "low")
end

do
	local d = sample()
	d.endpoint, d.model = "", ""
	local r = deepseek.request(d)
	check("DeepSeek: 缺省接入点用驱动自带值", r.endpoint == deepseek.DEFAULTS.endpoint)
	check("DeepSeek: 缺省模型用驱动自带值", r.req.model == deepseek.DEFAULTS.model)
end

-- ================= 响应映射（两协议共用） =================

local function parse_of(lib)
	local wire = lib == "openai" and openai or deepseek
	local decoded = json.decode([[
	{"choices":[{"message":{"role":"assistant","content":"你好",
	 "reasoning_content":"我先想了想",
	 "tool_calls":[{"id":"c9","type":"function",
	   "function":{"name":"run_action_script","arguments":"{\"script\":\"std.mt\"}"}}]}}]}
	]])
	return wire.parse_response(decoded)
end

for _, lib in ipairs({ "openai", "deepseek" }) do
	local res = parse_of(lib)
	check(lib .. ": 解析 content", res.content == "你好")
	check(lib .. ": 解析 tool_calls", #res.tool_calls == 1 and res.tool_calls[1].id == "c9")
	check(lib .. ": 展平 function 字段", res.tool_calls[1].name == "run_action_script")
	check(lib .. ": arguments 原样保留",
		res.tool_calls[1].arguments == '{"script":"std.mt"}')
	check(lib .. ": reasoning_content 进 extras",
		res.extras ~= nil and res.extras.reasoning_content == "我先想了想")
end

do
	local decoded = json.decode('{"choices":[{"message":{"role":"assistant","content":"纯文本"}}]}')
	local res = openai.parse_response(decoded)
	check("无工具调用时不挂 tool_calls", res.tool_calls == nil)
	check("无 reasoning_content 时不挂 extras", res.extras == nil)
	check("content 保留", res.content == "纯文本")
end

do
	local res, err = openai.parse_response(json.decode('{"choices":[]}'))
	check("缺 message 时返回 nil+原因", res == nil and tostring(err):find("message") ~= nil)
end

-- ================= API key 取值 =================

check("API key: 传入优先", openai.api_key({ api_key = "k1" }) == "k1")
check("API key: 缺失返回 nil", openai.api_key({}) == nil)

-- ================= 超时钳制（P1-3） =================

do
	local http = dofile("modules/lib/llm_http.lua")
	-- libcurl 语义：CURLOPT_TIMEOUT=0 是「不限时」，负值亦透传 → 一个配置项
	-- 就能让单次模型调用永远挂着。钳到 [1, 600] 后这些值不再可能到达 curl。
	check("超时 0 → 下限 1", http.clamp_timeout(0, 180) == 1)
	check("超时负数 → 下限 1", http.clamp_timeout(-5, 180) == 1)
	check("超时 1e30 → 上限 600", http.clamp_timeout(1e30, 180) == 600)
	check("超时 99999 → 上限 600", http.clamp_timeout(99999, 180) == 600)
	check("区间内数值原样", http.clamp_timeout(42, 180) == 42)
	check("小数向下取整", http.clamp_timeout(2.9, 180) == 2)
	check("数字字符串可解析", http.clamp_timeout("30", 180) == 30)
	check("非数字回落缺省", http.clamp_timeout("abc", 180) == 180)
	check("缺省值按参数生效", http.clamp_timeout(nil, 15) == 15)
end


-- ================= API key 控制字符判据（AG-14） =================
-- libcurl 对 curl_slist 里的 CR/LF 不做拒绝：多行密钥会把换行之后的内容变成
-- **额外的请求头**发给网关（用真实二进制复现过）。宿主侧 llmconfig.cpp 的
-- validateApiKeyChars 与这里必须同判据，故两边各钉一遍。
check("AG-14：普通密钥放行", not openai.api_key_has_control_chars("sk-abc123"))
check("AG-14：空串放行（无控制字符）", not openai.api_key_has_control_chars(""))
check("AG-14：CR 拒绝", openai.api_key_has_control_chars("sk-a\rb"))
check("AG-14：LF 拒绝", openai.api_key_has_control_chars("sk-a\nb"))
check("AG-14：CRLF 拒绝", openai.api_key_has_control_chars("sk-a\r\nX-Injected: yes"))
check("AG-14：TAB 拒绝", openai.api_key_has_control_chars("sk-a\tb"))
check("AG-14：NUL 拒绝", openai.api_key_has_control_chars("sk-a\0b"))
check("AG-14：DEL(0x7F) 拒绝", openai.api_key_has_control_chars("sk-a\127b"))
check("AG-14：空格与可打印字符放行", not openai.api_key_has_control_chars("sk-a b~c!"))
check("AG-14：高位字节不属控制字符（UTF-8 密钥不误伤）",
	not openai.api_key_has_control_chars("sk-\229\175\134"))
check("AG-14：非字符串入参判假（不抛错）", not openai.api_key_has_control_chars(nil))

-- ================= 句柄收尾不许留悬垂回调（AG-16） =================
-- 旧实现只 cb:free() 不清句柄，WRITEFUNCTION 仍指在已释放的回调上；当前只因
-- Session:post 每次都先 curl_easy_reset 才安全。Session.handle 是公开字段，
-- 任何「同句柄 perform 两次 / 加重试」的重构都会把它变成内存安全问题
-- （实测：post 后直接 perform 报 bad callback；槽位复用时 SIGSEGV）。
-- 修法是在 free 之前先 reset，使句柄自身满足「空闲即无武装」的不变量。
do
	local http = dofile("modules/lib/llm_http.lua")
	local s = http.session()
	check("AG-16：能建会话", s ~= nil, tostring(s))
	if s ~= nil then
		-- 端口 1 必然连不上：这里只要走到「perform 返回」即可，不需要成功响应
		local res = s:post({ url = "http://127.0.0.1:1/v1/x", body = "{}", timeout = 1 })
		check("AG-16：post 返回（连不上也不崩）", type(res) == "table", tostring(res))
		-- 关键断言：post 之后句柄必须是「未武装」的——直接对同一句柄 perform
		-- 不得触碰已释放的回调。libcurl 对「无 URL」返回 CURLE_URL_MALFORMAT，
		-- 不会调用任何回调，所以「返回」本身就是句柄已被 reset 的证据；
		-- 旧实现下这里会调用已释放的 WRITEFUNCTION（bad callback / SIGSEGV）。
		local ffi = require("ffi")
		ffi.cdef[[
int curl_easy_perform(void *h);
const char *curl_easy_strerror(int code);
]]
		local curl = ffi.load("curl")
		local ok, rc = pcall(function() return curl.curl_easy_perform(s.handle) end)
		check("AG-16：post 后句柄无悬垂回调（直接 perform 安全返回）", ok,
			tostring(rc))
		if ok then
			check("AG-16：返回的是「无 URL」类错误码（证明没走到回调）",
				tonumber(rc) ~= 0, "rc=" .. tostring(tonumber(rc)))
		end
		s:close()
	end
end


-- ================= 必填字段的显式断言（AG-15） =================
-- json.lua 的「字段为 nil 即报错」是死代码（Lua 表不可能有「键在值为 nil」的项），
-- 所以必填字段缺失时会被**静默丢掉**，真机只回一个含混的 400。现在由组装这些
-- 字段的地方（llm_wire_openai 的 require_wire_string）显式断言，报错必须点名
-- 是哪个字段——这正是「静默丢字段」与「可读报错」的分界。
do
	local function err_of(fn)
		local ok, e = pcall(fn)
		return ok, tostring(e)
	end

	-- tool 消息缺 tool_call_id
	local d1 = sample()
	d1.messages[4].tool_call_id = nil
	local ok1, e1 = err_of(function() return openai.build_request(d1, "m0") end)
	check("AG-15：tool 消息缺 tool_call_id 时抛错（不再静默丢字段）", not ok1, e1)
	check("AG-15：报错点名字段与消息种类",
		e1:find("tool_call_id", 1, true) ~= nil and e1:find("role=tool", 1, true) ~= nil, e1)

	-- assistant.tool_calls[].id 缺失
	local d2 = sample()
	d2.messages[3].tool_calls[1].id = nil
	local ok2, e2 = err_of(function() return openai.build_request(d2, "m0") end)
	check("AG-15：tool_calls[].id 缺失时抛错", not ok2, e2)
	check("AG-15：报错点名 tool_calls[].id", e2:find("tool_calls[]", 1, true) ~= nil, e2)

	-- assistant.tool_calls[].name 缺失
	local d3 = sample()
	d3.messages[3].tool_calls[1].name = nil
	local ok3, e3 = err_of(function() return openai.build_request(d3, "m0") end)
	check("AG-15：tool_calls[].function.name 缺失时抛错", not ok3, e3)
	check("AG-15：报错点名 function.name", e3:find("function.name", 1, true) ~= nil, e3)

	-- 合法样例必须仍然组包成功（别把断言写成误杀）
	local ok4, r4 = pcall(function() return openai.build_request(sample(), "m0") end)
	check("AG-15：合法样例仍能组包", ok4 and type(r4) == "table", tostring(r4))
	check("AG-15：合法样例的 tool_call_id 进报文",
		ok4 and r4.messages[4].tool_call_id == "c1")

	-- arguments 缺省时补 "{}"（模型可能给出 nil 参数体，不该让整轮失败）
	local d5 = sample()
	d5.messages[3].tool_calls[1].arguments = nil
	local ok5, r5 = pcall(function() return openai.build_request(d5, "m0") end)
	check("AG-15：arguments 缺失补 {} 而不是崩", ok5
		and r5.messages[3].tool_calls[1]["function"].arguments == "{}", tostring(r5))
end


-- ========== 组包失败必须走 ERR 契约（AG-15 落地口径） ==========
-- 驱动对宿主的契约是「返 ERR 表，不抛 Lua error」：跨语言边界上「抛」会被宿主
-- 折成一条带 Lua 栈帧的报错，文案可读性远不如点名到字段的那句。wire.request
-- 现在会为缺失的必填字段抛错，故 call() 必须把它折成 ERR。
-- （driver 是驱动级文件，dofile 后会覆盖全局 build/call/destroy —— 本文件用它
-- 之后就结束，不再依赖那两个全局。）
do
	local mpath = dofile("modules/lib/mpath.lua")
	dofile(mpath.resolve("lib/drv_common.lua"))
	dofile("modules/LlmOpenAI.lua")
	local bad = {
		endpoint = "http://127.0.0.1:1/v1/chat/completions", model = "m",
		messages = { { role = "tool", content = "x" } },	-- 缺 tool_call_id
		tools = {},
	}
	local ok, res = pcall(call, bad)
	check("AG-15：组包失败时驱动返 ERR 契约（不抛 Lua error）",
		ok and type(res) == "table" and res.result == "ERR",
		tostring(ok) .. "/" .. (type(res) == "table" and tostring(res.result) or tostring(res)))
	check("AG-15：ERR 文案保留字段级原因",
		ok and type(res) == "table" and tostring(res.message):find("tool_call_id", 1, true) ~= nil,
		type(res) == "table" and tostring(res.message) or "")
end


-- ================= DeepSeek 思考模式开关（AG-17） =================
-- 文档与本库注释都承诺「disabled/off/false 关闭」，但 data.thinking 是 JSON 值：
-- 宿主走 wantString，只有 .mt 动作脚本与结构脚本参数能传布尔。JSON 的 false 是
-- 最自然的一种「关掉」写法，此前被无视（实测 thinking=false 时请求体照样带
-- {"type":"enabled"}），与注释承诺矛盾。
do
	local function wire_thinking(data)
		local req = deepseek.build_request(data, "deepseek-chat")
		return req.thinking and req.thinking.type or "<无>"
	end

	check("AG-17：缺省开启（DeepSeek 默认即开启）", wire_thinking({}) == "enabled")
	check("AG-17：字符串 disabled 关闭", wire_thinking({ thinking = "disabled" }) == "disabled")
	check("AG-17：字符串 off 关闭", wire_thinking({ thinking = "off" }) == "disabled")
	check("AG-17：字符串 \"false\" 关闭", wire_thinking({ thinking = "false" }) == "disabled")
	-- 本轮修复的那一支：
	check("AG-17：布尔 false 关闭（修复前仍为 enabled）",
		wire_thinking({ thinking = false }) == "disabled",
		wire_thinking({ thinking = false }))
	check("AG-17：布尔 true 保持开启", wire_thinking({ thinking = true }) == "enabled")

	-- 关闭思考模式必须连带关掉 reasoning_content 回传（有状态协议的另一半：
	-- 非思考模式回传该字段会被真机拒）
	local off = deepseek.build_request({ thinking = false,
		messages = { { role = "assistant", content = "",
			tool_calls = { { id = "c1", name = "t", arguments = "{}" } },
			extras = { reasoning_content = "上一轮思考" } } } }, "m0")
	check("AG-17：布尔 false 关闭时不再回传 reasoning_content",
		off.messages[1].reasoning_content == nil, tostring(off.messages[1].reasoning_content))
	local on = deepseek.build_request({ messages = { { role = "assistant", content = "",
		tool_calls = { { id = "c1", name = "t", arguments = "{}" } },
		extras = { reasoning_content = "上一轮思考" } } } }, "m0")
	check("AG-17：开启时仍回传 reasoning_content（有状态要求）",
		on.messages[1].reasoning_content == "上一轮思考")
end

-- ========== 缺省端点注记两个库都有（AG-13②） ==========
do
	check("AG-13②：openai 暴露了注记函数（提到模块上供两库共用）",
		type(openai.note_default_endpoint) == "function")
	check("AG-13②：deepseek 的 request 在缺省端点上也会注记",
		type(deepseek.request) == "function")
	-- 注记只在进程内首次回退时打一次，且只写 stderr；这里断言行为而不抓输出：
	-- 显式给端点时不得改变返回的 endpoint
	local r = deepseek.request({ endpoint = "http://127.0.0.1:1/x", messages = {} })
	check("AG-13②：显式端点优先，不被缺省覆盖", r.endpoint == "http://127.0.0.1:1/x", r.endpoint)
	local d = deepseek.request({ messages = {} })
	check("AG-13②：无端点时回落到 DeepSeek 内置缺省",
		d.endpoint == deepseek.DEFAULTS.endpoint, d.endpoint)
end

tester.summary("llm 协议驱动单测")
