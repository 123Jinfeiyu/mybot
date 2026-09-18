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

tester.summary("llm 协议驱动单测")
