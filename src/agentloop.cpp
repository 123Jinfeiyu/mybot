#include "agentloop.hpp"

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
	: transport_(transport), tools_(tools), opts_(std::move(opts)) {
	ChatMessage sys;
	sys.role = "system";
	sys.content = opts_.systemPrompt;
	messages_.push_back(std::move(sys));
}

void AgentSession::log(const std::string& text) const {
	std::ostream& os = opts_.log != nullptr ? *opts_.log : std::cerr;
	os << text << "\n";
}

/* ---- trimHistory：历史按字节预算裁剪（REPL 长会话）----
 * 单位是「任务块」：一条任务消息（user 且不带图片——图片消息属于它前面
 * 的任务块）直到下一条任务消息之前的全部消息。整块裁剪保证 assistant 的
 * tool_calls 与其 tool 消息、以及夹在 tool 消息之后的图片 user 消息不被
 * 拆散——半截序列发给服务端会被 400 拒绝。
 * 老块连同其中的 base64 图片一起释放：图片是历史体积主项（一张照片数百 KB），
 * 只按条数裁剪挡不住它们。 */
namespace {

/* 估算一条消息的线上体积（下界：不计 JSON 语法开销，正文/调用参数/图片/extras
 * 占绝对大头）。extras 当前只装 reasoning_content 一类文本，整体串行化计入。 */
std::size_t messageWeight(const ChatMessage& m) {
	std::size_t n = m.content.size() + m.toolCallId.size();
	for (const ToolCall& c : m.toolCalls) {
		n += c.id.size() + c.name.size() + c.arguments.size();
	}
	for (const std::string& img : m.images) { n += img.size(); }
	if (!m.extras.isNull()) { n += m.extras.toString().size(); }
	return n;
}

} // namespace

void AgentSession::trimHistory() {
	if (opts_.maxHistoryBytes == 0 || messages_.size() <= 1) { return; }

	/* 任务块起点（messages_[0] 恒为 system，跳过） */
	std::vector<std::size_t> starts;
	for (std::size_t i = 1; i < messages_.size(); ++i) {
		const ChatMessage& m = messages_[i];
		if (m.role == "user" && m.images.empty()) { starts.push_back(i); }
	}
	if (starts.empty()) { return; }	/* 防御：历史理应至少含一次任务 */

	std::vector<std::size_t> blockWeight(starts.size(), 0);
	for (std::size_t j = 0; j < starts.size(); ++j) {
		std::size_t end = (j + 1 < starts.size()) ? starts[j + 1] : messages_.size();
		for (std::size_t k = starts[j]; k < end; ++k) { blockWeight[j] += messageWeight(messages_[k]); }
	}

	/* 从最新块向旧累计；最新块无条件保留（不能把对话裁空），其余装得下就留 */
	std::size_t keep = starts.size() - 1;
	std::size_t acc = blockWeight[keep];
	while (keep > 0 && acc + blockWeight[keep - 1] <= opts_.maxHistoryBytes) {
		--keep;
		acc += blockWeight[keep];
	}
	if (keep == 0) { return; }	/* 全部塞得下，无事可做 */

	std::size_t dropped = starts[keep] - 1;	/* system 之后、最新保留块之前的条数 */
	messages_.erase(messages_.begin() + 1, messages_.begin() + starts[keep]);
	if (opts_.verbose) {
		log("[历史] 超出字节预算，已裁掉最早的 " + std::to_string(dropped)
			+ " 条消息（含其中的图片）；更早的任务上下文不再可见");
	}
}

std::string AgentSession::runTurn(const std::string& userTask) {
	trimHistory();
	const std::size_t base = messages_.size();

	ChatMessage user;
	user.role = "user";
	user.content = userTask;
	messages_.push_back(std::move(user));

	try {
		for (int turn = 0; turn < opts_.maxTurns; ++turn) {
			/* D4：SIGINT/SIGTERM 置位后不再发起新的模型/工具轮，
			 * 让主流程尽快走 deactivate 清理链 */
			if (stopRequested()) {
				throw std::runtime_error("执行被中断");
			}

			ChatResponse resp = transport_.complete(messages_, tools_.specs());

			if (!resp.hasToolCalls()) {
				ChatMessage asst;
				asst.role = "assistant";
				asst.content = resp.content;
				/* provider 附加字段（如 DeepSeek 的 reasoning_content）必须跟着
				 * 这条 assistant 消息留在历史里——下一轮带 tools 的请求要把它们
				 * 完整回传，否则 DeepSeek 直接 400。 */
				asst.extras = resp.extras;
				messages_.push_back(std::move(asst));
				return resp.content;
			}

			/* 模型常先给一句过渡语再发 tool_calls；过渡语只进日志，
			 * 最终回答只由「无工具调用」的那一轮决定 */
			if (opts_.verbose && !resp.content.empty()) {
				log("[模型] " + resp.content);
			}

			/* 个别流式/兼容实现会漏 tool_call id：这里合成占位 id，
			 * 保证 assistant.tool_calls 与后续 tool.tool_call_id 一一对上 */
			std::vector<ToolCall> calls = resp.toolCalls;
			for (std::size_t i = 0; i < calls.size(); ++i) {
				if (calls[i].id.empty()) {
					calls[i].id = "call_" + std::to_string(turn) + "_" + std::to_string(i);
				}
			}

			ChatMessage asst;
			asst.role = "assistant";
			asst.content = resp.content;
			asst.toolCalls = calls;
			asst.extras = resp.extras;	/* 同上：附加字段随历史逐轮回传 */
			messages_.push_back(std::move(asst));

			/* 本轮工具产出的图片：必须等**全部** tool 消息回传之后再注入。
			 * OpenAI 协议要求 assistant 的 tool_calls 之后紧跟对应的 tool 消息，
			 * 中间插 user 消息会被判 400；tool 消息本身又只允许文本。 */
			std::vector<std::string> images;
			for (const auto& c : calls) {
				AgentToolResult result;
				try {
					Json args = parseToolArguments(c.arguments);
					result = tools_.execute(c.name, args);
				} catch (const std::exception& e) {
					result = {std::string("工具参数错误: ") + e.what(), ""};
				}
				if (opts_.verbose) {
					std::string shown = result.text;
					if (!result.imageDataUrl.empty()) {
						shown += "\n[返回图片 " + std::to_string(result.imageDataUrl.size())
							+ " 字节，已注入下一轮对话]";
					}
					log("[工具] " + c.name + "\n" + indentLines(shown, 2));
				}
				ChatMessage tool;
				tool.role = "tool";
				tool.content = result.text;
				tool.toolCallId = c.id;
				messages_.push_back(std::move(tool));
				if (!result.imageDataUrl.empty()) {
					images.push_back(result.imageDataUrl);
				}
			}
			if (!images.empty()) {
				ChatMessage img;
				img.role = "user";
				img.content = "以上工具返回的图片如下，请结合画面内容继续。";
				img.images = std::move(images);
				messages_.push_back(std::move(img));
			}
		}

		throw std::runtime_error("达到工具轮次上限（" + std::to_string(opts_.maxTurns)
			+ " 轮）仍未得到最终回答；可调大 --llm-max-turns，或把任务拆小");
	} catch (...) {
		messages_.resize(base);	/* 回滚本轮，保持历史是完整可发送的序列 */
		throw;
	}
}

} // namespace agent
} // namespace bot
