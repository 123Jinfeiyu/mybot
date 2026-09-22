-- test_llm_wire_contract.lua —— 协议驱动响应契约的**形变矩阵**
--
-- 用法（项目根目录）: luajit test/test_llm_wire_contract.lua
--
-- 为什么单独一份（而不是并进 test_llm_drivers.lua）：那一份按**功能**组织
-- （请求整形 / 响应映射 / 超时钳制 / 池化会话…），这一份按**形变**组织——
-- 一张「provider 报文畸形成什么样 → 驱动应当怎么办」的表。历史缺口正是
-- "线上撞到一个补一个"：PL-3（数组形态 content）与 PL-6（tool_calls 非表）
-- 都是这么来的，补完各自加了用例，但**没有矩阵**，所以下一个形变还要等线上撞。
--
-- 每行标注契约要求，三类结局：
--   named_err —— 返回 nil + **点名**原因（不抛裸 Lua 错误）
--   degrade   —— 静默降级但**如实记账**（丢弃的东西要能看见）
--   ok        —— 照常处理，形态与既有响应一致
--
-- 完全离线：只喂字符串给 parse_response，不碰网络与硬件。

local tester = dofile("test/lib/tester.lua")
local check = tester.check

local json = dofile("modules/lib/json.lua")

-- 诊断用的安全序列化：json.encode(nil) 会抛（不是合法的 JSON 值），而矩阵里
-- 大量断言要打印"实际是什么"——其中很多情况下它就是 nil。
local function J(v)
	if v == nil then return "nil" end
	local ok, s = pcall(json.encode, v)
	if not ok then return "<不可序列化: " .. type(v) .. ">" end
	return s
end
local openai = dofile("modules/lib/llm_wire_openai.lua")
local deepseek = dofile("modules/lib/llm_wire_deepseek.lua")

-- 两个内置驱动复用同一份 parse_response（llm_wire_deepseek 直接指向它），
-- 所以矩阵在两个驱动上都跑一遍——这样"某天有人给 DeepSeek 换了自己的实现"
-- 会被立刻发现。
local DRIVERS = { { "openai", openai }, { "deepseek", deepseek } }

-- 一行形变：body（provider 报文）+ expect（结局）+ 说明
-- expect.kind: "named_err" | "degrade" | "ok"
local MATRIX = {
	-- ---------- choices / message 层 ----------
	{ name = "choices 缺失", body = '{}', kind = "named_err", field = "choices" },
	{ name = "choices 为空数组", body = '{"choices":[]}', kind = "named_err",
	  field = "choices[1].message" },
	{ name = "choices 是数字", body = '{"choices":42}', kind = "named_err", field = "choices" },
	{ name = "choices[1] 是数字", body = '{"choices":[42]}', kind = "named_err",
	  field = "choices[1]" },
	{ name = "message 缺失", body = '{"choices":[{}]}', kind = "named_err",
	  field = "choices[1].message" },
	{ name = "message 为 null", body = '{"choices":[{"message":null}]}', kind = "named_err",
	  field = "choices[1].message" },
	{ name = "message 是数字", body = '{"choices":[{"message":42}]}', kind = "named_err",
	  field = "message" },

	-- ---------- content 层 ----------
	-- content 为 null 且无调用：驱动给"空文本 + 无调用"的**空响应**，
	-- 由宿主报「既没有返回文本也没有工具调用」（那是宿主的判据，不是驱动的）
	{ name = "content 为 null（无调用）", body = '{"choices":[{"message":{"content":null}}]}',
	  kind = "ok", content = "" },
	{ name = "content 为数字", body = '{"choices":[{"message":{"content":42}}]}',
	  kind = "ok", content = "" },
	{ name = "content 为空数组", body = '{"choices":[{"message":{"content":[]}}]}',
	  kind = "ok", content = "" },
	{ name = "content 为纯 text 数组", body = '{"choices":[{"message":{"content":['
		.. '{"type":"text","text":"a"},{"type":"text","text":"b"}]}}]}',
	  kind = "ok", content = "ab" },
	{ name = "content 数组含非 text 段", body = '{"choices":[{"message":{"content":['
		.. '{"type":"text","text":"看"},{"type":"image_url","image_url":{"url":"x"}}]}}]}',
	  kind = "degrade", content = "看", dropped = "content_parts_dropped" },
	{ name = "content 为单个 part 对象", body = '{"choices":[{"message":{"content":'
		.. '{"type":"text","text":"单段"}}}]}', kind = "ok", content = "单段" },

	-- ---------- tool_calls 层 ----------
	{ name = "tool_calls 为字符串", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":"oops"}}]}', kind = "named_err", field = "tool_calls" },
	{ name = "tool_calls 为数字", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":42}}]}', kind = "named_err", field = "tool_calls" },
	{ name = "tool_calls 为空数组", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":[]}}]}', kind = "ok", content = "x", no_calls = true },
	{ name = "tool_calls 元素为数字", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":[42]}}]}', kind = "degrade", content = "x",
	  dropped = "dropped_bad_tool_calls", no_calls = true },
	{ name = "tool_calls 元素为 null", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":[null]}}]}', kind = "ok", content = "x" },
	{ name = "function 为数字", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":[{"id":"c1","function":42}]}}]}', kind = "degrade", content = "x",
	  dropped = "dropped_bad_tool_calls", no_calls = true },
	{ name = "function 缺失", body = '{"choices":[{"message":{"content":"x",'
		.. '"tool_calls":[{"id":"c1"}]}}]}', kind = "ok", calls = { { id = "c1", name = "" } } },
	{ name = "id 缺失", body = '{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"function":{"name":"t","arguments":"{}"}}]}}]}',
	  kind = "ok", calls = { { id = "", name = "t" } } },
	{ name = "id 为数字", body = '{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":42,"function":{"name":"t","arguments":"{}"}}]}}]}',
	  kind = "ok", calls = { { id = "", name = "t" } } },
	{ name = "arguments 为对象", body = '{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c1","function":{"name":"t","arguments":{"a":1}}}]}}]}',
	  kind = "ok", calls = { { id = "c1", name = "t" } } },	-- 字符串化，由宿主报参数错误
	{ name = "arguments 为数字", body = '{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c1","function":{"name":"t","arguments":42}}]}}]}',
	  kind = "ok", calls = { { id = "c1", name = "t", arguments = "42" } } },
	{ name = "合法与畸形元素混合", body = '{"choices":[{"message":{"content":"x","tool_calls":['
		.. '{"id":"c1","function":{"name":"t","arguments":"{}"}},7]}}]}',
	  kind = "degrade", content = "x", dropped = "dropped_bad_tool_calls",
	  calls = { { id = "c1", name = "t" } } },

	-- ---------- extras / usage ----------
	{ name = "reasoning_content 为空串", body = '{"choices":[{"message":{"content":"x",'
		.. '"reasoning_content":""}}]}', kind = "ok", content = "x", no_extras = true },
	{ name = "reasoning_content 为非字符串", body = '{"choices":[{"message":{"content":"x",'
		.. '"reasoning_content":42}}]}', kind = "ok", content = "x" },
	{ name = "usage 缺失", body = '{"choices":[{"message":{"content":"x"}}]}',
	  kind = "ok", content = "x", no_usage = true },
	{ name = "usage 为非对象", body = '{"choices":[{"message":{"content":"x"}}],"usage":42}',
	  kind = "ok", content = "x", no_usage = true },
	{ name = "usage 为对象", body = '{"choices":[{"message":{"content":"x"}}],'
		.. '"usage":{"prompt_tokens":7,"completion_tokens":3}}',
	  kind = "ok", content = "x", usage_prompt = 7, usage_completion = 3 },
}

-- 逐行执行矩阵
for _, d in ipairs(DRIVERS) do
	local label, wire = d[1], d[2]
	for _, row in ipairs(MATRIX) do
		local ok, res, err = pcall(wire.parse_response, json.decode(row.body))
		local prefix = label .. "/" .. row.name .. "："
		check(prefix .. "不抛裸 Lua 错误", ok, tostring(res))
		if not ok then
			goto continue
		end

		if row.kind == "named_err" then
			check(prefix .. "返回 nil + 点名原因",
				res == nil and tostring(err):find(row.field, 1, true) ~= nil,
				"err=" .. tostring(err))
		else
			check(prefix .. "照常返回表", type(res) == "table", tostring(res))
			if type(res) ~= "table" then
				goto continue
			end
			check(prefix .. "result=OK", res.result == "OK", tostring(res.result))
			if row.content ~= nil then
				check(prefix .. "content 归一化正确",
					res.content == row.content,
					"want [" .. row.content .. "] got [" .. tostring(res.content) .. "]")
			end
			if row.no_calls then
				check(prefix .. "不产生 tool_calls",
					res.tool_calls == nil, J(res.tool_calls))
			end
			if row.calls ~= nil then
				check(prefix .. "tool_calls 条数正确",
					type(res.tool_calls) == "table" and #res.tool_calls == #row.calls,
					J(res.tool_calls))
				for i, want in ipairs(row.calls) do
					local got = res.tool_calls and res.tool_calls[i]
					if got == nil then
						check(prefix .. "第 " .. i .. " 条存在", false, "nil")
					else
						if want.id ~= nil then
							check(prefix .. "第 " .. i .. " 条 id 正确（不合成）",
								got.id == want.id,
								"want [" .. want.id .. "] got [" .. tostring(got.id) .. "]")
						end
						if want.name ~= nil then
							check(prefix .. "第 " .. i .. " 条 name 正确",
								got.name == want.name, tostring(got.name))
						end
						if want.arguments ~= nil then
							check(prefix .. "第 " .. i .. " 条 arguments 正确",
								got.arguments == want.arguments, tostring(got.arguments))
						end
					end
				end
			end
			if row.dropped ~= nil then
				check(prefix .. "丢弃计数记进 extras." .. row.dropped,
					type(res.extras) == "table" and res.extras[row.dropped] >= 1,
					J(res.extras))
			end
			if row.no_extras then
				check(prefix .. "空 reasoning_content 不挂 extras",
					res.extras == nil or res.extras.reasoning_content == nil,
					J(res.extras))
			end
			if row.no_usage then
				check(prefix .. "无 usage 时不挂该键", res.usage == nil,
					J(res.usage))
			end
			if row.usage_prompt ~= nil then
				check(prefix .. "usage 原样透传（宿主只挑两个数）",
					type(res.usage) == "table"
					and res.usage.prompt_tokens == row.usage_prompt
					and res.usage.completion_tokens == row.usage_completion,
					J(res.usage))
			end
		end
		::continue::
	end
end

-- ========== perform 层的形变：HTTP 语义（状态码 / 非 JSON / error 信封） ==========
-- 与上面同源：这些也是"provider 报文畸形成什么样 → 驱动怎么办"。HTTP 层用
-- 单例表的 post 打桩构造（`http` 是 dofile 返回的同一份表）。
do
	local http = dofile("modules/lib/llm_http.lua")
	local real_post = http.post
	local function perform_with(post_ret)
		http.post = function() return post_ret end
		local r = openai.perform("http://x/y", { timeout = 5 }, { model = "m" })
		http.post = real_post
		return r
	end

	-- 200 但正文不是 JSON：点名"不是合法 JSON"并附报文片段
	local r1 = perform_with({ ok = true, status = 200, body = "<html>nope</html>" })
	check("perform/200 非 JSON：点名且带片段",
		r1.result == "ERR" and tostring(r1.message):find("不是合法 JSON", 1, true) ~= nil
		and tostring(r1.message):find("html", 1, true) ~= nil, tostring(r1.message))

	-- 200 但正文是 JSON 标量（不是对象）
	local r2 = perform_with({ ok = true, status = 200, body = "42" })
	check("perform/200 JSON 标量：点名不是对象",
		r2.result == "ERR" and tostring(r2.message):find("不是 JSON 对象", 1, true) ~= nil,
		tostring(r2.message))

	-- 200 但带顶层 error 信封（配额耗尽等）：200 也算 API 错误，且**不可重试**
	local r3 = perform_with({ ok = true, status = 200,
		body = '{"error":{"message":"quota exhausted"}}' })
	check("perform/200 + error 信封：报 API 错误",
		r3.result == "ERR" and tostring(r3.message):find("API 错误", 1, true) ~= nil,
		tostring(r3.message))
	check("perform/200 + error 信封：不带可重试标记（那是「别再试了」）",
		r3.retriable == nil, tostring(r3.retriable))

	-- 空 body
	local r4 = perform_with({ ok = true, status = 200, body = "" })
	check("perform/200 空 body：点名不是合法 JSON",
		r4.result == "ERR" and tostring(r4.message):find("不是合法 JSON", 1, true) ~= nil,
		tostring(r4.message))

	-- 非 200 的状态码语义（SAN-3 的判据，这里钉矩阵形态）
	local r5 = perform_with({ ok = true, status = 429, body = "{}" })
	check("perform/429：可重试 + 带 http_status",
		r5.retriable == true and r5.http_status == 429, J(r5))
	local r6 = perform_with({ ok = true, status = 404, body = "{}" })
	check("perform/404：不可重试 + 带 http_status",
		r6.retriable == false and r6.http_status == 404, J(r6))
end

tester.summary("LLM 驱动响应契约形变矩阵")
