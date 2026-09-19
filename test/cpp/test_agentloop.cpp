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
		[](const Json& args) -> AgentToolResult {
			return {"echo:" + args.toString(), ""};
		}});
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

TH_TEST(agentloop_injects_tool_image_after_all_tool_messages) {
	FakeTransport transport;
	transport.script.push_back(callResponse("shot", "{}"));
	transport.script.push_back(answerResponse("看到了"));

	AgentToolRegistry tools;
	tools.add(AgentTool{"shot", "拍照", parseJson("{\"type\":\"object\"}"),
		[](const Json&) -> AgentToolResult {
			return {"已拍摄照片", "data:image/jpeg;base64,Zm9vYmFy"};
		}});

	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("看看周围") == "看到了");

	/* system,user,assistant(tool_calls),tool,user(图片) —— 图片必须排在
	 * 全部 tool 消息**之后**：OpenAI 要求 tool_calls 后紧跟对应 tool 消息，
	 * 中间插 user 消息会被 400 拒绝（tool 消息本身又只允许文本） */
	const auto& seen = transport.requests[1];
	TH_CHECK(seen.size() == 5);
	TH_CHECK(seen[2].role == "assistant");
	TH_CHECK(seen[3].role == "tool");
	TH_CHECK(seen[4].role == "user");
	TH_CHECK(seen[4].images.size() == 1);
	TH_CHECK(seen[4].images[0] == "data:image/jpeg;base64,Zm9vYmFy");
	TH_CHECK(seen[4].toolCallId.empty());
	/* 历史里也留住了图片（下一轮 REPL 任务仍看得到它） */
	TH_CHECK(session.history().size() == 6);
	TH_CHECK(session.history()[4].images.size() == 1);
}

TH_TEST(agentloop_text_only_tool_adds_no_image_message) {
	FakeTransport transport;
	transport.script.push_back(callResponse("echo", "{}"));
	transport.script.push_back(answerResponse("好"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("任务") == "好");
	TH_CHECK(transport.requests[1].size() == 4);	/* 没有多余的图片消息 */
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

/* ---- 历史字节预算裁剪（REPL 长会话）----
 * 任务块 = 一条任务消息到下一条任务消息之前的全部消息；裁剪以块为单位，
 * 保证 assistant.tool_calls 与其 tool 消息、图片 user 消息不被拆散。 */

AgentOptions budgetOpts(int maxTurns, std::size_t maxHistoryBytes) {
	AgentOptions o = optsFor(maxTurns);
	o.maxHistoryBytes = maxHistoryBytes;
	o.systemPrompt = "SYS";
	return o;
}

bool historyContains(const AgentSession& s, const std::string& text) {
	for (const ChatMessage& m : s.history()) {
		if (m.content.find(text) != std::string::npos) { return true; }
	}
	return false;
}

TH_TEST(agentloop_history_trimmed_to_byte_budget) {
	FakeTransport transport;
	/* 每块 = user(6B) + assistant(8B) = 14B；system 3B。
	 * 预算 40B：第 4 轮开始前总量 45B 超限，装得下的只有最近两块（28B），
	 * 第 1 块被整块裁掉。 */
	transport.script.push_back(answerResponse("ANSWER-1"));
	transport.script.push_back(answerResponse("ANSWER-2"));
	transport.script.push_back(answerResponse("ANSWER-3"));
	transport.script.push_back(answerResponse("ANSWER-4"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, budgetOpts(4, 40));
	TH_CHECK(session.runTurn("TASK-1") == "ANSWER-1");
	TH_CHECK(session.runTurn("TASK-2") == "ANSWER-2");
	TH_CHECK(session.runTurn("TASK-3") == "ANSWER-3");
	TH_CHECK(session.runTurn("TASK-4") == "ANSWER-4");

	/* system + 保留的两块（2×2 条）+ 本轮 user/assistant */
	TH_CHECK(session.history().size() == 7);
	TH_CHECK(session.history()[0].content == "SYS");
	TH_CHECK(session.history()[1].content == "TASK-2");
	TH_CHECK(!historyContains(session, "TASK-1"));
	TH_CHECK(!historyContains(session, "ANSWER-1"));
	/* 第 4 轮的请求只带裁剪后的历史：system + 第2/3块 + 本轮 user */
	TH_CHECK(transport.requests[3].size() == 6);
	for (const ChatMessage& m : transport.requests[3]) {
		TH_CHECK(m.content.find("TASK-1") == std::string::npos);
	}
}

TH_TEST(agentloop_history_trim_drops_old_block_with_its_image) {
	FakeTransport transport;
	transport.script.push_back(callResponse("shot", "{}"));	/* 块1：拍照 → 图片消息 */
	transport.script.push_back(answerResponse("ANSWER-1"));
	transport.script.push_back(answerResponse("ANSWER-2"));	/* 块2 */
	transport.script.push_back(answerResponse("ANSWER-3"));	/* 块3 */

	AgentToolRegistry tools;
	tools.add(AgentTool{"shot", "拍照", parseJson("{\"type\":\"object\"}"),
		[](const Json&) -> AgentToolResult {
			return {"已拍摄照片",
				"data:image/jpeg;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
		}});

	AgentSession session(transport, tools, budgetOpts(4, 40));
	TH_CHECK(session.runTurn("TASK-1") == "ANSWER-1");
	/* 块1 里的图片 user 消息在预算内时随历史保留 */
	bool hasImage = false;
	for (const ChatMessage& m : session.history()) {
		if (!m.images.empty()) { hasImage = true; }
	}
	TH_CHECK(hasImage);

	TH_CHECK(session.runTurn("TASK-2") == "ANSWER-2");
	TH_CHECK(session.runTurn("TASK-3") == "ANSWER-3");
	/* 块1（≈90B，大头是 base64 图片）超预算被整块裁掉：图片一并淘汰 */
	hasImage = false;
	for (const ChatMessage& m : session.history()) {
		if (!m.images.empty()) { hasImage = true; }
	}
	TH_CHECK(!hasImage);
	TH_CHECK(!historyContains(session, "TASK-1"));
	TH_CHECK(session.history().size() == 5);	/* system + 块2(2) + 块3(2) */
}

TH_TEST(agentloop_history_untrimmed_when_budget_zero) {
	FakeTransport transport;
	for (int i = 1; i <= 3; ++i) {
		transport.script.push_back(answerResponse("ANSWER-" + std::to_string(i)));
	}

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, budgetOpts(4, 0));	/* 0 = 不裁剪 */
	TH_CHECK(session.runTurn("TASK-1") == "ANSWER-1");
	TH_CHECK(session.runTurn("TASK-2") == "ANSWER-2");
	TH_CHECK(session.runTurn("TASK-3") == "ANSWER-3");
	TH_CHECK(historyContains(session, "TASK-1"));
	TH_CHECK(session.history().size() == 7);
}
