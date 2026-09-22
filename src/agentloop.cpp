#include "agentloop.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "util.hpp"

namespace bot {
namespace agent {

namespace {

/* 多行文本统缩进，便于 stderr 里的工具日志成块可读 */
std::string indentLines(const std::string& s, std::size_t n) {
	std::string pad(n, ' ');
	std::string out;
	std::size_t pos = 0;
	while (pos < s.size()) {
		out += pad;
		std::size_t nl = s.find('\n', pos);
		if (nl == std::string::npos) {
			out += s.substr(pos);
			break;
		}
		out += s.substr(pos, nl - pos + 1);
		pos = nl + 1;
	}
	return out;
}

} // namespace

AgentSession::AgentSession(ChatTransport& transport, const AgentToolRegistry& tools,
		AgentOptions opts)
	: transport_(transport), tools_(tools), opts_(std::move(opts)),
	history_(opts_.systemPrompt),
	budget_{opts_.maxTaskToolResultBytes, opts_.maxTaskImageBytes},
	gate_(tools_, opts_.policy, opts_.audit) {}

void AgentSession::log(const std::string& text) const {
	std::ostream& os = opts_.log != nullptr ? *opts_.log : std::cerr;
	os << text << "\n";
}

std::string AgentSession::runTurn(const std::string& userTask) {
	history_.trim(opts_.maxHistoryBytes, opts_.verbose,
		[this](const std::string& t) { log(t); });
	/* 任务内聚合预算按任务清零（AG-4）：它约束的是「这一个任务往上下文里灌了
	 * 多少字节」，跨任务的体积由 HistoryManager 的 maxHistoryBytes 管。 */
	budget_.resetTask();
	const std::size_t base = history_.messages().size();

	/* 本轮里最后一个「可直接发给服务端」的边界：起点（只有这条 user 消息）算完整，
	 * 此后每当一条 assistant 的 tool_calls 拿到全部 tool 回传（连同其后的图片消息）
	 * 就推进一格。异常时只丢弃边界之后写入的部分——需要回滚的从来不是「本轮」，
	 * 而是「半截的 tool_calls 序列」（服务端会 400 拒绝那种）。 */
	std::size_t complete = base;

	ChatMessage user;
	user.role = "user";
	user.content = userTask;
	history_.messages().push_back(std::move(user));

	try {
		for (int turn = 0; turn < opts_.maxTurns; ++turn) {
			/* D4：SIGINT/SIGTERM 置位后不再发起新的模型/工具轮，
			 * 让主流程尽快走 deactivate 清理链 */
			if (stopRequested()) {
				throw std::runtime_error("执行被中断");
			}

			/* 取数：一次模型调用 */
			ChatResponse resp = transport_.complete(history_.messages(), tools_.specs());

			/* 收尾：模型不再要工具 → 记下最终回答并返回 */
			if (!resp.hasToolCalls()) { return appendFinalAnswer(resp); }

			/* 模型常先给一句过渡语再发 tool_calls；过渡语只进日志，
			 * 最终回答只由「无工具调用」的那一轮决定 */
			if (opts_.verbose && !resp.content.empty()) {
				log("[模型] " + resp.content);
			}

			/* 执行：assistant(tool_calls) → 全部 tool 回传 → 图片 */
			executeToolRound(resp, turn);

			/* 到此为止 tool_calls 与 tool 回传一一对上，这段历史可以原样发送 */
			complete = history_.messages().size();
		}

		throw std::runtime_error("达到工具轮次上限（" + std::to_string(opts_.maxTurns)
			+ " 轮）仍未得到最终回答；可调大 --llm-max-turns，或把任务拆小");
	} catch (...) {
		/* 只裁掉半截的尾部：轮次上限这条路径走到这里时 complete 已等于当前长度，
		 * 于是本轮真实执行过的动作留在历史里——硬件状态不会因为「没拿到最终回答」
		 * 而回退，模型也不该被告知什么都没发生。 */
		if (history_.messages().size() > complete) { history_.messages().resize(complete); }
		throw;
	}
}

/* 收尾：无工具调用的那一轮。provider 附加字段（如 DeepSeek 的 reasoning_content）
 * 必须跟着这条 assistant 消息留在历史里——下一轮带 tools 的请求要把它们完整回传，
 * 否则 DeepSeek 直接 400。 */
std::string AgentSession::appendFinalAnswer(const ChatResponse& resp) {
	ChatMessage asst;
	asst.role = "assistant";
	asst.content = resp.content;
	asst.extras = resp.extras;
	history_.messages().push_back(std::move(asst));
	return resp.content;
}

/* 派发单个工具调用：DispatchGate（策略 → 执行 → 审计）→ TaskBudget
 * （必要时按任务内预算降级）→ verbose 日志 → 写 tool 消息。
 * 返回该结果携带的图片 data URL（无图片时为空串）。
 *
 * 预算降级的完整语义（AG-4/AG-21）见 agentbudget.hpp——这里只负责把判定
 * 结果落进历史与日志。本函数不写 assistant 消息、不动 complete 边界：
 * 调用方保证一轮里的 tool 消息与 assistant.tool_calls 一一对应。 */
std::string AgentSession::dispatchOneCall(const ToolCall& call, int turn) {
	AgentToolResult result = gate_.run(call, turn);

	/* ---- 任务内预算判定（AG-4 + AG-21，语义见 agentbudget.hpp）---- */
	std::string text = std::move(result.text);
	std::string image = std::move(result.imageDataUrl);
	budget_.admit(text, image);	/* 超限时 text 已被替换为降级说明、image 清空 */

	if (opts_.verbose) {
		/* 转义工具输出中的日志标记，防止干扰日志解析（util.hpp 单点，C7） */
		std::string shown = escapeLogMarkers(text);
		if (!image.empty()) {
			shown += "\n[返回图片 " + std::to_string(image.size())
				+ " 字节，已注入下一轮对话]";
		}
		log("[工具] " + call.name + "\n" + indentLines(shown, 2));
	}

	ChatMessage tool;
	tool.role = "tool";
	tool.content = std::move(text);
	tool.toolCallId = call.id;
	history_.messages().push_back(std::move(tool));
	return image;
}

/* 执行：一轮带工具调用的模型回合，按协议顺序写历史——
 * assistant(tool_calls) → 每个 tool 的回传（tool_call_id 一一对应）→ 图片消息。
 * 图片必须等**全部** tool 消息回传之后再注入：OpenAI 协议要求 assistant 的
 * tool_calls 之后紧跟对应的 tool 消息，中间插 user 消息会被判 400；tool 消息
 * 本身又只允许文本。工具参数解析失败折成 tool 回传文本，不中断本轮。 */
void AgentSession::executeToolRound(ChatResponse& resp, int turn) {
	/* 个别流式/兼容实现会漏 tool_call id：这里合成占位 id，
	 * 保证 assistant.tool_calls 与后续 tool.tool_call_id 一一对上。
	 * **轮次相关**（call_<turn>_<i>）而不是每轮从 1 重数：跨轮唯一是这个合成
	 * 方案的成立前提——SAN-6 之前驱动会先合出 call_<i>（每轮重来）把它遮住，
	 * 见 modules/lib/llm_wire_openai.lua 的 id 透传注释。 */
	std::vector<ToolCall>& calls = resp.toolCalls;
	for (std::size_t i = 0; i < calls.size(); ++i) {
		if (calls[i].id.empty()) {
			calls[i].id = "call_" + std::to_string(turn) + "_" + std::to_string(i);
		}
	}

	/* 单轮派发上限（审计第 4 轮）：assistant 消息里**只保留**前
	 * maxCallsPerRound 条调用，多余的折叠成说明——不派发它们是安全底线
	 * （不给硬件一次性下发失控数量动作），但必须如实告知模型已经被截断，
	 * 否则模型以为全部执行了。
	 *
	 * SAN-1（2026-09-22）：折叠说明**挂在 assistant 的文本里**，不再新增一条
	 * tool 消息。原实现把它挂到 calls.back().id 上——那是**最后一条已派发**
	 * 调用，两行之前已经拿到过自己的 tool 回传，于是历史里出现两条同
	 * tool_call_id 的 tool 消息（违反本文件上方与 agentloop.hpp 的不变量），
	 * 服务端以 400 answered more than once 拒绝，而该轮派发已完成、回滚边界
	 * 已越过——这条坏消息不可回滚，此后每个任务都以同一个 400 失败。
	 * assistant.content 是唯一既不违反协议、又不与 id 一一对应绑定的位置。 */
	std::size_t excess = 0;
	if (opts_.maxCallsPerRound > 0 && calls.size() > static_cast<std::size_t>(opts_.maxCallsPerRound)) {
		excess = calls.size() - static_cast<std::size_t>(opts_.maxCallsPerRound);
		calls.resize(static_cast<std::size_t>(opts_.maxCallsPerRound));
	}

	ChatMessage asst;
	asst.role = "assistant";
	asst.content = resp.content;
	if (excess > 0) {
		/* 截断说明必须如实告知模型（与 AG-4 同口径「不谎报状态」）。
		 * 文案里的三个数字（总数/已执行/未派发）是模型判断"要不要重发"的依据。 */
		if (!asst.content.empty()) { asst.content += "\n"; }
		asst.content += "本轮模型返回了 " + std::to_string(calls.size() + excess)
			+ " 条工具调用，因超过单轮上限（" + std::to_string(opts_.maxCallsPerRound)
			+ " 条）只执行了前 " + std::to_string(calls.size())
			+ " 条；其余 " + std::to_string(excess)
			+ " 条未被派发，请基于已执行的结果决定下一步";
		/* 截断是安全相关事实，操作者也必须看得见（SAN-8①）：此前它只到达模型，
		 * stderr 与审计轨迹里一个字都没有。 */
		if (opts_.verbose) {
			log("[上限] 本轮 " + std::to_string(calls.size() + excess)
				+ " 条工具调用超过单轮上限 " + std::to_string(opts_.maxCallsPerRound)
				+ " 条，已丢弃 " + std::to_string(excess) + " 条未派发");
		}
	}
	asst.toolCalls = calls;
	asst.extras = resp.extras;	/* 同上：附加字段随历史逐轮回传 */
	history_.messages().push_back(std::move(asst));

	std::vector<std::string> images;
	for (const auto& c : calls) {
		std::string img = dispatchOneCall(c, turn);
		if (!img.empty()) { images.push_back(std::move(img)); }
	}
	if (!images.empty()) {
		ChatMessage img;
		img.role = "user";
		img.content = "以上工具返回的图片如下，请结合画面内容继续。";
		img.images = std::move(images);
		history_.messages().push_back(std::move(img));
	}
}

/* 历史协议自洽判据（SAN-1）。语义与理由见 agentloop.hpp 的声明处；
 * 这里只做机械核对，不做任何修正——它的用途是让用例能断言"服务端会接受这段历史"。 */
std::string historyProtocolViolation(const std::vector<ChatMessage>& history) {
	/* 已声明的 id：按出现顺序收集，同时记下是被哪条 assistant 消息声明的 */
	std::vector<std::string> declared;
	for (std::size_t i = 0; i < history.size(); ++i) {
		const ChatMessage& m = history[i];
		if (m.role != "assistant") { continue; }
		for (const ToolCall& c : m.toolCalls) {
			if (c.id.empty()) {
				return "messages[" + std::to_string(i)
					+ "]: assistant 声明了一条 id 为空的工具调用（服务端 400）";
			}
			declared.push_back(c.id);
		}
	}

	std::vector<std::string> answered;
	for (std::size_t i = 0; i < history.size(); ++i) {
		const ChatMessage& m = history[i];
		if (m.role != "tool") { continue; }
		if (m.toolCallId.empty()) {
			return "messages[" + std::to_string(i) + "]: tool 消息缺少 tool_call_id（服务端 400）";
		}
		if (std::find(declared.begin(), declared.end(), m.toolCallId) == declared.end()) {
			return "messages[" + std::to_string(i) + "]: tool_call_id '" + m.toolCallId
				+ "' 不匹配任何 assistant 声明的工具调用（孤儿 tool 消息）";
		}
		if (std::find(answered.begin(), answered.end(), m.toolCallId) != answered.end()) {
			return "messages[" + std::to_string(i) + "]: tool_call_id '" + m.toolCallId
				+ "' 被回答了不止一次（服务端 400 answered more than once）";
		}
		answered.push_back(m.toolCallId);
	}

	for (const std::string& id : declared) {
		if (std::find(answered.begin(), answered.end(), id) == answered.end()) {
			return "assistant 声明的工具调用 '" + id + "' 没有得到任何 tool 回传（服务端 400）";
		}
	}
	return "";
}

} // namespace agent
} // namespace bot
