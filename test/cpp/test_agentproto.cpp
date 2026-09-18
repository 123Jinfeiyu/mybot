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

	Json body = buildChatRequest("m", messages, tools, 4096, 0.7, 0.9);
	TH_CHECK(body.get("model")->getValue() == "m");
	TH_CHECK(body.get("stream")->isBool() && !body.get("stream")->getBool());
	TH_CHECK(body.get("max_tokens")->getInt() == 4096);
	TH_CHECK(body.get("temperature")->getFloat() == 0.7);
	TH_CHECK(body.get("top_p")->getFloat() == 0.9);
	TH_CHECK(body.get("messages")->size() == 2);
	TH_CHECK(body.get("messages")->at(0).get("role")->getValue() == "system");
	TH_CHECK(body.get("messages")->at(1).get("content")->getValue() == "你好");
	TH_CHECK(body.get("tools")->size() == 1);
	TH_CHECK(body.get("tools")->at(0).get("type")->getValue() == "function");
	TH_CHECK(body.get("tools")->at(0).get("function")->get("name")->getValue()
		== "list_action_scripts");
	TH_CHECK(body.get("tool_choice")->getValue() == "auto");

	/* 序列化 → 再解析：验证产出的是可被真实服务端接受的 JSON 文本 */
	Json round = parseJson(body.toString());
	TH_CHECK(round.get("messages")->size() == 2);
}

TH_TEST(agentproto_request_without_tools_omits_field) {
	Json body = buildChatRequest("m", {textMsg("user", "hi")}, {}, 100, 0.1, 0.2);
	TH_CHECK(body.get("tools") == std::nullopt);
	TH_CHECK(body.get("tool_choice") == std::nullopt);
}

TH_TEST(agentproto_assistant_tool_call_message) {
	ChatMessage m;
	m.role = "assistant";
	m.content = "";
	m.toolCalls.push_back(ToolCall{"call_1", "run_action_script", "{\"script\":\"std.mt\"}"});
	Json j = messageToJson(m);
	TH_CHECK(j.get("role")->getValue() == "assistant");
	/* 无过渡语时 content 必须是 null（空串会被部分实现当成「有文本」而拒收） */
	TH_CHECK(j.get("content")->isNull());
	TH_CHECK(j.get("tool_calls")->size() == 1);
	TH_CHECK(j.get("tool_calls")->at(0).get("id")->getValue() == "call_1");
	TH_CHECK(j.get("tool_calls")->at(0).get("type")->getValue() == "function");
	TH_CHECK(j.get("tool_calls")->at(0).get("function")->get("name")->getValue()
		== "run_action_script");
	TH_CHECK(j.get("tool_calls")->at(0).get("function")->get("arguments")->getValue()
		== "{\"script\":\"std.mt\"}");
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

TH_TEST(agentproto_parse_content_only) {
	ChatResponse r = parseChatResponse(
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"好了\"}}]}");
	TH_CHECK(!r.hasToolCalls());
	TH_CHECK(r.content == "好了");
}

TH_TEST(agentproto_parse_tool_calls) {
	ChatResponse r = parseChatResponse(
		"{\"choices\":[{\"message\":{\"content\":\"先看看\",\"tool_calls\":["
		"{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"list_hardware\","
		"\"arguments\":\"{}\"}}]}}]}");
	TH_CHECK(r.content == "先看看");
	TH_CHECK(r.hasToolCalls());
	TH_CHECK(r.toolCalls.size() == 1);
	TH_CHECK(r.toolCalls[0].id == "c1");
	TH_CHECK(r.toolCalls[0].name == "list_hardware");
	TH_CHECK(r.toolCalls[0].arguments == "{}");
}

TH_TEST(agentproto_parse_arguments_as_object) {
	/* 少数兼容实现把 arguments 直接回成对象而非字符串：统一成字符串形态 */
	ChatResponse r = parseChatResponse(
		"{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"c\",\"function\":"
		"{\"name\":\"t\",\"arguments\":{\"a\":1}}}]}}]}");
	TH_CHECK(r.hasToolCalls());
	TH_CHECK(parseToolArguments(r.toolCalls[0].arguments).get("a")->getInt() == 1);
}

TH_TEST(agentproto_parse_errors) {
	TH_CHECK_THROWS_MSG(std::runtime_error, "API 错误",
		parseChatResponse("{\"error\":{\"message\":\"bad key\"}}"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "没有 choices",
		parseChatResponse("{\"choices\":[]}"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "缺少 message",
		parseChatResponse("{\"choices\":[{}]}"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "既没有返回文本",
		parseChatResponse("{\"choices\":[{\"message\":{\"content\":null}}]}"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "响应为空", parseChatResponse("   "));
	/* 坏 JSON：parseJson 的 IllegalArgumentException（runtime_error 子类）外抛 */
	TH_CHECK_THROWS(std::runtime_error, parseChatResponse("not json"));
}

TH_TEST(agentproto_parse_tool_arguments) {
	TH_CHECK(parseToolArguments("").isObj());
	TH_CHECK(parseToolArguments("   ").isObj());
	TH_CHECK(parseToolArguments("{\"x\":1}").get("x")->getInt() == 1);
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "不是 JSON 对象",
		parseToolArguments("[1,2]"));
	TH_CHECK_THROWS(IllegalArgumentException, parseToolArguments("{bad}"));
}
