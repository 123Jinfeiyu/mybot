#pragma once
/* agentloop.hpp —— 智能体工具循环
 *
 * 流程：user 任务 → 模型返回 tool_calls → 派发工具 → 结果作为 tool 消息回传
 * → 再问模型 …… 直到模型给出纯文本最终回答，或达到轮次上限。
 *
 * 只依赖 ChatTransport / AgentToolRegistry 两个抽象，因此可以完全离线单测
 * （脚本化传输 + 真实工具层）。会话历史跨轮保留，REPL 模式下机器人可以
 * 「记得」之前做过什么。
 */

#include <iosfwd>
#include <string>
#include <vector>

#include "agentproto.hpp"
#include "agenttools.hpp"

namespace bot {
namespace agent {

struct AgentOptions {
	int maxTurns = 8;		/* 单次任务的最大模型调用轮数 */
	bool verbose = true;		/* 工具调用与结果打印到日志流 */
	std::ostream* log = nullptr;	/* 空 = std::cerr */
	std::string systemPrompt;	/* 系统提示词（含机器人结构与脚本清单） */
};

class AgentSession {
public:
	AgentSession(ChatTransport& transport, const AgentToolRegistry& tools, AgentOptions opts);

	/* 追加一条 user 消息并跑完工具循环，返回最终回答文本。
	 * 失败（轮次上限 / 传输错误 / 中断）时回滚本轮全部消息后抛出：
	 * 半截的 tool_calls 序列会让下一轮请求被服务端 400 拒绝。 */
	std::string runTurn(const std::string& userTask);

	const std::vector<ChatMessage>& history() const { return messages_; }

private:
	ChatTransport& transport_;
	const AgentToolRegistry& tools_;
	AgentOptions opts_;
	std::vector<ChatMessage> messages_;

	void log(const std::string& text) const;
};

} // namespace agent
} // namespace bot
