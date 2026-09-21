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

/* 固定体积结果的工具（AG-4 用）：单次结果固定 n 字节，便于精确推预算 */
AgentToolRegistry bigTools(std::size_t n) {
	AgentToolRegistry reg;
	reg.add(AgentTool{"big", "返回固定体积文本", parseJson("{\"type\":\"object\"}"),
		[n](const Json&) -> AgentToolResult {
			return {std::string(n, 'x'), ""};
		}});
	return reg;
}

/* 带固定体积图片的工具（AG-4 图片预算用） */
AgentToolRegistry imageTools(std::size_t imgBytes) {
	AgentToolRegistry reg;
	reg.add(AgentTool{"shoot", "返回一张图", parseJson("{\"type\":\"object\"}"),
		[imgBytes](const Json&) -> AgentToolResult {
			return {"已拍照", std::string("data:image/jpeg;base64,") + std::string(imgBytes, 'A')};
		}});
	return reg;
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

TH_TEST(agentloop_max_turns_keeps_completed_history) {
	FakeTransport transport;
	for (int i = 0; i < 4; ++i) { transport.script.push_back(callResponse("echo", "{}")); }

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(2));
	TH_CHECK_THROWS_MSG(std::runtime_error, "轮次上限", session.runTurn("任务"));
	/* 轮次上限时两轮的 tool_calls 都已完整回传，序列本身可发送：历史必须原样留着。
	 * 硬件已真动过，抹掉这段就是告诉模型「什么都没发生」。 */
	TH_CHECK(session.history().size() == 6);	/* system + user + (asst+tool) x2 */
	/* 协议完整性：每条带 tool_calls 的 assistant 后面跟着等量的 tool 消息，
	 * 且 id 一一对上（半截序列才会被服务端 400 拒绝） */
	const auto& h = session.history();
	for (std::size_t i = 0; i < h.size(); ++i) {
		if (h[i].role != "assistant" || h[i].toolCalls.empty()) { continue; }
		const std::size_t n = h[i].toolCalls.size();
		TH_CHECK(i + n < h.size());
		for (std::size_t k = 0; k < n; ++k) {
			TH_CHECK(h[i + 1 + k].role == "tool");
			TH_CHECK(h[i + 1 + k].toolCallId == h[i].toolCalls[k].id);
		}
	}
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

/* ---- AG-25（第三轮审计）：回滚边界的两条**未被覆盖**的语义 ----
 * docs/agent.md 声称「失败时只裁掉半截的 tool_calls 尾部 / 轮次上限保留本轮历史」，
 * 但 loop 级单测原先只覆盖 complete == base 那一种（turn-0 传输错误，
 * agentloop_transport_error_rolls_back_history）：那种情况下待回滚的只有刚 push 的
 * user 消息，边界逻辑其实没被检验过。下面两条把「已完成轮次要留下、半截尾部要去掉」
 * 分开钉住。 */

/* 传输在第 2 轮失败：第 1 轮已完整回传的 tool_calls+tool 必须留在历史里
 * （硬件已真动过，抹掉等于骗模型什么都没发生），只丢掉第 2 轮的半截记录。 */
TH_TEST(agentloop_transport_error_after_completed_round_keeps_it) {
	FakeTransport transport;
	transport.script.push_back(callResponse("echo", "{\"x\":1}"));	/* 轮 1：完整 */
	/* 轮 2 的请求会在 complete() 里抛（脚本用尽），此时第 1 轮的记录必须已入历史 */
	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK_THROWS_MSG(std::runtime_error, "脚本用尽", session.runTurn("任务"));

	/* system + user + assistant(tool_calls) + tool —— 第 1 轮四件套全在 */
	TH_CHECK(session.history().size() == 4);
	TH_CHECK(session.history()[2].role == "assistant");
	TH_CHECK(session.history()[2].toolCalls.size() == 1);
	TH_CHECK(session.history()[3].role == "tool");
	TH_CHECK(session.history()[3].toolCallId == session.history()[2].toolCalls[0].id);
	/* 且**没有**半截的第二轮痕迹 */
	TH_CHECK(!historyContains(session, "脚本用尽"));
}

/* 最终回答里的 provider 附加字段必须随消息留在历史（下一轮带 tools 的请求要完整
 * 回传，否则 DeepSeek 400）。loop 级原先零覆盖——一次性 --task 永远不会再发第二轮，
 * 只有 REPL 的第二条任务才看得到，所以这里用第二个任务当观察窗。 */
TH_TEST(agentloop_final_answer_extras_are_kept_in_history) {
	FakeTransport transport;
	ChatResponse first = answerResponse("第一次");
	first.extras = parseJson("{\"reasoning_content\":\"首轮思考\"}");
	transport.script.push_back(first);
	transport.script.push_back(answerResponse("第二次"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("A") == "第一次");
	TH_CHECK(session.runTurn("B") == "第二次");

	/* 第二轮请求的历史里，第一条 assistant 必须带着 extras */
	const auto& second = transport.requests[1];
	bool found = false;
	for (const ChatMessage& m : second) {
		if (m.role != "assistant") { continue; }
		if (m.content != "第一次") { continue; }
		const Json* rc = m.extras.find("reasoning_content");
		TH_CHECK_MSG(rc != nullptr && rc->isStr() && rc->getValue() == "首轮思考",
			"最终回答的 extras 丢了：下一轮带 tools 的请求会因此被 provider 400");
		found = true;
	}
	TH_CHECK_MSG(found, "没在第二轮请求里找到第一轮的 assistant 消息");
}

/* ---- AG-4：任务内聚合体积预算 ----
 * 背景：maxHistoryBytes 只裁「已完结的旧任务块」，且最新块无条件保留——于是
 * **正在进行的任务**没有任何体积上限，模型只要多调几次工具就能把请求体顶穿
 * 网关限额；一旦被 413/400 拒绝，那块历史既不在回滚范围内也裁不掉，REPL 从此
 * 每条任务都以同一个请求体失败（与 AG-1 同一失效形态）。 */

/* 超预算的那一次结果不进历史，换成说明；任务本身继续推进（不升级成任务失败） */
TH_TEST(agentloop_task_budget_degrades_oversized_tool_result) {
	FakeTransport tr;
	tr.script = {callResponse("big", "{}"), callResponse("big", "{}"), answerResponse("结束")};
	const AgentToolRegistry tools = bigTools(50);

	AgentOptions opts;
	opts.maxTurns = 4;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	/* 80：第一次 50 字节入账后剩 30，第二次 50 装不下（判据是 > 上限，
	 * 恰好等于上限仍算装得下——所以这里别用 100 去踩边界） */
	opts.maxTaskToolResultBytes = 80;
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("任务") == "结束");
	TH_CHECK(tr.requests.size() == 3);	/* 预算超了也要让模型有机会收尾 */

	/* 最后一次请求里：第一条 tool 内容是原文（50 字节 x），第二条是预算说明 */
	const auto& msgs = tr.requests.back();
	std::vector<std::string> toolContents;
	for (const auto& m : msgs) {
		if (m.role == "tool") { toolContents.push_back(m.content); }
	}
	TH_CHECK_MSG(toolContents.size() == 2, "应有两条 tool 消息，实际 "
		+ std::to_string(toolContents.size()));
	TH_CHECK_MSG(toolContents[0] == std::string(50, 'x'),
		"第一条应原样入历史（预算内），实际前 40 字节: " + toolContents[0].substr(0, 40));
	TH_CHECK_MSG(toolContents[1].find("未注入上下文") != std::string::npos,
		"第二条应是预算说明，实际: " + toolContents[1].substr(0, 80));
	/* 说明必须如实交代「工具已执行」（副作用已发生，谎称没执行会误导模型） */
	TH_CHECK_MSG(toolContents[1].find("已执行") != std::string::npos,
		"预算说明未交代工具已执行: " + toolContents[1].substr(0, 120));
	/* 不许出现半截结果（截断比没有更危险：模型会以为看到的是完整内容） */
	TH_CHECK_MSG(toolContents[1].find(std::string(20, 'x')) == std::string::npos,
		"预算说明里混进了半截结果");
}

/* 预算按**任务**清零：上一个任务用尽不影响下一个任务 */
TH_TEST(agentloop_task_budget_resets_per_task) {
	FakeTransport tr;
	tr.script = {
		callResponse("big", "{}"), answerResponse("第一轮结束"),
		callResponse("big", "{}"), answerResponse("第二轮结束"),
	};
	const AgentToolRegistry tools = bigTools(50);

	AgentOptions opts;
	opts.maxTurns = 3;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskToolResultBytes = 100;
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("任务一") == "第一轮结束");
	TH_CHECK(s.runTurn("任务二") == "第二轮结束");

	/* 第二个任务的第一条工具结果必须仍是原文（预算是任务内量，不是会话量） */
	const auto& msgs = tr.requests.back();
	std::size_t seen = 0;
	bool secondTaskOriginal = false;
	for (const auto& m : msgs) {
		if (m.role != "tool") { continue; }
		++seen;
		if (seen == 2) { secondTaskOriginal = (m.content == std::string(50, 'x')); }
	}
	TH_CHECK_MSG(secondTaskOriginal,
		"第二个任务的首条工具结果应原样入历史（预算未按任务清零？）");
}

/* 图片预算独立计账：文本没超但图片超了，也要降级（且说明区分是图片超限） */
TH_TEST(agentloop_task_budget_covers_images) {
	FakeTransport tr;
	tr.script = {callResponse("shoot", "{}"), callResponse("shoot", "{}"), answerResponse("结束")};
	const AgentToolRegistry tools = imageTools(64);

	AgentOptions opts;
	opts.maxTurns = 4;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskToolResultBytes = 1024 * 1024;	/* 文本预算刻意给大 */
	opts.maxTaskImageBytes = 100;			/* 只装得下一张 */
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("拍照") == "结束");
	const auto& msgs = tr.requests.back();

	std::size_t imgMsgs = 0;
	for (const auto& m : msgs) {
		if (m.role == "user" && !m.images.empty()) { ++imgMsgs; }
	}
	TH_CHECK_MSG(imgMsgs == 1, "超预算的图片不该被注入（应有 1 条图片消息，实际 "
		+ std::to_string(imgMsgs) + " 条）");

	/* 降级说明应点名是**图片**超限，而不是含混的「超预算」 */
	bool hasImgNote = false;
	for (const auto& m : msgs) {
		if (m.role == "tool" && m.content.find("图片累计体积") != std::string::npos) {
			hasImgNote = true;
		}
	}
	TH_CHECK_MSG(hasImgNote, "应有点名「图片累计体积」的降级说明");
}

/* 预算为 0 = 不限制（与 maxHistoryBytes 同口径，便于需要时整体放开） */
TH_TEST(agentloop_task_budget_zero_means_unlimited) {
	FakeTransport tr;
	tr.script = {callResponse("big", "{}"), answerResponse("结束")};
	const AgentToolRegistry tools = bigTools(4096);

	AgentOptions opts;
	opts.maxTurns = 3;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskToolResultBytes = 0;
	opts.maxTaskImageBytes = 0;
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("任务") == "结束");
	const auto& msgs = tr.requests.back();
	bool original = false;
	for (const auto& m : msgs) {
		if (m.role == "tool" && m.content == std::string(4096, 'x')) { original = true; }
	}
	TH_CHECK_MSG(original, "预算为 0 时应原样入历史（不限制）");
}
