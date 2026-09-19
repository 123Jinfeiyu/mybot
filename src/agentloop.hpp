#pragma once
/* agentloop.hpp —— 智能体工具循环
 *
 * 流程：user 任务 → 模型返回 tool_calls → 派发工具 → 结果作为 tool 消息回传
 * → 再问模型 …… 直到模型给出纯文本最终回答，或达到轮次上限。
 *
 * 只依赖 ChatTransport / AgentToolRegistry 两个抽象，因此可以完全离线单测
 * （脚本化传输 + 真实工具层）。会话历史跨轮保留，REPL 模式下机器人可以
 * 「记得」之前做过什么；历史按字节预算自动裁剪（trimHistory，见
 * maxHistoryBytes），老任务块连同其中的 base64 图片一起淘汰——否则长会话
 * 的请求体与内存无界增长，而 base64 图片是主要体积来源。
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
	/* 会话历史的字节预算（估重，见 agentloop.cpp 的 messageWeight），超出时
	 * 在下一轮任务开始前按整块裁掉最老的任务块；0 = 不裁剪。4 MiB：base64
	 * 图片与 64KB 截断的工具输出是历史体积主项，该预算把线上请求体稳稳压在
	 * 常见网关限额之下；仅约束「已完结」的旧轮块，单块超预算不封顶
	 * （正在进行的对话不能裁空，否则无法推进）。 */
	std::size_t maxHistoryBytes = 4 * 1024 * 1024;
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

	/* 历史按 maxHistoryBytes 预算裁剪：以「任务消息」（user 且非图片载体）为
	 * 界整块裁剪最老的块，system 提示词与最新一块无条件保留。runTurn 开头
	 * 调用，push 新任务消息之前。 */
	void trimHistory();
};

} // namespace agent
} // namespace bot
