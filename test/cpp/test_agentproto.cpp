#include "harness.hpp"

#include <string>
#include <vector>

#include "agentproto.hpp"
#include "jsonparse.hpp"
#include "util.hpp"

using namespace bot;
using namespace bot::agent;

namespace {

ChatMessage textMsg(const std::string& role, const std::string& content) {
	ChatMessage m;
	m.role = role;
	m.content = content;
	return m;
}

Json toolSpecParams() {
	return parseJson("{\"type\":\"object\",\"properties\":{},\"required\":[]}");
}

} // namespace

TH_TEST(agentproto_request_shape) {
	std::vector<ChatMessage> messages{textMsg("system", "sys"), textMsg("user", "你好")};
	std::vector<ToolSpec> tools{{"list_action_scripts", "列出脚本", toolSpecParams()}};

	Json req = buildDriverRequest("m", messages, tools, 4096, 0.7, 0.9, 180);
	TH_CHECK(req.get("model")->getValue() == "m");
	TH_CHECK(req.get("max_tokens")->getInt() == 4096);
	TH_CHECK(req.get("temperature")->getFloat() == 0.7);
	TH_CHECK(req.get("top_p")->getFloat() == 0.9);
	TH_CHECK(req.get("timeout")->getInt() == 180);
	TH_CHECK(req.get("messages")->size() == 2);
	TH_CHECK(req.get("messages")->at(0).get("role")->getValue() == "system");
	TH_CHECK(req.get("messages")->at(1).get("content")->getValue() == "你好");
	TH_CHECK(req.get("tools")->size() == 1);
	/* 规范化协议里工具是**扁平**的 {name, description, parameters}：包成
	 * {"type":"function","function":{...}} 是 Lua 驱动的事。这里若提前包一层，
	 * 驱动会再包一层，function 变空 —— 模型看不到任何工具，而循环表面上还在跑
	 * （曾真实发生过，端到端也没抓住，故此处显式钉住「不得出现 provider 字段」）。
	 * 注意 get() 返回的是 optional 值，必须先接住再取引用，否则引用悬垂。 */
	auto toolsOpt = req.get("tools");
	const Json& tool = toolsOpt->at(0);
	TH_CHECK(tool.get("name")->getValue() == "list_action_scripts");
	TH_CHECK(tool.get("description")->getValue() == "列出脚本");
	TH_CHECK(tool.get("parameters")->isObj());
	TH_CHECK(tool.get("type") == std::nullopt);
	TH_CHECK(tool.get("function") == std::nullopt);

	/* 序列化 → 再解析：验证产出的是可交给 Lua 驱动的合法 JSON */
	Json round = parseJson(req.toString());
	TH_CHECK(round.get("messages")->size() == 2);
}

TH_TEST(agentproto_request_without_tools_omits_field) {
	Json req = buildDriverRequest("m", {textMsg("user", "hi")}, {}, 100, 0.1, 0.2, 60);
	TH_CHECK(req.get("tools") == std::nullopt);
}

TH_TEST(agentproto_assistant_message_is_canonical) {
	ChatMessage m;
	m.role = "assistant";
	m.content = "";
	m.toolCalls.push_back(ToolCall{"call_1", "run_action_script", "{\"script\":\"std.mt\"}"});
	m.extras = parseJson("{\"reasoning_content\":\"想一想\"}");

	Json j = messageToJson(m);
	TH_CHECK(j.get("role")->getValue() == "assistant");
	TH_CHECK(j.get("content")->isStr());	/* provider 侧的 null/省略由驱动决定 */
	TH_CHECK(j.get("tool_calls")->size() == 1);
	TH_CHECK(j.get("tool_calls")->at(0).get("id")->getValue() == "call_1");
	TH_CHECK(j.get("tool_calls")->at(0).get("name")->getValue() == "run_action_script");
	TH_CHECK(j.get("tool_calls")->at(0).get("arguments")->getValue()
		== "{\"script\":\"std.mt\"}");
	/* extras 原样透传：正是 DeepSeek 的 reasoning_content 回传所依赖的通道 */
	TH_CHECK(j.get("extras")->get("reasoning_content")->getValue() == "想一想");
}

TH_TEST(agentproto_message_without_extras_omits_field) {
	ChatMessage m = textMsg("user", "hi");
	Json j = messageToJson(m);
	TH_CHECK(j.get("extras") == std::nullopt);
	TH_CHECK(j.get("content")->getValue() == "hi");
}

TH_TEST(agentproto_image_message_is_multimodal) {
	ChatMessage m;
	m.role = "user";
	m.content = "看一下这张图";
	m.images.push_back("data:image/jpeg;base64,Zm9vYmFy");

	Json j = messageToJson(m);
	TH_CHECK(j.get("role")->getValue() == "user");
	auto content = j.get("content");
	TH_CHECK(content->isArr());		/* 带图时 content 变成多模态数组 */
	TH_CHECK(content->size() == 2);
	TH_CHECK(content->at(0).get("type")->getValue() == "text");
	TH_CHECK(content->at(0).get("text")->getValue() == "看一下这张图");
	TH_CHECK(content->at(1).get("type")->getValue() == "image_url");
	TH_CHECK(content->at(1).get("image_url")->get("url")->getValue()
		== "data:image/jpeg;base64,Zm9vYmFy");

	/* 无图片时仍是纯字符串（保持既有报文形态不变） */
	ChatMessage plain = textMsg("user", "你好");
	TH_CHECK(messageToJson(plain).get("content")->isStr());
}

/* B3：游标模式——图片不进请求 Json，以 {"__bot_img": N} 占位并登记引用 */
TH_TEST(agentproto_image_cursor_mode_places_placeholder) {
	ChatMessage m;
	m.role = "user";
	m.content = "看一下这张图";
	m.images.push_back("data:image/jpeg;base64,Zm9vYmFy");
	m.images.push_back("data:image/png;base64,aHVnZQ==");

	std::vector<const std::string*> refs;
	Json j = messageToJson(m, &refs);
	auto content = j.get("content");
	TH_CHECK(content->size() == 3);
	auto url = content->at(1).get("image_url")->get("url");
	TH_CHECK(url->isObj());
	TH_CHECK(url->get("__bot_img")->getInt() == 1);	/* 游标从 1 起 */
	auto url2 = content->at(2).get("image_url")->get("url");
	TH_CHECK(url2->get("__bot_img")->getInt() == 2);
	/* 引用按游标顺序登记，指向消息里的字符串本体（不拷贝字节） */
	TH_CHECK(refs.size() == 2);
	TH_CHECK(*refs[0] == "data:image/jpeg;base64,Zm9vYmFy");
	TH_CHECK(*refs[1] == "data:image/png;base64,aHVnZQ==");
	/* 游标模式下字节本体绝不出现在请求 Json 里 */
	TH_CHECK(j.toString().find("Zm9vYmFy") == std::string::npos);
}

TH_TEST(agentproto_tool_result_message) {
	ChatMessage m;
	m.role = "tool";
	m.content = "已执行";
	m.toolCallId = "call_1";
	Json j = messageToJson(m);
	TH_CHECK(j.get("role")->getValue() == "tool");
	TH_CHECK(j.get("tool_call_id")->getValue() == "call_1");
	TH_CHECK(j.get("content")->getValue() == "已执行");
}

TH_TEST(agentproto_parse_response_content_only) {
	ChatResponse r = parseDriverResponse(parseJson(
		"{\"result\":\"OK\",\"content\":\"好了\"}"));
	TH_CHECK(!r.hasToolCalls());
	TH_CHECK(r.content == "好了");
}

TH_TEST(agentproto_parse_response_tool_calls_and_extras) {
	ChatResponse r = parseDriverResponse(parseJson(
		"{\"result\":\"OK\",\"content\":\"先看看\",\"tool_calls\":["
		"{\"id\":\"c1\",\"name\":\"list_hardware\",\"arguments\":\"{}\"}],"
		"\"extras\":{\"reasoning_content\":\"想过了\"}}"));
	TH_CHECK(r.content == "先看看");
	TH_CHECK(r.hasToolCalls());
	TH_CHECK(r.toolCalls.size() == 1);
	TH_CHECK(r.toolCalls[0].id == "c1");
	TH_CHECK(r.toolCalls[0].name == "list_hardware");
	TH_CHECK(r.toolCalls[0].arguments == "{}");
	TH_CHECK(r.extras.isObj());
	TH_CHECK(r.extras.get("reasoning_content")->getValue() == "想过了");
}

TH_TEST(agentproto_parse_response_arguments_as_object) {
	/* 驱动侧若把 arguments 直接给成对象，也统一成字符串形态 */
	ChatResponse r = parseDriverResponse(parseJson(
		"{\"result\":\"OK\",\"tool_calls\":[{\"id\":\"c\",\"name\":\"t\","
		"\"arguments\":{\"a\":1}}]}"));
	TH_CHECK(r.hasToolCalls());
	TH_CHECK(parseToolArguments(r.toolCalls[0].arguments).get("a")->getInt() == 1);
}

TH_TEST(agentproto_parse_response_errors) {
	TH_CHECK_THROWS_MSG(std::runtime_error, "驱动返回错误",
		parseDriverResponse(parseJson("{\"result\":\"ERR\",\"message\":\"libcurl 不可用\"}")));
	TH_CHECK_THROWS_MSG(std::runtime_error, "既没有返回文本",
		parseDriverResponse(parseJson("{\"result\":\"OK\",\"content\":\"\"}")));
	TH_CHECK_THROWS_MSG(std::runtime_error, "不是表",
		parseDriverResponse(parseJson("\"just a string\"")));
	/* ERR 报错要把驱动给的 message 带出来（否则排障时只有「失败」两个字） */
	try {
		parseDriverResponse(parseJson("{\"result\":\"ERR\",\"message\":\"HTTP 401: bad key\"}"));
		TH_CHECK(false);
	} catch (const std::exception& e) {
		TH_CHECK(std::string(e.what()).find("HTTP 401") != std::string::npos);
	}
}

TH_TEST(agentproto_parse_tool_arguments) {
	TH_CHECK(parseToolArguments("").isObj());
	TH_CHECK(parseToolArguments("   ").isObj());
	TH_CHECK(parseToolArguments("{\"x\":1}").get("x")->getInt() == 1);
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "不是 JSON 对象",
		parseToolArguments("[1,2]"));
	TH_CHECK_THROWS(IllegalArgumentException, parseToolArguments("{bad}"));
}

/* ============ SAN-11：宿主侧的响应形变矩阵 ============
 * 与 Lua 侧 test/test_llm_wire_contract.lua 互补：那边测"驱动把 provider 报文
 * 整形成什么"，这边测"宿主拿到规范化结果之后的容错"。两处边界必须一致地
 * **点名报错或如实降级**，而不是抛裸的解析错误或静默丢东西。 */
TH_TEST(agentproto_host_side_shape_matrix) {
	/* ① 缺 name 的调用被丢弃，且整轮为空时如实报数（SAN-8③） */
	{
		Json res = parseJson("{\"result\":\"OK\",\"content\":\"\",\"tool_calls\":["
			"{\"id\":\"c1\",\"name\":\"\",\"arguments\":\"{}\"},"
			"{\"id\":\"c2\",\"name\":\"\",\"arguments\":\"{}\"}]}");
		try {
			parseDriverResponse(res);
			TH_CHECK_MSG(false, "应抛错");
		} catch (const std::exception& e) {
			TH_CHECK_MSG(std::string(e.what()).find("2 条") != std::string::npos,
				"应报出丢弃 2 条，实际: " + std::string(e.what()));
		}
	}
	/* ② 有文本时缺 name 的调用被安静丢弃（模型仍给了回答，不该报错） */
	{
		Json res = parseJson("{\"result\":\"OK\",\"content\":\"我看看\",\"tool_calls\":["
			"{\"id\":\"c1\",\"name\":\"\",\"arguments\":\"{}\"}]}");
		ChatResponse r = parseDriverResponse(res);
		TH_CHECK(r.content == "我看看");
		TH_CHECK(r.toolCalls.empty());
	}
	/* ③ 非字符串 id/name/arguments 的宿主容错 */
	{
		Json res = parseJson("{\"result\":\"OK\",\"content\":\"\",\"tool_calls\":["
			"{\"id\":42,\"name\":\"t\",\"arguments\":{\"a\":1}}]}");
		ChatResponse r = parseDriverResponse(res);
		TH_CHECK(r.toolCalls.size() == 1);
		TH_CHECK(r.toolCalls[0].id.empty());		/* 数字 id 被丢弃，由宿主合成 */
		TH_CHECK(r.toolCalls[0].name == "t");
		TH_CHECK(r.toolCalls[0].arguments == "{\"a\":1}");	/* 对象→字符串，供参数解析报错 */
	}
	/* ④ tool_calls 非数组（字符串）：整轮无调用 → 落在既有兜底错误上 */
	{
		Json res = parseJson("{\"result\":\"OK\",\"content\":\"\",\"tool_calls\":\"oops\"}");
		TH_CHECK_THROWS_MSG(std::runtime_error, "既没有返回文本也没有工具调用",
			parseDriverResponse(res));
	}
	/* ⑤ extras / usage 透传；缺失时为空 */
	{
		Json res = parseJson("{\"result\":\"OK\",\"content\":\"x\","
			"\"extras\":{\"reasoning_content\":\"想一想\"},"
			"\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}");
		ChatResponse r = parseDriverResponse(res);
		TH_CHECK(r.extras.isObj());
		TH_CHECK(r.usage.isObj());
		TH_CHECK(r.usage.find("prompt_tokens") != nullptr);
	}
	{
		Json res = parseJson("{\"result\":\"OK\",\"content\":\"x\"}");
		ChatResponse r = parseDriverResponse(res);
		TH_CHECK(r.usage.isNull());
	}
	/* ⑥ result 非 "OK" 一律抛，且带上驱动给的 message（不是裸解析错误） */
	{
		Json res = parseJson("{\"result\":\"ERR\",\"message\":\"驱动说不行\"}");
		TH_CHECK_THROWS_MSG(std::runtime_error, "驱动说不行", parseDriverResponse(res));
	}
}

/* ================= A7：provider 回传字段的长度上限 =================
 * id / name 都来自远端，却会进历史并逐轮原样回传；不设上限就等于让网关决定
 * 回传文本的体积。id 超限**不能截断**（截断会破坏与 assistant.tool_calls 的
 * 配对），所以判据是整条响应拒绝。 */
TH_TEST(agentproto_parse_response_rejects_oversized_id_and_name) {
	const std::string atLimit(proto_limits::kMaxToolCallIdBytes, 'a');
	const std::string overLimit(proto_limits::kMaxToolCallIdBytes + 1, 'a');

	/* 恰好等于上限放行：判据是 >，不是 >=（ off-by-one 会把合法 id 拒掉） */
	ChatResponse ok = parseDriverResponse(parseJson(
		"{\"result\":\"OK\",\"tool_calls\":[{\"id\":\"" + atLimit
		+ "\",\"name\":\"echo\",\"arguments\":\"{}\"}]}"));
	TH_CHECK(ok.toolCalls.size() == 1);
	TH_CHECK(ok.toolCalls[0].id == atLimit);

	TH_CHECK_THROWS_MSG(std::runtime_error, "tool_calls[].id",
		parseDriverResponse(parseJson(
			"{\"result\":\"OK\",\"tool_calls\":[{\"id\":\"" + overLimit
			+ "\",\"name\":\"echo\",\"arguments\":\"{}\"}]}")));
	/* 报错要点名实测字节数、上限与"为何不能截断"（排障时不用猜） */
	try {
		parseDriverResponse(parseJson(
			"{\"result\":\"OK\",\"tool_calls\":[{\"id\":\"" + overLimit
			+ "\",\"name\":\"echo\",\"arguments\":\"{}\"}]}"));
		TH_CHECK(false);
	} catch (const std::exception& e) {
		const std::string w = e.what();
		TH_CHECK_MSG(w.find(std::to_string(overLimit.size())) != std::string::npos,
			"应给出实测字节数: " + w);
		TH_CHECK_MSG(w.find(std::to_string(proto_limits::kMaxToolCallIdBytes)) != std::string::npos,
			"应给出上限: " + w);
		TH_CHECK_MSG(w.find("配对") != std::string::npos, "应说明为何不能截断: " + w);
	}

	const std::string longName(proto_limits::kMaxToolNameBytes + 1, 'n');
	TH_CHECK_THROWS_MSG(std::runtime_error, "tool_calls[].name",
		parseDriverResponse(parseJson(
			"{\"result\":\"OK\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"" + longName
			+ "\",\"arguments\":\"{}\"}]}")));
	/* 上限之内的真实工具名不受影响（最长的内置名 run_action_script = 17 字节） */
	ChatResponse real = parseDriverResponse(parseJson(
		"{\"result\":\"OK\",\"tool_calls\":[{\"id\":\"call_1\",\"name\":\"run_action_script\","
		"\"arguments\":\"{\\\"script\\\":\\\"std.mt\\\"}\"}]}"));
	TH_CHECK(real.toolCalls.size() == 1);
	TH_CHECK(real.toolCalls[0].name == "run_action_script");
}
