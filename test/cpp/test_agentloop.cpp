#include "harness.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

/* 带固定体积图片的工具（AG-4 图片预算用）。
 * 文案刻意与真 capture_image 逐字一致（`src/agenttools.cpp` 的返回值
 * "已拍摄照片。画面已加入你的上下文。"）——**这是 AG-4′ 能测到的前提**：降级
 * 说明要剥掉的正是这一句成功断言，夹具若换别的文案，用例就退化成自证
 * （实测：第一版夹具写「已拍照」，反向验证时该用例照样绿，抓不到 bug）。 */
AgentToolRegistry imageTools(std::size_t imgBytes) {
	AgentToolRegistry reg;
	reg.add(AgentTool{"shoot", "返回一张图", parseJson("{\"type\":\"object\"}"),
		[imgBytes](const Json&) -> AgentToolResult {
			return {"已拍摄照片。画面已加入你的上下文。",
				std::string("data:image/jpeg;base64,") + std::string(imgBytes, 'A')};
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

/* C7（F6-2）：单轮派发上限的**可达路径**。原先 `calls.empty() ? "call_capped" : …`
 * 是永不可达的兜底（excess > 0 只在 maxCallsPerRound > 0 分支产生，那时 calls 已被
 * 截到 ≥1 条），删除它之后这条路径的语义必须逐条仍成立：截断、如实告知、id 配对。 */
TH_TEST(agentloop_caps_calls_per_round_and_tells_the_model) {
	FakeTransport transport;
	ChatResponse many;
	many.toolCalls.push_back(ToolCall{"c1", "echo", "{}"});
	many.toolCalls.push_back(ToolCall{"c2", "echo", "{}"});
	many.toolCalls.push_back(ToolCall{"c3", "echo", "{}"});
	transport.script.push_back(many);
	transport.script.push_back(answerResponse("ok"));

	int executed = 0;
	AgentToolRegistry tools;
	tools.add(AgentTool{"echo", "回显参数", parseJson("{\"type\":\"object\"}"),
		[&executed](const Json&) -> AgentToolResult {
			++executed;
			return {"echo", ""};
		}});

	AgentOptions o = optsFor(4);
	o.maxCallsPerRound = 2;
	AgentSession session(transport, tools, o);
	TH_CHECK(session.runTurn("任务") == "ok");
	TH_CHECK(executed == 2);	/* 第 3 条**未派发**：上限是安全底线 */

	const auto& h = session.history();
	/* system, user, assistant(截到 2 条 + 截断说明文本), tool(c1), tool(c2), assistant
	 * —— 截断说明不再单独占一条 tool 消息（SAN-1） */
	TH_CHECK(h.size() == 6);
	TH_CHECK(h[2].role == "assistant");
	TH_CHECK(h[2].toolCalls.size() == 2);
	TH_CHECK(h[2].toolCalls[0].id == "c1" && h[2].toolCalls[1].id == "c2");

	TH_CHECK(h[3].role == "tool");
	TH_CHECK(h[3].toolCallId == "c1");
	TH_CHECK(h[4].role == "tool");
	TH_CHECK(h[4].toolCallId == "c2");
	TH_CHECK_MSG(h[2].content.find("3 条") != std::string::npos, h[2].content);
	TH_CHECK_MSG(h[2].content.find("只执行了前 2 条") != std::string::npos, h[2].content);
	TH_CHECK_MSG(h[2].content.find("其余 1 条未被派发") != std::string::npos, h[2].content);
	/* SAN-1（2026-09-22）：截断说明从"新挂一条 tool 消息、复用 calls.back().id"
	 * 改为"追加进 assistant 文本"。旧行为让历史里两条 tool 消息回答同一个
	 * tool_call_id，服务端 400 `answered more than once`，而那条消息落在
	 * **不可回滚**的历史里（该轮派发已完成、回滚边界已越过）——此后每个任务都
	 * 以同一个 400 失败，只有 /clear 或重启能恢复。实测复现见
	 * .tmp/AGENT_NEXT_PLAN_20260922.md 的 SAN-1 一节。 */

	/* 本用例的核心判据：整段历史必须能被服务端接受。
	 * historyProtocolViolation 的判据 ②（同一 id 只回答一次）正是本缺陷的正判据。 */
	TH_CHECK_MSG(historyProtocolViolation(session.history()).empty(),
		historyProtocolViolation(session.history()));

	/* 发给服务端的历史里 assistant.tool_calls 与 tool 回传仍一一对上（否则 400） */
	const std::vector<ChatMessage>& req = transport.requests[1];
	TH_CHECK(req[2].toolCalls.size() == 2);
	for (std::size_t k = 0; k < req[2].toolCalls.size(); ++k) {
		TH_CHECK(req[3 + k].role == "tool");
		TH_CHECK(req[3 + k].toolCallId == req[2].toolCalls[k].id);
	}
	TH_CHECK_MSG(historyProtocolViolation(req).empty(), historyProtocolViolation(req));
}

/* SAN-1：历史协议自洽判据本身必须能抓住四种违规——否则上面那条用例的
 * `violation().empty()` 只是个永远为真的摆设（本项目对"自证要挑能测出问题的
 * 判据"有明确口径）。这里逐条构造违规历史，断言判据真的响。 */
TH_TEST(agentloop_history_protocol_violation_detects_each_break) {
	auto msg = [](const std::string& role) {
		ChatMessage m;
		m.role = role;
		return m;
	};
	auto call = [](const std::string& id) {
		ToolCall c;
		c.id = id;
		c.name = "echo";
		c.arguments = "{}";
		return c;
	};

	/* 合法基线：不报 */
	{
		std::vector<ChatMessage> h;
		h.push_back(msg("system"));
		ChatMessage a = msg("assistant");
		a.toolCalls.push_back(call("c1"));
		h.push_back(a);
		ChatMessage t = msg("tool");
		t.toolCallId = "c1";
		h.push_back(t);
		TH_CHECK_MSG(historyProtocolViolation(h).empty(), historyProtocolViolation(h));
	}
	/* ① 重复回答同一个 id —— 这正是 SAN-1 的缺陷形态 */
	{
		std::vector<ChatMessage> h;
		ChatMessage a = msg("assistant");
		a.toolCalls.push_back(call("c1"));
		h.push_back(a);
		for (int i = 0; i < 2; ++i) {
			ChatMessage t = msg("tool");
			t.toolCallId = "c1";
			h.push_back(t);
		}
		TH_CHECK_MSG(historyProtocolViolation(h).find("不止一次") != std::string::npos,
			historyProtocolViolation(h));
	}
	/* ② 孤儿 tool 消息（id 没有被任何 assistant 声明） */
	{
		std::vector<ChatMessage> h;
		ChatMessage t = msg("tool");
		t.toolCallId = "ghost";
		h.push_back(t);
		TH_CHECK_MSG(historyProtocolViolation(h).find("孤儿") != std::string::npos,
			historyProtocolViolation(h));
	}
	/* ③ 漏答（assistant 声明了却没回传） */
	{
		std::vector<ChatMessage> h;
		ChatMessage a = msg("assistant");
		a.toolCalls.push_back(call("c1"));
		a.toolCalls.push_back(call("c2"));
		h.push_back(a);
		ChatMessage t = msg("tool");
		t.toolCallId = "c1";
		h.push_back(t);
		TH_CHECK_MSG(historyProtocolViolation(h).find("没有得到任何 tool 回传")
			!= std::string::npos, historyProtocolViolation(h));
	}
	/* ④ assistant 声明了空 id（宿主合成逻辑失效时的形态） */
	{
		std::vector<ChatMessage> h;
		ChatMessage a = msg("assistant");
		a.toolCalls.push_back(call(""));
		h.push_back(a);
		TH_CHECK_MSG(historyProtocolViolation(h).find("id 为空") != std::string::npos,
			historyProtocolViolation(h));
	}
	/* ⑤ tool 消息缺少 tool_call_id */
	{
		std::vector<ChatMessage> h;
		ChatMessage a = msg("assistant");
		a.toolCalls.push_back(call("c1"));
		h.push_back(a);
		h.push_back(msg("tool"));
		TH_CHECK_MSG(historyProtocolViolation(h).find("缺少 tool_call_id") != std::string::npos,
			historyProtocolViolation(h));
	}
}

/* SAN-1：无 id 的模型响应经宿主合成后，多轮历史仍自洽，且两个轮次的合成 id
 * **不同**（同一轮内不同调用也不同）——这是"跨轮唯一"的直接判据，
 * 也是 SAN-6 把驱动的 call_<i> 合成去掉之后才真正生效的那条路径。 */
TH_TEST(agentloop_synthesized_ids_stay_unique_across_turns) {
	FakeTransport transport;
	ChatResponse r1;
	r1.toolCalls.push_back(ToolCall{"", "echo", "{}"});	/* provider 漏 id */
	transport.script.push_back(r1);
	ChatResponse r2;
	r2.toolCalls.push_back(ToolCall{"", "echo", "{}"});
	transport.script.push_back(r2);
	transport.script.push_back(answerResponse("ok"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("任务") == "ok");

	const auto& h = session.history();
	TH_CHECK_MSG(historyProtocolViolation(h).empty(), historyProtocolViolation(h));
	/* 两轮的合成 id 必须不同（call_0_0 与 call_1_0） */
	std::vector<std::string> seen;
	for (const ChatMessage& m : h) {
		if (m.role != "tool") { continue; }
		TH_CHECK(std::find(seen.begin(), seen.end(), m.toolCallId) == seen.end());
		seen.push_back(m.toolCallId);
	}
	TH_CHECK(seen.size() == 2);
}

/* SAN-12（2026-09-23）：provider 在同一次响应里返回**重复的 tool_call id** 时，
 * 宿主必须把第二条改写唯一后再写历史——原样写入就是"同一个 id 被回答两次"，
 * 服务端 400 `answered more than once`，而那段历史已越过回滚边界、trim 又不裁
 * 最新块 ⇒ 此后每个任务都以同一个 400 失败（与 SAN-1 同一失效形态）。 */
TH_TEST(agentloop_duplicate_tool_call_ids_are_repaired_and_reported) {
	std::ostringstream log;
	FakeTransport transport;
	ChatResponse r;
	r.toolCalls.push_back(ToolCall{"dup", "echo", "{\"a\":1}"});
	r.toolCalls.push_back(ToolCall{"dup", "echo", "{\"a\":2}"});	/* 重复 id */
	transport.script.push_back(r);
	transport.script.push_back(answerResponse("好了"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4, &log));
	TH_CHECK(session.runTurn("任务") == "好了");

	const auto& h = session.history();
	TH_CHECK_MSG(historyProtocolViolation(h).empty(), historyProtocolViolation(h));

	/* 两条调用都**照常派发**：被改写的只是不透明的 id，模型的意图分毫未动
	 * （若实现选择"丢弃重复的那条"，这里会少一条 tool 回传 → 判据失败） */
	std::vector<std::string> ids;
	for (const ChatMessage& m : h) {
		if (m.role == "tool") { ids.push_back(m.toolCallId); }
	}
	TH_CHECK(ids.size() == 2);
	TH_CHECK(ids.size() == 2 && ids[0] != ids[1]);
	/* 第一条无需改写，保留 provider 原值（只从冲突的第二条开始改写） */
	TH_CHECK(ids[0] == "dup");
	/* 「provider 违反了协议」级别的事实必须让操作者看得见，且**不被 verbose 闸住**
	 * （verbose 缺省即 true，但这条判据不该依赖那个缺省） */
	TH_CHECK_MSG(log.str().find("重复") != std::string::npos, log.str());
}

/* SAN-12：合成 id 的**会话内**唯一性。turn 每轮从 0 重数，故 REPL 的第 2 个任务
 * 在第 0 轮又会合成出 call_0_0——与第 1 个任务的同名，历史里于是有两条回答同一
 * id 的 tool 消息（historyProtocolViolation 判据②命中）。这条**跨任务**路径此前
 * 没有任何判据看着（既有的跨轮用例只在**同一个任务**里跑两轮）。 */
TH_TEST(agentloop_synthesized_ids_stay_unique_across_tasks) {
	FakeTransport transport;
	for (int task = 0; task < 2; ++task) {
		ChatResponse r;
		r.toolCalls.push_back(ToolCall{"", "echo", "{}"});	/* 漏 id → 宿主合成 */
		transport.script.push_back(r);
		transport.script.push_back(answerResponse("ok"));
	}

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("任务一") == "ok");
	TH_CHECK(session.runTurn("任务二") == "ok");

	const auto& h = session.history();
	TH_CHECK_MSG(historyProtocolViolation(h).empty(), historyProtocolViolation(h));
	std::vector<std::string> ids;
	for (const ChatMessage& m : h) {
		if (m.role == "tool") { ids.push_back(m.toolCallId); }
	}
	TH_CHECK(ids.size() == 2);
	/* 第二个任务的合成 id 必须避开第一个任务已用过的那个（追加后缀） */
	TH_CHECK(ids.size() == 2 && ids[0] != ids[1]);
	TH_CHECK_MSG(ids[0] == "call_0_0", "首个合成 id 应保持基线形态，实际 " + ids[0]);
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

TH_TEST(agentloop_clear_keeps_system_only) {
	/* C1（REPL /clear）：清空后只剩 system；清空后再跑新任务，请求里不含旧历史。
	 * 硬件常驻状态不在本层（RobotStruct 不经过 AgentSession），本用例钉住
	 * 「历史归零 + system 保留 + 清空后的请求干净」三个可见面。 */
	FakeTransport transport;
	transport.script.push_back(answerResponse("第一次"));
	transport.script.push_back(callResponse("echo", "{}"));
	transport.script.push_back(answerResponse("清空后"));

	AgentToolRegistry tools = stubTools();
	AgentSession session(transport, tools, optsFor(4));
	TH_CHECK(session.runTurn("A") == "第一次");
	TH_CHECK(session.history().size() == 3);	/* system,user,asst */

	session.clearHistory();
	TH_CHECK(session.history().size() == 1);
	TH_CHECK(session.history()[0].role == "system");
	TH_CHECK(session.history()[0].content == "测试系统提示词");

	/* 清空后跑一轮带工具的任务：请求（第 2 次）只含 system + 新任务，
	 * 旧任务的对话不再可见 */
	TH_CHECK(session.runTurn("B") == "清空后");
	TH_CHECK(transport.requests[1].size() == 2);
	TH_CHECK(transport.requests[1][1].content == "B");

	/* 对空历史重复清空是幂等的安全操作 */
	session.clearHistory();
	TH_CHECK(session.history().size() == 1);
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

TH_TEST(agentloop_history_budget_excludes_system_prompt) {
	/* C4 口径钉子：maxHistoryBytes 的记账**不含** messages_[0]（system）——
	 * system 另有 64KiB 独立上限（agentprompt），且恒不被裁剪，把它计入预算
	 * 只会让「还没装满历史就先裁历史」的假超限。这里 system 50KB、两块各
	 * ~10KB、预算 25KB：按真实口径 20KB ≤ 25KB 不裁；若误把 system 计入
	 * （70KB > 25KB）则会把第 1 块裁掉——用例在两种口径下结果不同，能抓住
	 * 口径回退。工具 schema 同样不在记账内（注册期固定、逐请求重复发送）。 */
	FakeTransport transport;
	transport.script.push_back(answerResponse("ANSWER-1"));
	transport.script.push_back(answerResponse("ANSWER-2"));

	AgentToolRegistry tools = stubTools();
	AgentOptions opts = optsFor(4);
	opts.maxHistoryBytes = 25000;
	opts.systemPrompt = std::string(50000, 'S');
	AgentSession session(transport, tools, opts);
	TH_CHECK(session.runTurn("TASK-1" + std::string(10000, 'x')) == "ANSWER-1");
	TH_CHECK(session.runTurn("TASK-2" + std::string(10000, 'x')) == "ANSWER-2");

	TH_CHECK(session.history().size() == 5);	/* system + 2 块 × (user+asst) */
	TH_CHECK(historyContains(session, "TASK-1"));	/* 旧块未被（假超限）裁掉 */
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

/* ---- AG-4′（第三轮审计）：降级说明不许自相矛盾 ----
 * 图片超限时原实现把 capture_image 的成功文案原样拼进说明，而那句末尾是
 * 「画面已加入你的上下文」——与说明开头的「未注入上下文」直接冲突，而图片
 * 确实没注入。模型读到互斥的两句，按哪一句行事都错。 */
TH_TEST(agentloop_task_budget_image_note_has_no_false_context_claim) {
	FakeTransport tr;
	tr.script = {callResponse("shoot", "{}"), answerResponse("结束")};
	const AgentToolRegistry tools = imageTools(64);

	AgentOptions opts;
	opts.maxTurns = 3;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskImageBytes = 10;	/* 一张都装不下 */
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("拍照") == "结束");
	const auto& msgs = tr.requests.back();
	std::string note;
	for (const auto& m : msgs) {
		if (m.role == "tool" && m.content.find("未注入上下文") != std::string::npos) {
			note = m.content;
		}
	}
	TH_CHECK_MSG(!note.empty(), "应有降级说明");
	TH_CHECK_MSG(note.find("画面已加入你的上下文") == std::string::npos,
		"降级说明不许再出现成功断言（与「未注入」矛盾）: " + note);
	/* 描述仍要给（模型据此知道拍到了什么），但要显式说明画面没进上下文 */
	TH_CHECK_MSG(note.find("画面本身没有进上下文") != std::string::npos, note);
	TH_CHECK_MSG(note.find("已拍摄照片") != std::string::npos,
		"工具的描述性文字要原样保留（模型据此知道拍到了什么）: " + note);
}

/* ---- AG-21：图片额度用尽后必须**明说**，否则模型会一直重试到轮次上限 ----
 * 实测（.tmp/memaudit/out_mid.txt）：一次任务里连拒 3 次，最后以「达到工具轮次
 * 上限」收场——那几轮全是白烧的。图片预算是逐任务单向累加的，说不说都不会恢复，
 * 所以正确做法是把它讲成硬事实并给出唯一出路。 */
TH_TEST(agentloop_task_budget_image_exhaustion_is_terminal_and_stated) {
	FakeTransport tr;
	/* 模型连续三次拍照都不放弃，最后才给结论 */
	tr.script = {callResponse("shoot", "{}"), callResponse("shoot", "{}"),
		callResponse("shoot", "{}"), answerResponse("改给文字结论")};
	const AgentToolRegistry tools = imageTools(64);

	AgentOptions opts;
	opts.maxTurns = 4;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskImageBytes = 100;	/* 只装得下一张 */
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("拍照") == "改给文字结论");

	const auto& msgs = tr.requests.back();
	std::size_t imgMsgs = 0, terminal = 0;
	for (const auto& m : msgs) {
		if (m.role == "user" && !m.images.empty()) { ++imgMsgs; }
		if (m.role == "tool" && m.content.find("额度已经用尽") != std::string::npos) {
			++terminal;
		}
	}
	TH_CHECK_MSG(imgMsgs == 1, "只该注入一张（额度 100 字节）");
	TH_CHECK_MSG(terminal >= 2, "第二、三次拍照都必须收到「额度已用尽」的明示，实际 "
		+ std::to_string(terminal) + " 条");
	/* 说明里要给出唯一出路，否则模型仍会换着法子重试 */
	bool actionable = false;
	for (const auto& m : msgs) {
		if (m.role == "tool" && m.content.find("再调用也不会成功") != std::string::npos) {
			actionable = true;
		}
	}
	TH_CHECK_MSG(actionable, "说明要写清「再调用也不会成功」");
}

/* ---- AG-21：降级说明自身也计入预算 ----
 * 否则「反复超限」可以往历史里无界灌说明文本（每条约 300 字节 × 轮数 × 每轮
 * 调用数）。判据用说明**自己报的账**（文案里有「已用 <n> + 本次 <m> 字节」）：
 * 第二次降级的「已用」必须等于第一条说明的长度——若说明不计入，那里会是 0。
 * 这样既不依赖魔数长度，也不会把「说明被计入」与「载荷被丢弃」两件事混在一起。 */
TH_TEST(agentloop_task_budget_counts_the_degradation_note) {
	FakeTransport tr;
	tr.script = {callResponse("big", "{}"), callResponse("big", "{}"), answerResponse("结束")};
	const AgentToolRegistry tools = bigTools(64);

	AgentOptions opts;
	opts.maxTurns = 3;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskToolResultBytes = 1;	/* 任何结果都装不下 -> 每次都走降级 */
	AgentSession s(tr, tools, opts);

	TH_CHECK(s.runTurn("任务") == "结束");
	std::vector<std::string> notes;
	for (const ChatMessage& m : tr.requests.back()) {
		if (m.role == "tool") { notes.push_back(m.content); }
	}
	TH_CHECK_MSG(notes.size() == 2, "两次调用都应回传（不中断任务），实际 "
		+ std::to_string(notes.size()));
	for (const std::string& c : notes) {
		TH_CHECK_MSG(c.find("未注入上下文") != std::string::npos,
			"应为降级说明: " + c.substr(0, 60));
		TH_CHECK_MSG(c.find(std::string(20, 'x')) == std::string::npos,
			"说明里不许混进载荷");
	}
	/* 关键断言：第二条说明的「已用」== 第一条说明的长度（说明被计入了） */
	const std::size_t firstLen = notes[0].size();
	const std::string marker = "已用 " + std::to_string(firstLen) + " + 本次";
	TH_CHECK_MSG(notes[1].find(marker) != std::string::npos,
		"第二条说明应把第一条说明计入已用（找 '" + marker + "'），实际: "
			+ notes[1].substr(0, 140));
}

/* ============ SAN-4：没执行的调用不得被说成"副作用已发生" ============
 * 缺陷形态（已实测复现）：策略拒绝 / 未知工具 / 参数错误这三类结果**根本没被派发**，
 * 但预算降级对它们一视同仁，把文本换成"…工具本身已执行完毕（副作用已发生）…"
 * ——而同一轮的审计日志写着 ok=false / policy_denied。两条通道给出相反的事实，
 * 模型按"命令已经跑过"继续推进。对物理机器人，谎报状态是安全相关的错误。 */
TH_TEST(agentloop_task_budget_does_not_claim_side_effects_for_denied_calls) {
	/* 用长文案的"未知工具"触发降级：预算调到极小（1 字节）保证必然超限 */
	const auto runWith = [](const std::string& toolName, const char* expectInText) {
		FakeTransport tr;
		tr.script = {callResponse(toolName, "{}"), answerResponse("结束")};
		AgentToolRegistry tools;	/* 空注册表：任何名字都是"未知工具" */
		AgentOptions opts;
		opts.maxTurns = 3;
		opts.verbose = false;
		opts.systemPrompt = "sys";
		opts.maxTaskToolResultBytes = 1;	/* 必然超限 */
		AgentSession s(tr, tools, opts);
		TH_CHECK(s.runTurn("任务") == "结束");
		std::string toolText;
		for (const auto& m : tr.requests.back()) {
			if (m.role == "tool") { toolText = m.content; }
		}
		TH_CHECK_MSG(toolText.find("副作用已发生") == std::string::npos,
			"未执行的调用不得声称副作用已发生，实际: " + toolText.substr(0, 160));
		TH_CHECK_MSG(toolText.find(expectInText) != std::string::npos,
			"原文应原样到达模型（这正是模型要的事实），实际: " + toolText.substr(0, 160));
		/* 明确的"没执行"表述必须在（与降级说明的"已执行"相反的那一侧） */
		TH_CHECK_MSG(toolText.find("未知工具") != std::string::npos,
			"未知工具的裁决文本被改写了: " + toolText.substr(0, 160));
	};

	runWith("no_such_tool", "未知工具 no_such_tool");
}

/* 对照：**执行过但失败**的调用仍然保留"副作用可能已发生"的口径——
 * 脚本跑到一半报错时机器人可能停在半途，抹掉这句话同样是谎报（方向相反）。 */
TH_TEST(agentloop_task_budget_keeps_side_effect_warning_for_executed_failures) {
	FakeTransport tr;
	tr.script = {callResponse("boom", "{}"), answerResponse("结束")};
	AgentToolRegistry tools;
	tools.add(AgentTool{"boom", "总是抛异常的夹具", parseJson("{\"type\":\"object\"}"),
		[](const Json&) -> AgentToolResult {
			throw std::runtime_error("夹具故意失败（执行过了）");
		}});
	AgentOptions opts;
	opts.maxTurns = 3;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	opts.maxTaskToolResultBytes = 1;	/* 必然超限 */
	AgentSession s(tr, tools, opts);
	TH_CHECK(s.runTurn("任务") == "结束");

	std::string toolText;
	for (const auto& m : tr.requests.back()) {
		if (m.role == "tool") { toolText = m.content; }
	}
	TH_CHECK_MSG(toolText.find("未注入上下文") != std::string::npos,
		"执行过的调用超限时仍应降级，实际: " + toolText.substr(0, 160));
	TH_CHECK_MSG(toolText.find("已执行") != std::string::npos,
		"执行过的调用必须保留「已执行」口径（副作用可能已发生），实际: "
			+ toolText.substr(0, 160));
}

/* executed=false 的结果仍然**计入额度**：否则"反复被拒"就成了往历史里
 * 无界灌文本的免费通道（与 AG-21 同一条口径）。 */
TH_TEST(agentloop_task_budget_still_counts_denied_call_bytes) {
	FakeTransport tr;
	tr.script = {callResponse("no_such_tool", "{}"), callResponse("no_such_tool", "{}"),
		answerResponse("结束")};
	AgentToolRegistry tools;
	AgentOptions opts;
	opts.maxTurns = 4;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	/* 第一次"未知工具"文本约 60 字节，限额给 40 → 第二次必然超限。
	 * 若未执行的调用不记账，第二次也会原样放行（额度永远为 0）。 */
	opts.maxTaskToolResultBytes = 40;
	AgentSession s(tr, tools, opts);
	TH_CHECK(s.runTurn("任务") == "结束");

	std::vector<std::string> toolTexts;
	for (const auto& m : tr.requests.back()) {
		if (m.role == "tool") { toolTexts.push_back(m.content); }
	}
	TH_CHECK_MSG(toolTexts.size() == 2, "应有两条 tool 消息");
	/* 两条都必须是"未知工具"原文（没执行 → 不降级），但第二条能被判定超限
	 * 恰恰证明第一条**记了账**。 */
	TH_CHECK_MSG(toolTexts[1].find("未知工具") != std::string::npos,
		"第二条也应是未执行的原裁决文本，实际: " + toolTexts[1].substr(0, 120));
}

/* ================= A1：单轮上限不得破坏协议形状 =================
 * AG-26 第一版把「被截断」写成**额外一条** tool 消息，并复用最后一条已派发
 * 调用的 tool_call_id ⇒ N 个 tool_calls 对上 N+1 条 tool 消息、且有一个 id 出现
 * 两次。严格网关 400；而那块历史已被 runTurn 的 complete 边界认作「可原样发送」、
 * HistoryManager::trim 又不裁最新块 ⇒ REPL 从此每条任务都以同一个请求体失败（AG-4 同款）。
 * 形状判据用产品侧的 historyProtocolViolation（SAN-1 单点），不在测试里另写一份；
 * 出站报文侧另有 Lua 的 llm_wire_openai.check_tool_pairing（A1b）在发包前把门。 */
namespace {

/* 计次工具：记录被真实派发的次数（可选每次调用睡 ms 毫秒，A4 用） */
AgentToolRegistry countingTools(std::shared_ptr<int> hits, int sleepMs = 0) {
	AgentToolRegistry reg;
	reg.add(AgentTool{"count", "计次", parseJson("{\"type\":\"object\"}"),
		[hits, sleepMs](const Json&) -> AgentToolResult {
			++(*hits);
			if (sleepMs > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs)); }
			return {"ok", ""};
		}});
	return reg;
}

ChatResponse manyCalls(int n, const std::string& name = "count") {
	ChatResponse r;
	for (int i = 0; i < n; ++i) {
		r.toolCalls.push_back(ToolCall{"c" + std::to_string(i), name, "{}"});
	}
	return r;
}

} // namespace

TH_TEST(agentloop_round_cap_keeps_tool_call_pairing) {
	FakeTransport tr;
	tr.script = {manyCalls(5), answerResponse("结束")};
	auto hits = std::make_shared<int>(0);
	const AgentToolRegistry tools = countingTools(hits);

	AgentOptions opts = optsFor(4);
	opts.maxCallsPerRound = 2;
	AgentSession s(tr, tools, opts);
	TH_CHECK(s.runTurn("跑五个") == "结束");

	/* 副作用次数受上限约束：派发 2 条，不是 5 条 */
	TH_CHECK_MSG(*hits == 2, "只应派发 2 条，实际 " + std::to_string(*hits));

	/* 核心断言：发出去的报文形状合规（AG-26 第一版在这里是「多一条 + id 重复」） */
	TH_CHECK_MSG(tr.requests.size() == 2, "应有两次模型调用，实际 "
		+ std::to_string(tr.requests.size()));
	const std::string v = historyProtocolViolation(tr.requests.back());
	TH_CHECK_MSG(v.empty(), "协议形状违例: " + v);

	/* assistant 里只留被派发的那 2 条（未派发的不进 tool_calls） */
	const ChatMessage* asst = nullptr;
	for (const auto& m : tr.requests.back()) {
		if (m.role == "assistant" && !m.toolCalls.empty()) { asst = &m; }
	}
	TH_CHECK(asst != nullptr);
	if (asst != nullptr) {
		TH_CHECK_MSG(asst->toolCalls.size() == 2, "assistant.tool_calls 应为 2 条，实际 "
			+ std::to_string(asst->toolCalls.size()));
	}
}

TH_TEST(agentloop_round_cap_note_reaches_model) {
	FakeTransport tr;
	tr.script = {manyCalls(5), answerResponse("结束")};
	auto hits = std::make_shared<int>(0);
	const AgentToolRegistry tools = countingTools(hits);

	AgentOptions opts = optsFor(4);
	opts.maxCallsPerRound = 2;
	AgentSession s(tr, tools, opts);
	TH_CHECK(s.runTurn("跑五个") == "结束");

	/* 截断说明落在**该轮 assistant 的文本里**（SAN-1）：tool 消息只装工具结果，
	 * 说明不借道任何 tool_call_id——借道就会让同一个 id 被回答两次（400）。
	 * 条数与「未被派发」必须如实（AG-4/AG-21 口径：不向模型谎报状态）。 */
	const ChatMessage* asst = nullptr;
	std::vector<std::string> toolContents;
	for (const auto& m : tr.requests.back()) {
		if (m.role == "assistant" && !m.toolCalls.empty()) { asst = &m; }
		if (m.role == "tool") { toolContents.push_back(m.content); }
	}
	TH_CHECK(asst != nullptr);
	if (asst != nullptr) {
		TH_CHECK_MSG(asst->content.find("5 条") != std::string::npos
				&& asst->content.find("3 条") != std::string::npos,
			"说明应给出总条数(5)与未派发条数(3): " + asst->content);
		TH_CHECK_MSG(asst->content.find("只执行了前 2 条") != std::string::npos,
			"说明应点名已执行条数: " + asst->content);
		TH_CHECK_MSG(asst->content.find("未被派发") != std::string::npos,
			"说明应写「未被派发」而不是「执行失败」: " + asst->content);
	}
	/* 工具结果通道保持纯净：两条都是工具原文，没有被追加说明 */
	TH_CHECK_MSG(toolContents.size() == 2,
		"tool 消息应为 2 条（与 tool_calls 一一对应），实际 "
			+ std::to_string(toolContents.size()));
	for (const auto& c : toolContents) {
		TH_CHECK_MSG(c == "ok", "tool 消息应只有工具原文，实际: " + c);
	}
}

TH_TEST(agentloop_round_cap_zero_means_unlimited) {
	FakeTransport tr;
	tr.script = {manyCalls(5), answerResponse("结束")};
	auto hits = std::make_shared<int>(0);
	const AgentToolRegistry tools = countingTools(hits);

	AgentOptions opts = optsFor(4);
	opts.maxCallsPerRound = 0;	/* 逃生口：不限 */
	AgentSession s(tr, tools, opts);
	TH_CHECK(s.runTurn("跑五个") == "结束");
	TH_CHECK_MSG(*hits == 5, "0 = 不限，应派发全部 5 条，实际 " + std::to_string(*hits));
	TH_CHECK(historyProtocolViolation(tr.requests.back()).empty());
}

/* ================= A4：单任务墙钟预算 =================
 * 体积预算（AG-4/AG-21）管的是"往上下文里灌了多少字节"，没有一条管**时间**；
 * 而 agent 模式全程硬件带电常驻。判定点有两个，语义不同，各一条用例。 */

TH_TEST(agentloop_task_deadline_blocks_next_model_call) {
	FakeTransport tr;
	/* 三条脚本：若预算不生效会跑满三轮；生效后只应发出第一次模型调用 */
	tr.script = {manyCalls(1), manyCalls(1), answerResponse("结束")};
	auto hits = std::make_shared<int>(0);
	/* 每条调用睡 1.1s，预算 1s ⇒ 第一轮工具跑完即超预算 */
	const AgentToolRegistry tools = countingTools(hits, 1100);

	AgentOptions opts = optsFor(6);
	opts.maxTaskSeconds = 1;
	AgentSession s(tr, tools, opts);

	/* 判定点①：不再发起新的模型调用，按「达到预算」如实失败 */
	TH_CHECK_THROWS_MSG(std::runtime_error, "墙钟预算", s.runTurn("慢慢跑"));
	TH_CHECK_MSG(tr.requests.size() == 1, "预算耗尽后不应再发模型调用，实际发了 "
		+ std::to_string(tr.requests.size()) + " 次");
	TH_CHECK_MSG(*hits == 1, "应只执行过 1 条调用，实际 " + std::to_string(*hits));

	/* 已执行的动作**不回退**：那一轮的 assistant+tool 仍在历史里（硬件不会倒回去，
	 * 抹掉等于骗模型说什么都没发生——口径同 runTurn 的 catch(...) 注释） */
	const std::string v = historyProtocolViolation(s.history());
	TH_CHECK_MSG(v.empty(), "回滚后历史仍须合规: " + v);
	bool sawTool = false;
	for (const auto& m : s.history()) {
		if (m.role == "tool" && m.content == "ok") { sawTool = true; }
	}
	TH_CHECK_MSG(sawTool, "已执行那一轮的 tool 回执应留在历史里");
}

TH_TEST(agentloop_task_deadline_skips_remaining_calls_with_receipts) {
	FakeTransport tr;
	tr.script = {manyCalls(3), answerResponse("结束")};
	auto hits = std::make_shared<int>(0);
	const AgentToolRegistry tools = countingTools(hits, 1100);

	AgentOptions opts = optsFor(6);
	opts.maxTaskSeconds = 1;
	AgentSession s(tr, tools, opts);
	TH_CHECK_THROWS_MSG(std::runtime_error, "墙钟预算", s.runTurn("跑三个"));

	/* 判定点②：预算在一轮**中间**耗尽 ⇒ 剩余调用不派发（副作用只有 1 次），
	 * 但每条各留一条「未派发」回执——少一条就是服务端 400。 */
	TH_CHECK_MSG(*hits == 1, "只应真实执行 1 条，实际 " + std::to_string(*hits));
	const std::string v = historyProtocolViolation(s.history());
	TH_CHECK_MSG(v.empty(), "一轮内部分未派发时历史仍须一一对应: " + v);

	std::vector<std::string> toolContents;
	for (const auto& m : s.history()) {
		if (m.role == "tool") { toolContents.push_back(m.content); }
	}
	TH_CHECK_MSG(toolContents.size() == 3, "3 个 tool_calls 应有 3 条回执，实际 "
		+ std::to_string(toolContents.size()));
	if (toolContents.size() == 3) {
		TH_CHECK(toolContents[0] == "ok");
		TH_CHECK_MSG(toolContents[1].find("未派发") != std::string::npos,
			"第二条应为未派发回执: " + toolContents[1]);
		TH_CHECK_MSG(toolContents[2].find("未派发") != std::string::npos,
			"第三条应为未派发回执: " + toolContents[2]);
		TH_CHECK_MSG(toolContents[1].find("没有产生任何副作用") != std::string::npos,
			"未派发回执必须说明无副作用: " + toolContents[1]);
	}
}

TH_TEST(agentloop_task_deadline_zero_means_unlimited) {
	FakeTransport tr;
	tr.script = {manyCalls(2), answerResponse("结束")};
	auto hits = std::make_shared<int>(0);
	const AgentToolRegistry tools = countingTools(hits);

	AgentOptions opts = optsFor(4);
	opts.maxTaskSeconds = 0;	/* 逃生口：不限（旧行为） */
	AgentSession s(tr, tools, opts);
	TH_CHECK(s.runTurn("跑两个") == "结束");
	TH_CHECK_MSG(*hits == 2, "0 = 不限，应全部执行，实际 " + std::to_string(*hits));
}
