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
