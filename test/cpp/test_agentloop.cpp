#include "harness.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentloop.hpp"
#include "agenttools.hpp"
#include "jsonparse.hpp"
#include "util.hpp"

using namespace bot;
using namespace bot::agent;

namespace {

/* 脚本化传输：按预设序列返回响应，并记录每次请求的消息快照 */
class FakeTransport : public ChatTransport {
public:
	std::vector<ChatResponse> script;
	std::vector<std::vector<ChatMessage>> requests;

	ChatResponse complete(const std::vector<ChatMessage>& msgs,
			const std::vector<ToolSpec>&) override {
		requests.push_back(msgs);
		if (script.empty()) { throw std::runtime_error("假传输脚本用尽"); }
		ChatResponse r = script.front();
		script.erase(script.begin());
		return r;
	}
};

ChatResponse callResponse(const std::string& name, const std::string& args,
		const std::string& id = "c1") {
	ChatResponse r;
	r.toolCalls.push_back(ToolCall{id, name, args});
	return r;
}

ChatResponse answerResponse(const std::string& text) {
	ChatResponse r;
	r.content = text;
	return r;
}

AgentToolRegistry stubTools() {
	AgentToolRegistry reg;
	reg.add(AgentTool{"echo", "回显参数", parseJson("{\"type\":\"object\"}"),
		[](const Json& args) { return "echo:" + args.toString(); }});
	return reg;
}

AgentOptions optsFor(int maxTurns, std::ostream* log = nullptr) {
	AgentOptions o;
	o.maxTurns = maxTurns;
	o.verbose = false;
	o.log = log;
	o.systemPrompt = "测试系统提示词";
	return o;
}

} // namespace

TH_TEST(agentloop_final_answer_only) {
	FakeTransport transport;
	transport.script.push_back(answerResponse("完成"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	std::string out = session.runTurn("做点什么");

	TH_CHECK(out == "完成");
	TH_CHECK(session.history().size() == 3);
	TH_CHECK(session.history()[0].role == "system");
	TH_CHECK(session.history()[0].content == "测试系统提示词");
	TH_CHECK(session.history()[1].role == "user");
	TH_CHECK(session.history()[2].role == "assistant");
	TH_CHECK(transport.requests.size() == 1);
	TH_CHECK(transport.requests[0].size() == 2);	/* system + user */
}

TH_TEST(agentloop_tool_call_then_answer) {
	FakeTransport transport;
	transport.script.push_back(callResponse("echo", "{\"x\":1}"));
	transport.script.push_back(answerResponse("好了"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	std::string out = session.runTurn("任务");

	TH_CHECK(out == "好了");
	TH_CHECK(transport.requests.size() == 2);
	const auto& second = transport.requests[1];
	TH_CHECK(second.size() == 4);	/* system, user, assistant, tool */
	TH_CHECK(second[2].role == "assistant");
	TH_CHECK(second[2].toolCalls.size() == 1);
	TH_CHECK(second[2].toolCalls[0].id == "c1");
	TH_CHECK(second[2].toolCalls[0].name == "echo");
	TH_CHECK(second[3].role == "tool");
	TH_CHECK(second[3].toolCallId == "c1");
	TH_CHECK(second[3].content.find("echo:") != std::string::npos);
	/* system, user, assistant(tool_calls), tool, assistant(最终回答) */
	TH_CHECK(session.history().size() == 5);
}

TH_TEST(agentloop_unknown_tool_fed_back) {
	FakeTransport transport;
	transport.script.push_back(callResponse("no_such_tool", "{}"));
	transport.script.push_back(answerResponse("换个办法"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("任务") == "换个办法");
	TH_CHECK(transport.requests[1][3].content.find("未知工具") != std::string::npos);
}

TH_TEST(agentloop_bad_arguments_fed_back) {
	FakeTransport transport;
	transport.script.push_back(callResponse("echo", "not json"));
	transport.script.push_back(answerResponse("收到"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("任务") == "收到");
	TH_CHECK(transport.requests[1][3].content.find("工具参数错误") != std::string::npos);
}

TH_TEST(agentloop_synthesizes_missing_call_id) {
	FakeTransport transport;
	transport.script.push_back(callResponse("echo", "{}", ""));
	transport.script.push_back(answerResponse("ok"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("任务") == "ok");
	/* assistant.tool_calls 与 tool.tool_call_id 必须一一对上，否则服务端 400 */
	TH_CHECK(transport.requests[1][2].toolCalls[0].id == "call_0_0");
	TH_CHECK(transport.requests[1][2].toolCalls[0].id == transport.requests[1][3].toolCallId);
}

TH_TEST(agentloop_max_turns_rolls_back_history) {
	FakeTransport transport;
	for (int i = 0; i < 4; ++i) { transport.script.push_back(callResponse("echo", "{}")); }

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(2));
	TH_CHECK_THROWS_MSG(std::runtime_error, "轮次上限", session.runTurn("任务"));
	/* 回滚到只有 system：半截的 tool_calls 序列会让下一轮请求被 400 拒绝 */
	TH_CHECK(session.history().size() == 1);
}

TH_TEST(agentloop_transport_error_rolls_back_history) {
	FakeTransport transport;	/* 空脚本 → 第一次 complete 即抛 */

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(2));
	TH_CHECK_THROWS_MSG(std::runtime_error, "脚本用尽", session.runTurn("任务"));
	TH_CHECK(session.history().size() == 1);
}

TH_TEST(agentloop_history_accumulates_across_turns) {
	FakeTransport transport;
	transport.script.push_back(answerResponse("第一次"));
	transport.script.push_back(answerResponse("第二次"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("A") == "第一次");
	TH_CHECK(session.runTurn("B") == "第二次");
	TH_CHECK(session.history().size() == 5);	/* system,user,asst,user,asst */
	/* 第二轮请求带着上一轮的历史（REPL 里机器人"记得"之前做过什么） */
	TH_CHECK(transport.requests[1].size() == 4);
	TH_CHECK(transport.requests[1][2].content == "第一次");
	TH_CHECK(transport.requests[1][3].content == "B");
}

TH_TEST(agentloop_stop_requested_aborts) {
	stopRequested().store(true);
	FakeTransport transport;
	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(2));
	TH_CHECK_THROWS_MSG(std::runtime_error, "中断", session.runTurn("任务"));
	TH_CHECK(session.history().size() == 1);
	stopRequested().store(false);
}

TH_TEST(agentloop_verbose_logs_tool_activity) {
	std::ostringstream log;
	FakeTransport transport;
	transport.script.push_back(callResponse("echo", "{\"x\":1}"));
	transport.script.push_back(answerResponse("好了"));

	AgentToolRegistry tools = stubTools();
	AgentOptions opts = optsFor(4, &log);
	opts.verbose = true;
	AgentSession session(transport, tools, opts);
	TH_CHECK(session.runTurn("任务") == "好了");
	TH_CHECK(log.str().find("[工具] echo") != std::string::npos);
}
