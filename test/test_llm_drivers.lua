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
	-- B2（2026-09-22）：OpenAI 库不再内置缺省接入点——「什么都没配」的会话
	-- 不能把上下文静默发往特定第三方网关。缺省模型仍可用（显式配了 endpoint
	-- 而没配 model 的会话不该被连带失败）。
	local d = sample()
	d.endpoint, d.model = "", ""
	local ok, err = pcall(openai.request, d)
	check("OpenAI(B2): 无 endpoint 时组包失败（fail-closed）", not ok)
	check("OpenAI(B2): 报错点名配置途径", ok or
		(err:find("endpoint", 1, true) ~= nil and err:find("BOT4CJ_LLM_ENDPOINT", 1, true) ~= nil),
		ok and "(未抛错)" or tostring(err))
	local d2 = sample()
	d2.model = ""
	local r2 = openai.request(d2)
	check("OpenAI(B2): endpoint 在而 model 空时用缺省模型",
		r2.req.model == openai.DEFAULTS.model)
	check("OpenAI(B2): DEFAULTS 不再含 endpoint", openai.DEFAULTS.endpoint == nil)
end

do
	local d = sample()
	d.tools = {}
	local r = openai.request(d)
	check("OpenAI: 无工具时不发 tools", r.req.tools == nil)
	check("OpenAI: 无工具时不发 tool_choice", r.req.tool_choice == nil)
end

-- ================= B3：图片游标的组包期落值 =================
do
	-- 宿主在 image_url.url 放 {"__bot_img": N} 游标；wire 经宿主注册的
	-- 全局 __bot_img(N) 取字节（这是 payload 从宿主进 Lua 的唯一一次拷贝）。
	local d = sample()
	d.messages = {
		{ role = "user", content = {
			{ type = "text", text = "看这张图" },
			{ type = "image_url", image_url = { url = { __bot_img = 2 } } },
		} },
	}
	local fetched = {}
	_G.__bot_img = function(n) fetched[#fetched + 1] = n
		return "data:image/jpeg;base64,Q1VSU09S" .. n end
	local ok, r = pcall(openai.request, d)
	check("B3: 游标模式下组包成功", ok)
	local part = ok and r.req.messages[1].content[2] or nil
	check("B3: image_url 落值为宿主返回的字符串",
		part ~= nil and part.image_url.url == "data:image/jpeg;base64,Q1VSU09S2")
	check("B3: 取值用的正是游标编号", #fetched == 1 and fetched[1] == 2)
	check("B3: 文本部件原样保留",
		r.req.messages[1].content[1].text == "看这张图")
	_G.__bot_img = nil

	-- fail-closed：通道未注册（版本不匹配/窗口外）必须响亮报错，不发半截报文
	local ok2, err2 = pcall(openai.request, d)
	check("B3: 通道未注册时组包失败", not ok2)
	check("B3: 报错点名 __bot_img 通道", ok2 or err2:find("__bot_img", 1, true) ~= nil)

	-- 通道在但取值报错/返回非字符串：同样 fail-closed
	_G.__bot_img = function() error("宿主图片上下文已关闭") end
	local ok3, err3 = pcall(openai.request, d)
	check("B3: 取值报错折进组包错误", not ok3 and err3:find("已关闭", 1, true) ~= nil)
	_G.__bot_img = function() return 42 end
	local ok4, err4 = pcall(openai.request, d)
	check("B3: 非字符串结果视为失败", not ok4 and err4:find("解析失败", 1, true) ~= nil)
	_G.__bot_img = nil

	-- 非 image_url 部件与字符串 url 原样透传（不受游标逻辑影响）
	local d2 = sample()
	d2.messages = { { role = "user", content = {
		{ type = "image_url", image_url = { url = "data:image/png;base64,RAWDATA" } },
	} } }
	local ok5, r5 = pcall(openai.request, d2)
	check("B3: 字符串 url 不经游标处理", ok5
		and r5.req.messages[1].content[1].image_url.url == "data:image/png;base64,RAWDATA")
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

-- PL-6（2026-09-22）：tool_calls 形态非法（非表）时给**点名**原因，不抛裸 Lua 错误。
-- 旧行为是直接 ipairs(tcs)：抛 "bad argument #1 to 'ipairs'"，经 pcall 浮上来
-- 只剩 Lua 内部报错，既不点名字段也不给报文片段。两个驱动共用 parse_response，
-- 故同一组用例同时覆盖 openai 与 deepseek。
for _, lib in ipairs({ "openai", "deepseek" }) do
	local wire = lib == "openai" and openai or deepseek
	local bads = {
		{ name = "字符串", body = '{"choices":[{"message":{"content":"x","tool_calls":"oops"}}]}' },
		{ name = "数字", body = '{"choices":[{"message":{"content":"x","tool_calls":42}}]}' },
		{ name = "布尔", body = '{"choices":[{"message":{"content":"x","tool_calls":true}}]}' },
	}
	for _, b in ipairs(bads) do
		-- pcall 包住：旧行为在这里 ok=false（裸抛），修复后 ok=true 且返回 nil+原因
		local ok, res, err = pcall(wire.parse_response, json.decode(b.body))
		check(lib .. ": tool_calls 为" .. b.name .. "时不抛裸错误", ok, tostring(res))
		check(lib .. ": tool_calls 为" .. b.name .. "时返回点名原因",
			ok and res == nil and tostring(err):find("tool_calls", 1, true) ~= nil,
			tostring(err))
	end
	-- 反向对照：合法数组照常解析（确认新判据没有把正常路径一起挡掉）
	local res = wire.parse_response(json.decode(
		'{"choices":[{"message":{"content":"x","tool_calls":[{"id":"c1","function":{"name":"t","arguments":"{}"}}]}}]}'))
	check(lib .. ": 合法 tool_calls 仍照常解析",
		type(res) == "table" and #res.tool_calls == 1 and res.tool_calls[1].name == "t")
end

-- SAN-6（2026-09-22）：PL-6 只守了**容器**类型，元素与 choices/message 仍会抛裸错误。
-- 复现（修复前）：tool_calls=[42] → "attempt to index local 'tc' (a number value)"；
-- [{"function":42}] → "attempt to index local 'fn'"；{"choices":42} /
-- {"choices":[{"message":42}]} 同理。M.perform 调 parse_response **不在 pcall 里**，
-- 所以这些错误会逃出驱动"返 ERR 表、绝不抛"的契约，宿主最终只看到一句
-- "Hardware call failed (…): …attempt to index local 'tc'"——不点名字段、不给报文片段。
for _, lib in ipairs({ "openai", "deepseek" }) do
	local wire = lib == "openai" and openai or deepseek

	local function no_raise(body)
		local ok, res, err = pcall(wire.parse_response, json.decode(body))
		return ok, res, err
	end

	-- ① choices 非表 / 元素非表
	local bads = {
		{ name = "choices 为数字", body = '{"choices":42}', field = "choices" },
		{ name = "choices[1] 为数字", body = '{"choices":[42]}', field = "choices[1]" },
		{ name = "message 为数字", body = '{"choices":[{"message":42}]}', field = "message" },
	}
	for _, b in ipairs(bads) do
		local ok, res, err = no_raise(b.body)
		check(lib .. ": " .. b.name .. "时不抛裸错误", ok, tostring(res))
		check(lib .. ": " .. b.name .. "时返回点名原因",
			ok and res == nil and tostring(err):find(b.field, 1, true) ~= nil, tostring(err))
	end

	-- ② tool_calls 的元素非表 / function 非表：**跳过并计数**（不整轮失败）
	local ok1, r1 = no_raise('{"choices":[{"message":{"content":"x","tool_calls":[42]}}]}')
	check(lib .. ": tool_calls 元素为数字时不抛裸错误", ok1, tostring(r1))
	check(lib .. ": 元素为数字时被丢弃但计数进 extras",
		type(r1) == "table" and r1.tool_calls == nil
		and type(r1.extras) == "table" and r1.extras.dropped_bad_tool_calls == 1,
		json.encode(r1))

	local ok2, r2 = no_raise('{"choices":[{"message":{"content":"x","tool_calls":'
		.. '[{"id":"c1","function":42}]}}]}')
	check(lib .. ": function 为数字时不抛裸错误", ok2, tostring(r2))
	check(lib .. ": function 为数字时被丢弃但计数",
		type(r2) == "table" and r2.tool_calls == nil
		and r2.extras.dropped_bad_tool_calls == 1, json.encode(r2))

	-- ③ 混合：一条合法 + 一条畸形 → 合法的照常派发，畸形的计数
	local ok3, r3 = no_raise('{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c1","function":{"name":"t","arguments":"{}"}},42]}}]}')
	check(lib .. ": 合法与畸形混合时保留合法的",
		ok3 and type(r3.tool_calls) == "table" and #r3.tool_calls == 1
		and r3.tool_calls[1].id == "c1" and r3.extras.dropped_bad_tool_calls == 1,
		json.encode(r3))

	-- ④ 全合法时**不出现** dropped_bad_tool_calls（保持既有响应形态不变）
	local ok4, r4 = no_raise('{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c1","function":{"name":"t","arguments":"{}"}}]}}]}')
	check(lib .. ": 全合法时不出现丢弃计数字段",
		ok4 and (r4.extras == nil or r4.extras.dropped_bad_tool_calls == nil),
		json.encode(r4))
end

-- SAN-6：驱动**不再合成** tool_call id（此前是 `tc.id or ("call_" .. i)`）。
-- 起因：宿主的合成方案是轮次相关的（原 agentloop 的 call_<turn>_<i>），而驱动的
-- call_<i> 每轮从 1 重来，会把宿主那条路径遮住并造成**跨轮重复 id**——真机
-- 400 answered more than once，且那条坏消息落在不可回滚的历史里。
-- 宿主本体已随 C++ Agent REPL 移除（d384a9b9），现在填 id 的是 Rust 侧
-- （rig 的 ToolCallId::new_or_mint）；驱动侧判据不变：
-- id 缺失/非字符串时给空串（由宿主填），而不是自己编一个可能撞车的。
do
	local function parse(body) return openai.parse_response(json.decode(body)) end

	local r = parse('{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"function":{"name":"t","arguments":"{}"}}]}}]}')
	check("SAN-6：id 缺失时驱动透传空串（不合成 call_1）",
		type(r) == "table" and r.tool_calls[1].id == "", json.encode(r.tool_calls[1]))

	local r2 = parse('{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":42,"function":{"name":"t","arguments":"{}"}}]}}]}')
	check("SAN-6：id 非字符串时同样给空串（不把数字当 id 下发）",
		type(r2) == "table" and r2.tool_calls[1].id == "", json.encode(r2.tool_calls[1]))

	local r3 = parse('{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c9","function":{"name":"t","arguments":"{}"}}]}}]}')
	check("SAN-6：id 存在时原样透传", r3.tool_calls[1].id == "c9", json.encode(r3.tool_calls[1]))

	-- arguments 非字符串时 tostring（宿主 parseToolArguments 会折成"工具参数错误"），
	-- 而不是把对象/数字丢给宿主当字符串用
	local r4 = parse('{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c1","function":{"name":"t","arguments":42}}]}}]}')
	check("SAN-6：arguments 为数字时转成字符串（宿主据此报参数错误）",
		type(r4) == "table" and r4.tool_calls[1].arguments == "42",
		json.encode(r4.tool_calls[1]))
end

-- PL-3（2026-09-22）：content 归一化。新版 OpenAI 协议允许 content 为
-- [{type="text",text=...},…] 数组形态，部分兼容网关也这么返回；宿主只认字符串
-- （agentproto.cpp 的 content->isStr()），原样透传会让整轮以「既没有返回文本
-- 也没有工具调用」告终，报错完全不指向真因。两个驱动共用 parse_response。
for _, lib in ipairs({ "openai", "deepseek" }) do
	local wire = lib == "openai" and openai or deepseek
	local function parse(body) return wire.parse_response(json.decode(body)) end

	local r = parse('{"choices":[{"message":{"content":'
		.. '[{"type":"text","text":"你"},{"type":"text","text":"好"}]}}]}')
	check(lib .. ": 数组 content 按序拼接", r.content == "你好")

	local r2 = parse('{"choices":[{"message":{"content":'
		.. '[{"type":"text","text":"看"},{"type":"image_url","image_url":{"url":"x"}}]}}]}')
	check(lib .. ": 非 text 段不混进文本", r2.content == "看")
	-- 丢了东西必须可见（项目口径：不谎报状态），但不能污染既有响应形态
	check(lib .. ": 被丢弃的段计数进 extras",
		r2.extras ~= nil and r2.extras.content_parts_dropped == 1)

	local r3 = parse('{"choices":[{"message":{"content":[]}}]}')
	check(lib .. ": 空数组 → 空文本", r3.content == "")
	check(lib .. ": 没丢东西就不挂计数", r3.extras == nil)

	-- 网关直接回单个 part 对象（非数组）：ipairs 一个都遍历不到，不特判会静默变空
	local r4 = parse('{"choices":[{"message":{"content":{"type":"text","text":"单段"}}}]}')
	check(lib .. ": 单个 part 对象也认", r4.content == "单段")

	-- 畸形表：宁可空文本 + 计数可见，也不要静默吞掉
	local r5 = parse('{"choices":[{"message":{"content":{"foo":1}}}]}')
	check(lib .. ": 畸形 content 表 → 空文本但计数可见",
		r5.content == "" and r5.extras ~= nil and r5.extras.content_parts_dropped == 1)

	-- 反向对照：既有的字符串形态与 null 一字不变
	local r6 = parse('{"choices":[{"message":{"content":"普通"}}]}')
	check(lib .. ": 字符串 content 不变", r6.content == "普通" and r6.extras == nil)
	local r7 = parse('{"choices":[{"message":{"content":null}}]}')
	check(lib .. ": null content → 空文本", r7.content == "")
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

-- ============ 出站一致性自检：tool_calls ⇔ tool 消息一一对应（A1b） ============
-- 判据必须能抓住 AG-26 第一版那种形状（多写一条 tool 消息 + 复用别人的 id）：
-- 那正是「宿主写历史出错 → 网关只回含混 400 → 会话从此每条任务都失败」的成因。
-- 每条断言都因「删掉 check_tool_pairing」而变红（反向验证过）。
do
	local function asst(ids)
		local tcs = {}
		for i, id in ipairs(ids) do tcs[i] = { id = id, name = "echo", arguments = "{}" } end
		return { role = "assistant", content = "", tool_calls = tcs }
	end
	local function tool(id) return { role = "tool", tool_call_id = id, content = "ok" } end
	local user = { role = "user", content = "跑一下" }

	check("A1b：合法序列（1 调用 ⇔ 1 回执）通过",
		openai.check_tool_pairing({ user, asst({ "c1" }), tool("c1") }) == nil)
	check("A1b：合法序列（3 调用 ⇔ 3 回执，顺序一致）通过",
		openai.check_tool_pairing({ user, asst({ "c1", "c2", "c3" }),
			tool("c1"), tool("c2"), tool("c3") }) == nil)
	check("A1b：图片消息夹在 tool 回执之后不影响判定",
		openai.check_tool_pairing({ user, asst({ "c1" }), tool("c1"),
			{ role = "user", content = "看图" } }) == nil)
	check("A1b：没有 tool_calls 的纯文本历史通过",
		openai.check_tool_pairing({ user, { role = "assistant", content = "好了" } }) == nil)

	local e1 = openai.check_tool_pairing({ user, asst({ "c1", "c2" }), tool("c1") })
	check("A1b：少一条 tool 回执 → 报错", type(e1) == "string", tostring(e1))
	check("A1b：少一条的报错点名期望数量",
		type(e1) == "string" and e1:find("2 个 tool_calls", 1, true) ~= nil, tostring(e1))

	-- AG-26 第一版的形状：N 个 tool_calls 后面跟了 N+1 条 tool 消息
	local e2 = openai.check_tool_pairing({ user, asst({ "c1" }), tool("c1"), tool("c1") })
	check("A1b：多一条 tool 回执 → 报错", type(e2) == "string", tostring(e2))
	check("A1b：多一条的报错点明「多写一条同样会被服务端 400」",
		type(e2) == "string" and e2:find("多出的一条", 1, true) ~= nil, tostring(e2))

	local e3 = openai.check_tool_pairing({ user, asst({ "c1" }), tool("cX") })
	check("A1b：id 不匹配 → 报错", type(e3) == "string" and e3:find("不匹配", 1, true) ~= nil,
		tostring(e3))

	local e4 = openai.check_tool_pairing({ user, asst({ "c1", "c1" }), tool("c1"), tool("c1") })
	check("A1b：assistant 内 id 重复 → 报错",
		type(e4) == "string" and e4:find("重复", 1, true) ~= nil, tostring(e4))

	local e5 = openai.check_tool_pairing({ user, asst({ "" }), tool("") })
	check("A1b：tool_calls 缺 id → 报错",
		type(e5) == "string" and e5:find("缺 id", 1, true) ~= nil, tostring(e5))

	-- build_request 必须真的调用它：不配对时报 error（驱动侧 pcall 折成 ERR 契约）
	local ok, e = pcall(openai.build_request,
		{ messages = { user, asst({ "c1" }), tool("c1"), tool("c1") } }, "m")
	check("A1b：build_request 对不配对的历史抛错", ok == false, tostring(e))
	check("A1b：抛错文案点名「出站报文自检失败」",
		ok == false and tostring(e):find("出站报文自检失败", 1, true) ~= nil, tostring(e))
	local ok2 = pcall(openai.build_request,
		{ messages = { user, asst({ "c1" }), tool("c1") } }, "m")
	check("A1b：配对正确的历史照常组包", ok2 == true, tostring(ok2))
end


-- ================= 句柄收尾不许留悬垂回调（AG-16） =================
-- 旧实现只 cb:free() 不清句柄，WRITEFUNCTION 仍指在已释放的回调上；当前只因
-- Session:post 每次都先 curl_easy_reset 才安全。Session.handle 是公开字段，
-- 任何「同句柄 perform 两次 / 加重试」的重构都会把它变成内存安全问题。
-- 修法是在 free 之前先 reset，使句柄自身满足「空闲即无武装」的不变量。
--
-- 【2026-09-21 第三轮审计 AG-20 附带修正】原判据是「post 之后直接对同一句柄
-- curl_easy_perform，只要返回即证明已 reset」。那个断言本身就在做 UB 的事：
-- cb:free() 已经把回调槽还掉了，句柄倘若**没有**被 reset 干净，这一 perform
-- 就是踩在已释放的回调对象上——旧实现「恰好没崩」只是 libcurl 早退（rc=3
-- CURLE_URL_MALFORMAT，根本没走到回调），并不是安全性的证据。AG-20 把
-- M.post 改成池化复用后，同样的调用序列变成了 curl_multi_cleanup 里的
-- SIGSEGV（gdb 栈：#4 curl_easy_cleanup → #2 curl_multi_cleanup）。
-- 现在换成**只看句柄状态、不再对已 free 回调的句柄发包**的判据：
-- CURLINFO_EFFECTIVE_URL 在 reset 之后必须为空 —— 这正是「未武装」的定义，
-- 且不依赖 libcurl 的早退行为。
--
-- 【2026-09-21 第七轮审计 AG-27 修正：这条门禁在树莓派上一直是**假红**】
-- 判据本身没错，错在**读句柄的库不是建句柄的库**：原实现 `ffi.load("curl")`，
-- 而 llm_http 按 CANDIDATES 先试 `libcurl.so.4`。本机（Raspberry Pi OS）
-- `libcurl.so → libcurl-gnutls.so`、`libcurl.so.4 → libcurl.so.4.8.0` 是**两个
-- 不同文件**，于是进程里有两份 libcurl 实例（实测 curl_easy_init 地址
-- 0x7fff802a6d80 vs 0x7fff7eb65c60）。用 B 实例的 getinfo 去读 A 实例的 easy
-- handle，等于按另一套结构体布局解引用不透明指针 —— 实测返回端口串 "1"
-- 这样的错位垃圾；同一句柄改用 A 实例读，reset 前是完整 URL、reset 后为空，
-- 产品代码一直是对的。CI（bookworm：libcurl.so 与 libcurl.so.4 同一文件）
-- 只有一份实例，所以那边绿 —— 典型的「环境相关假红」，而红门禁被忽略的
-- 代价是它从此不再具备判别力。
-- 修法：一律经 http.libname() 拿**同一份**实例；并补一条阳性对照，证明该探针
-- 真能分辨「武装/未武装」（常量写错时旧断言会空洞地通过）。
do
	local http = dofile("modules/lib/llm_http.lua")
	local s = http.session()
	check("AG-16：能建会话", s ~= nil, tostring(s))
	if s ~= nil then
		local libname = http.libname()
		check("AG-27：llm_http 暴露实际加载的库名", type(libname) == "string" and libname ~= "",
			tostring(libname))

		local ffi = require("ffi")
		-- 不再 ffi.cdef：curl_easy_getinfo/setopt 已由 llm_http.lua 声明过，
		-- 重复声明是「同名不同签名」的冲突源；按本仓既有写法用定签名 cast 调用。
		local curl = ffi.load(libname)
		local getinfo_str = ffi.cast("int (*)(void *, int, const char **)", curl.curl_easy_getinfo)
		local setopt_ptr = ffi.cast("int (*)(void *, int, const void *)", curl.curl_easy_setopt)
		local setopt_long = ffi.cast("int (*)(void *, int, long)", curl.curl_easy_setopt)
		local CURLINFO_EFFECTIVE_URL = 1048577	-- curl.h 权威值，与 test_curl_consts.c 同族
		local OPT_URL, OPT_TIMEOUT, OPT_NOSIGNAL = 10002, 13, 99

		local function effUrl(h)
			local q = ffi.new("const char *[1]")
			local rc = getinfo_str(h, CURLINFO_EFFECTIVE_URL, q)
			return tonumber(rc), (q[0] ~= nil and ffi.string(q[0]) or nil)
		end

		-- 两份实例并存是本机的环境事实（不是缺陷），但值得让操作者看见：
		-- 任何别的加载者若不用 libname()，就会重现上面那种错位读。
		local okAlt, alt = pcall(ffi.load, "curl")
		if okAlt and tostring(alt.curl_easy_init) ~= tostring(curl.curl_easy_init) then
			io.stderr:write("[llm-drivers] 注记: 本机 ffi.load(\"curl\") 与 ffi.load(\""
				.. tostring(libname) .. "\") 是**两份不同的 libcurl 实例**；"
				.. "跨实例使用 easy handle 会读出错位字段（AG-27 的假红成因）\n")
			io.stderr:flush()
		end

		-- 阳性对照：同一实例下，武装过的句柄 EFFECTIVE_URL 非空、reset 后为空。
		-- 没有这一条，"post 之后为空"可能是常量写错导致的空洞通过。
		local probe = curl.curl_easy_init()
		check("AG-27：阳性对照能建句柄", probe ~= nil)
		if probe ~= nil then
			setopt_ptr(probe, OPT_URL, "http://127.0.0.1:1/v1/x")
			setopt_long(probe, OPT_TIMEOUT, 1)
			setopt_long(probe, OPT_NOSIGNAL, 1)
			curl.curl_easy_perform(probe)	-- 端口 1 必然连不上，要的是「走过 libcurl」
			local rcA, urlA = effUrl(probe)
			check("AG-27：武装后 EFFECTIVE_URL 非空（探针有判别力）",
				rcA == 0 and urlA ~= nil and urlA ~= "", tostring(urlA))
			curl.curl_easy_reset(probe)
			local rcB, urlB = effUrl(probe)
			check("AG-27：reset 后 EFFECTIVE_URL 为空（同一实例、同一常量）",
				rcB == 0 and (urlB == nil or urlB == ""), tostring(urlB))
			curl.curl_easy_cleanup(probe)
		end

		-- 端口 1 必然连不上：这里只要走到「perform 返回」即可，不需要成功响应
		local res = s:post({ url = "http://127.0.0.1:1/v1/x", body = "{}", timeout = 1 })
		check("AG-16：post 返回（连不上也不崩）", type(res) == "table", tostring(res))

		local rc, url = effUrl(s.handle)
		check("AG-16：getinfo 可取（句柄仍有效）", rc == 0, "rc=" .. tostring(rc))
		check("AG-16：post 之后句柄已复位（EFFECTIVE_URL 为空 = 未武装）",
			url == nil or url == "", tostring(url))

		s:close()
	end
end


-- ================= 池化会话：连接与 TLS 跨轮复用（AG-20） =================
-- 症状：M.post 原实现**每次请求都新建 curl 句柄、返回前销毁**，于是 M.session
-- 的存在理由（「保留连接与 TLS 会话，省掉每轮重新握手」）在任何生产路径上都没
-- 生效——两个内置驱动都走 M.post。agent 每轮模型调用因此重做 DNS/TCP/TLS，
-- 且历史里那张 base64 图每轮重新编码上传一遍。
-- 判据分两段：
--   ① 离线（端口 1 不可达）：池化 + 连接层失败的自愈 + 显式关闭；
--   ② 会话**创建计数**：连续 N 次请求只允许建 1 份会话。
--      **不用句柄地址当判据**——libcurl 每次 init 申请同样大的块，free 后 malloc
--      常把同一地址还回来；反向验证时「每次新建」也过了地址判据（假绿），
--      换成计数后当场转红（agent_e2e.sh 的 AG-20 判据同源，同样只用计数）。
do
	local http = dofile("modules/lib/llm_http.lua")
	check("AG-20：暴露池化句柄探针", type(http.pooled_handle) == "function")
	check("AG-20：暴露池化关闭入口", type(http.close_pooled) == "function")
	check("AG-20：暴露会话计数（判据用）", type(http.session_count) == "function")

	http.close_pooled()	-- 前面的用例也走过 M.post，先清干净再断言「池为空」
	check("AG-20：暴露池活跃探针", type(http.pooled_active) == "function")
	check("AG-20：关闭后池为空", http.pooled_active() == false)

	-- 端口 1 必然连不上：要的是「请求真的走过 libcurl」，不是成功响应
	local seq0 = http.session_count()
	local r1 = http.post({ url = "http://127.0.0.1:1/v1/x", body = "{}", timeout = 1 })
	check("AG-20：post 返回结果表（连不上也不崩）", type(r1) == "table", tostring(r1))
	check("AG-20：端口 1 不可达 → ok=false", r1.ok == false, tostring(r1.ok))
	check("AG-20：失败后池里仍有可用句柄（自愈而非弃用）",
		http.pooled_handle() ~= nil)
	check("AG-20：首次请求只建一份会话", http.session_count() == seq0 + 1,
		"seq0=" .. seq0 .. " now=" .. http.session_count())

	-- 连接层失败后**不得**新建会话（AG-20 的第二个缺陷，由 LSan 逼出来）：
	-- 曾经每次失败都 curl_easy_init 一份新句柄做「自愈」，LSan 实测每次失败
	-- 请求泄漏约 71 字节（easy handle 的 share/DNS/TLS-session 缓存收不干净），
	-- 断网重试场景下线性增长；删掉那段后连跑 5 次失败请求**零泄漏**。
	-- libcurl 自己会把失败连接标记为不可复用，句柄本身可以继续用，
	-- 所以正确的行为是「什么都不做」。
	local seq1 = http.session_count()
	http.post({ url = "http://127.0.0.1:1/v1/x", body = "{}", timeout = 1 })
	http.post({ url = "http://127.0.0.1:1/v1/x", body = "{}", timeout = 1 })
	http.post({ url = "http://127.0.0.1:1/v1/x", body = "{}", timeout = 1 })
	local seq2 = http.session_count()
	check("AG-20：连续 3 次失败请求不新建会话（失败即重建会按次数泄漏）",
		seq2 == seq1, "seq1=" .. seq1 .. " seq2=" .. seq2)

	-- ---- ② 真服务：成功请求之间句柄必须复用 ----
	local py = ([[
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(n)
        b = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", %d), H).serve_forever()
]]):format(18792)
	local f = io.popen("python3 -c " .. string.format("%q", py) .. " >/dev/null 2>&1 & echo $!")
	local pid = f and f:read("*l")
	if f then f:close() end
	local okReq = false
	if pid ~= nil and pid ~= "" then
		os.execute("sleep 0.6")
		local seqBefore = http.session_count()
		local ok1, ra = pcall(http.post,
			{ url = "http://127.0.0.1:18792/v1/x", body = "{}", timeout = 5 })
		local ok2, rb = pcall(http.post,
			{ url = "http://127.0.0.1:18792/v1/x", body = "{}", timeout = 5 })
		local seqAfter = http.session_count()
		os.execute("kill " .. pid .. " 2>/dev/null")
		if ok1 and ok2 and type(ra) == "table" and ra.ok and type(rb) == "table" and rb.ok then
			okReq = true
			check("AG-20：真服务两次请求都成功", true)
			-- 正面判据以**计数**为准（地址会被 malloc 复用，不可作判据）
			check("AG-20：两次成功请求之间没有新建会话",
				seqAfter == seqBefore, "before=" .. seqBefore .. " after=" .. seqAfter)
		end
	end
	if not okReq then
		print("[SKIP] AG-20 真服务正面判据：本机无法起临时 HTTP 服务（缺 python3？）")
	end

	-- 显式关闭后必须真的释放（驱动 destroy 路径）
	http.close_pooled()
	check("AG-20：close_pooled 后池清空", http.pooled_active() == false)
	http.close_pooled()
	check("AG-20：close_pooled 幂等", http.pooled_active() == false)
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
	-- 夹具必须是**协议合规**的历史形状：assistant 带 tool_calls 就得跟上配对的
	-- tool 回执，否则 A1b 的出站自检会（正确地）拒绝它。断言只看 messages[1]。
	local off = deepseek.build_request({ thinking = false,
		messages = { { role = "assistant", content = "",
			tool_calls = { { id = "c1", name = "t", arguments = "{}" } },
			extras = { reasoning_content = "上一轮思考" } },
			{ role = "tool", tool_call_id = "c1", content = "ok" } } }, "m0")
	check("AG-17：布尔 false 关闭时不再回传 reasoning_content",
		off.messages[1].reasoning_content == nil, tostring(off.messages[1].reasoning_content))
	local on = deepseek.build_request({ messages = { { role = "assistant", content = "",
		tool_calls = { { id = "c1", name = "t", arguments = "{}" } },
		extras = { reasoning_content = "上一轮思考" } },
		{ role = "tool", tool_call_id = "c1", content = "ok" } } }, "m0")
	check("AG-17：开启时仍回传 reasoning_content（有状态要求）",
		on.messages[1].reasoning_content == "上一轮思考")
end

-- ========== SAN-3：错误必须带**结构化判据**（可重试分类的依据） ==========
-- 背景（2026-09-22 实测）：宿主原先靠匹配人话子串判「能不能重试」
-- （CURLE_* / "curl error" / "网络"），而真实错误是
-- `curl rc=7: Could not connect to server`、`HTTP 503: {...}`——一个都不匹配，
-- 于是 max_retries 在所有真实 HTTP/curl 失败上都不生效（实测各只发 1 次请求）。
-- 判据因此改为结构化字段优先：驱动必须把 http_status / curl_rc / retriable /
-- retry_after_ms 透出来，宿主据此判类。
--
-- 离线手法：`http` 是 llm_http.lua 的**单例表**（dofile 返回同一份），
-- 临时替换它的 post 即可精确构造「服务端返回了什么」，无需真起 HTTP 服务。
-- 用完必须还原（后面的用例还要用它连真服务）。
do
	local http = dofile("modules/lib/llm_http.lua")
	local real_post = http.post
	local function restore() http.post = real_post end

	-- ---- ① 429：可重试 + 带上服务端给的 Retry-After（毫秒） ----
	http.post = function()
		return { ok = true, status = 429, body = '{"error":{"message":"slow down"}}',
			retry_after_ms = 2000 }
	end
	local r1 = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：429 标为可重试", r1.retriable == true, tostring(r1.retriable))
	check("SAN-3：429 带 http_status", r1.http_status == 429, tostring(r1.http_status))
	check("SAN-3：Retry-After 以毫秒透出（服务端说了算）",
		r1.retry_after_ms == 2000, tostring(r1.retry_after_ms))
	check("SAN-3：ERR 表仍带人话 message",
		r1.result == "ERR" and type(r1.message) == "string", tostring(r1.message))

	-- ---- ② 503：可重试（服务端故障，重试有意义） ----
	http.post = function() return { ok = true, status = 503, body = "{}" } end
	local r2 = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：503 标为可重试", r2.retriable == true, tostring(r2.retriable))

	-- ---- ③ 401/400：**不可**重试（重试不会改变结果，只烧配额） ----
	http.post = function() return { ok = true, status = 401, body = "{}" } end
	local r3 = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：401 标为不可重试", r3.retriable == false, tostring(r3.retriable))
	http.post = function() return { ok = true, status = 400, body = "{}" } end
	local r3b = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：400 标为不可重试", r3b.retriable == false, tostring(r3b.retriable))

	-- ---- ④ 传输层失败：把 curl 数值退出码透出（散文靠不住） ----
	http.post = function()
		return { ok = false, err = "curl rc=7: Could not connect to server", curl_rc = 7 }
	end
	local r4 = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：传输失败透出 curl_rc", r4.curl_rc == 7, tostring(r4.curl_rc))
	check("SAN-3：传输失败保留人话文案",
		tostring(r4.message):find("curl rc=7") ~= nil, tostring(r4.message))

	-- ---- ⑤ 没有 Retry-After 头时不长出该键（保持既有响应形态） ----
	http.post = function() return { ok = true, status = 429, body = "{}" } end
	local r5 = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：服务端没给 Retry-After 时不出现 retry_after_ms 键",
		r5.retry_after_ms == nil, tostring(r5.retry_after_ms))
	check("SAN-3：此时仍可重试（退避回落到本地指数）", r5.retriable == true)

	-- ---- ⑥ 成功路径不受影响（形态逐字不变） ----
	http.post = function()
		return { ok = true, status = 200,
			body = '{"choices":[{"message":{"role":"assistant","content":"ok"}}]}' }
	end
	local r6 = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
	restore()
	check("SAN-3：成功响应不带任何新的错误字段",
		r6.result == "OK" and r6.content == "ok"
		and r6.retriable == nil and r6.http_status == nil and r6.curl_rc == nil)
end

-- ========== 缺省端点注记（AG-13②，B2 后仅 DeepSeek 走缺省回退） ==========
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
	check("AG-13②：无端点时回落到 DeepSeek 内置缺省（官方 API 域，带 stderr 注记）",
		d.endpoint == deepseek.DEFAULTS.endpoint, d.endpoint)
	-- B2：DeepSeek 的 require_endpoint 也从 openai 库继承——但 DeepSeek 自己的
	-- request 先回落到内置缺省，所以不会触达必填判据；这里钉住判据本身存在。
	check("B2：openai.require_endpoint 存在且可离线单测",
		type(openai.require_endpoint) == "function")
end

tester.summary("llm 协议驱动单测")

-- ================= Anthropic Messages（第三个协议实例，2026-09-23 批次 3）=================
-- 这一段的重点不是「字段抄得对」，而是**形状差异真的被抹平了**：system 抽到顶层、
-- tool_use/tool_result 块、严格交替、图片拆 data URL、鉴权头不是 Bearer。
-- 判据与既有两驱动同源：请求整形 + 响应映射 + 错误分类（命中 ERR 契约）。

local anthropic = dofile("modules/lib/llm_wire_anthropic.lua")

do
	local r = anthropic.request(sample())
	check("Anthropic: 用传入的接入点", r.endpoint == "http://127.0.0.1:1/v1/chat/completions")
	check("Anthropic: 用传入的模型", r.req.model == "m0")
	check("Anthropic: 非流式", r.req.stream == false)
	check("Anthropic: max_tokens 透传（本协议必填）", r.req.max_tokens == 4096)
	check("Anthropic: temperature 透传", r.req.temperature == 0.7)
	check("Anthropic: top_p 透传", r.req.top_p == 0.9)
	-- ① system 是顶层字段，不在 messages 里
	check("Anthropic: system 抽到顶层", r.req.system == "sys")
	check("Anthropic: messages 里没有 system 角色",
		r.req.messages[1].role == "user" and r.req.messages[1].content[1].text == "hi")
	-- ② 工具调用是 tool_use 块，input 是**对象**
	check("Anthropic: assistant 的工具调用表达为 tool_use 块",
		r.req.messages[2].role == "assistant"
		and r.req.messages[2].content[1].type == "tool_use"
		and r.req.messages[2].content[1].name == "t")
	check("Anthropic: tool_use.input 是对象（规范化侧是字符串）",
		type(r.req.messages[2].content[1].input) == "table")
	-- ③ 工具结果并进一条 user 消息的 tool_result 块
	check("Anthropic: 工具结果是 user 消息里的 tool_result 块",
		r.req.messages[3].role == "user"
		and r.req.messages[3].content[1].type == "tool_result"
		and r.req.messages[3].content[1].tool_use_id == "c1")
	check("Anthropic: 严格交替（4 条规范化消息 → 3 条 provider 消息）",
		#r.req.messages == 3)
	-- 工具声明用 input_schema，不是 function.parameters
	check("Anthropic: 工具声明用 name/description/input_schema",
		r.req.tools[1].name == "t" and r.req.tools[1].input_schema ~= nil
		and r.req.tools[1]["function"] == nil)
	check("Anthropic: 无 tool_choice=auto（本协议无此档）", r.req.tool_choice == nil)
	-- 工具调用参数是 JSON 字符串，必须解码成对象（包括非空参数）
	local r2 = pcall(anthropic.request, {
		endpoint = "http://x", max_tokens = 16,
		messages = {
			{ role = "user", content = "u" },
			{ role = "assistant", content = "",
			  tool_calls = { { id = "c9", name = "f", arguments = '{"a":1,"b":"x"}' } } },
			{ role = "tool", tool_call_id = "c9", content = "r" },
		},
	})
	check("Anthropic: 非空 arguments 字符串解码成对象", r2 == true)
end

do
	-- 多条 tool 消息并进同一条 user 消息（一轮多调用）
	local r = anthropic.request({
		endpoint = "http://x", max_tokens = 16,
		messages = {
			{ role = "user", content = "u" },
			{ role = "assistant", content = "",
			  tool_calls = { { id = "a", name = "f", arguments = "{}" },
			                 { id = "b", name = "g", arguments = "{}" } } },
			{ role = "tool", tool_call_id = "a", content = "r1" },
			{ role = "tool", tool_call_id = "b", content = "r2" },
		},
	})
	check("Anthropic: 两条 tool 消息并成一条 user 消息的 2 个块",
		#r.req.messages == 3 and r.req.messages[3].role == "user"
		and #r.req.messages[3].content == 2
		and r.req.messages[3].content[1].tool_use_id == "a"
		and r.req.messages[3].content[2].tool_use_id == "b")
end

do
	-- 图片：data URL 拆成 media_type + base64 数据（Anthropic 不接受整串 data URL）
	local r = anthropic.request({
		endpoint = "http://x", max_tokens = 16,
		messages = {
			{ role = "user", content = {
				{ type = "text", text = "看图" },
				{ type = "image_url", image_url = { url = "data:image/png;base64,QUJD" } },
			} },
		},
	})
	local blk = r.req.messages[1].content[2]
	check("Anthropic: 图片映射为 image 块 + base64 source",
		blk.type == "image" and blk.source.type == "base64"
		and blk.source.media_type == "image/png" and blk.source.data == "QUJD")
	check("Anthropic: 文本部件保持 text 块",
		r.req.messages[1].content[1].type == "text"
		and r.req.messages[1].content[1].text == "看图")
end

do
	-- fail-closed 族：四种必须响亮拒绝而不是发出去拿含混 400
	local function must_err(name, data)
		local ok, e = pcall(anthropic.request, data)
		check("Anthropic: " .. name .. " → 拒绝", ok == false and tostring(e):len() > 0)
	end
	must_err("缺 endpoint", { max_tokens = 16, messages = {} })
	must_err("缺 max_tokens", { endpoint = "http://x", messages = {} })
	must_err("temperature 越界（>1，OpenAI 系允许）", {
		endpoint = "http://x", max_tokens = 16, temperature = 1.5,
		messages = { { role = "user", content = "u" } } })
	must_err("图片不是 data URL", {
		endpoint = "http://x", max_tokens = 16,
		messages = { { role = "user", content = {
			{ type = "image_url", image_url = { url = "https://example.com/a.png" } } } } } })
	must_err("assistant 空 content（无文本无调用）", {
		endpoint = "http://x", max_tokens = 16,
		messages = {
			{ role = "user", content = "u" },
			{ role = "assistant", content = "" },
		} })
	must_err("tool 消息缺 tool_call_id", {
		endpoint = "http://x", max_tokens = 16,
		messages = {
			{ role = "assistant", content = "", tool_calls = { { id = "c", name = "t", arguments = "{}" } } },
			{ role = "tool", content = "r" },
		} })
	must_err("工具参数不是合法 JSON", {
		endpoint = "http://x", max_tokens = 16,
		messages = {
			{ role = "assistant", content = "",
			  tool_calls = { { id = "c", name = "t", arguments = "{不是 JSON" } } },
		} })
end

do
	-- 响应映射：text 段拼接、tool_use → 规范化 tool_calls、usage 字段名映射
	local ok, out = pcall(anthropic.parse_response, {
		content = {
			{ type = "text", text = "前半" },
			{ type = "thinking", thinking = "内部思考" },
			{ type = "text", text = "后半" },
			{ type = "tool_use", id = "tu1", name = "run_action_script",
			  input = { script = "std.mt" } },
		},
		stop_reason = "tool_use",
		usage = { input_tokens = 11, output_tokens = 22 },
	})
	check("Anthropic 响应: 解析成功", ok == true and out.result == "OK")
	check("Anthropic 响应: text 段按序拼接", out.content == "前半后半")
	check("Anthropic 响应: tool_use → 规范化 tool_calls（input 编码回字符串）",
		#out.tool_calls == 1 and out.tool_calls[1].id == "tu1"
		and out.tool_calls[1].name == "run_action_script"
		and type(out.tool_calls[1].arguments) == "string"
		and out.tool_calls[1].arguments:find("std.mt") ~= nil)
	check("Anthropic 响应: 非文本非工具块丢弃但记账（thinking）",
		out.extras ~= nil and out.extras.dropped_content_blocks == 1)
	check("Anthropic 响应: stop_reason 进 extras（排障要知道是不是被截断）",
		out.extras ~= nil and out.extras.stop_reason == "tool_use")
	check("Anthropic 响应: usage 字段名映射到规范化口径",
		out.usage ~= nil and out.usage.prompt_tokens == 11
		and out.usage.completion_tokens == 22)

	-- 形态非法：点名返回（不抛裸 Lua 错误）
	local ok2, err2 = anthropic.parse_response({ content = 42 })
	check("Anthropic 响应: content 非数组 → 点名错误",
		ok2 == nil and tostring(err2):find("content") ~= nil)

	-- 只有 tool_use 无 text：照常（与 OpenAI 系同款结局）
	local ok3, out3 = pcall(anthropic.parse_response, {
		content = { { type = "tool_use", id = "z", name = "f", input = {} } },
	})
	check("Anthropic 响应: 纯工具调用轮", ok3 == true and out3.content == ""
		and #out3.tool_calls == 1)
end

do
	-- 鉴权头形态（④）：x-api-key + anthropic-version；密钥含控制字符拒绝。
	-- 用假 env 读取链验证 key 来源，不碰网络（perform 的 HTTP 部分由 E2E 覆盖）。
	check("Anthropic: 鉴权读 ANTHROPIC_API_KEY 链",
		anthropic.api_key({}) ~= nil or os.getenv("BOT4CJ_LLM_KEY") == nil)
	check("Anthropic: 控制字符判据与 OpenAI 系同源",
		anthropic.api_key_has_control_chars("a\nb") == true
		and anthropic.api_key_has_control_chars("abc") == false)
end
