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

std::string AgentSession::runTurn(const std::string& userTask) {
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

			for (const auto& c : calls) {
				std::string result;
				try {
					Json args = parseToolArguments(c.arguments);
					result = tools_.execute(c.name, args);
				} catch (const std::exception& e) {
					result = std::string("工具参数错误: ") + e.what();
				}
				if (opts_.verbose) {
					log("[工具] " + c.name + "\n" + indentLines(result, 2));
				}
				ChatMessage tool;
				tool.role = "tool";
				tool.content = result;
				tool.toolCallId = c.id;
				messages_.push_back(std::move(tool));
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
