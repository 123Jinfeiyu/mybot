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
	/* ---- 任务内聚合预算（2026-09-21 审计 AG-4）----
	 * 上面那条只管「已完结的旧任务块」，且最新块无条件保留——于是**正在进行
	 * 的任务**没有任何体积上限：模型只要多调几次工具，就能在一个任务里把
	 * maxTurns × 每轮调用数 × 单次上限 的字节灌进请求体（capture_image 单张
	 * 就 2 MiB base64）。一旦网关以 413/400 拒绝，那块历史既不在回滚范围内、
	 * 也不能被裁掉（trimHistory 不裁最新块），REPL 从此每条任务都以同一个
	 * 请求体失败——与 AG-1 同一失效形态。
	 * 这两条是**任务内**的上限，超限时把该次工具结果换成一句说明（见
	 * executeToolRound），模型据此知道「这一步没拿到数据、换小一点的步子」，
	 * 而不是让整个会话进入不可恢复状态。0 = 不限制。 */
	std::size_t maxTaskToolResultBytes = 2 * 1024 * 1024;
	std::size_t maxTaskImageBytes = 4 * 1024 * 1024;
};

class AgentSession {
public:
	AgentSession(ChatTransport& transport, const AgentToolRegistry& tools, AgentOptions opts);

	/* 追加一条 user 消息并跑完工具循环，返回最终回答文本。
	 * 失败时只裁掉「半截的 tool_calls 尾部」（那种序列会被服务端 400 拒绝），
	 * 已完整回传的部分保留：传输错误发生在第一条请求上时本轮回到原点，而
	 * 「达到轮次上限」前模型已真实执行过若干动作脚本——硬件不会倒回去，
	 * 把这段历史也抹掉就等于告诉模型「什么都没发生」。 */
	std::string runTurn(const std::string& userTask);

	const std::vector<ChatMessage>& history() const { return messages_; }

private:
	ChatTransport& transport_;
	const AgentToolRegistry& tools_;
	AgentOptions opts_;
	std::vector<ChatMessage> messages_;
	/* 当前任务已注入的工具结果/图片字节数（AG-4）。每轮任务开始时清零——
	 * 预算约束的是**单个任务**的上下文体积，跨任务由 maxHistoryBytes 管。 */
	std::size_t taskToolResultBytes_ = 0;
	std::size_t taskImageBytes_ = 0;

	void log(const std::string& text) const;

	/* 派发单个工具调用 → 一条 tool 消息（内含 AG-4 的任务内预算判定）。
	 * 返回该结果携带的图片 data URL（无图片时为空串）。 */
	std::string dispatchOneCall(const ToolCall& call, int turn);

	/* 历史按 maxHistoryBytes 预算裁剪：以「任务消息」（user 且非图片载体）为
	 * 界整块裁剪最老的块，system 提示词与最新一块无条件保留。runTurn 开头
	 * 调用，push 新任务消息之前。 */
	void trimHistory();

	/* ---- runTurn 的三段拆分（B5/N5，2026-09-21）：取数 / 执行 / 收尾 ----
	 * 回滚边界仍留在 runTurn 的同一个 try 里（它需要同时覆盖三段），
	 * 这里只把两段自成的逻辑搬出去，行为逐字保持。 */

	/* 收尾：无工具调用的那一轮——把这句 assistant 回答写进历史并返回文本。
	 * provider 附加字段（DeepSeek 的 reasoning_content 等）必须随消息留在
	 * 历史里，下一轮带 tools 的请求要完整回传，否则对方直接 400。 */
	std::string appendFinalAnswer(const ChatResponse& resp);

	/* 执行：一轮带工具调用的模型回合，按协议顺序写历史——
	 * assistant(tool_calls) → 每个 tool 的回传（tool_call_id 一一对应）→ 图片消息
	 * （必须最后，见实现注释）。工具参数解析失败折成 tool 回传文本，不中断本轮。 */
	void executeToolRound(ChatResponse& resp, int turn);
};

} // namespace agent
} // namespace bot
