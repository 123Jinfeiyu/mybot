#pragma once
/* agentloop.hpp —— 智能体工具循环（编排层，重构 A1）
 *
 * 流程：user 任务 → 模型返回 tool_calls → 派发工具 → 结果作为 tool 消息回传
 * → 再问模型 …… 直到模型给出纯文本最终回答，或达到轮次上限。
 *
 * 只依赖 ChatTransport / AgentToolRegistry 两个抽象，因此可以完全离线单测
 * （脚本化传输 + 真实工具层）。会话历史跨轮保留，REPL 模式下机器人可以
 * 「记得」之前做过什么；历史按字节预算自动裁剪（HistoryManager::trim），
 * 老任务块连同其中的 base64 图片一起淘汰——否则长会话的请求体与内存无界
 * 增长，而 base64 图片是主要体积来源。
 *
 * 职责拆分（2026-09-22 重构 A1，行为逐字不变）：
 *   HistoryManager（agenthistory）—— 历史所有权、字节估算、任务块整块裁剪；
 *   TaskBudget     （agentbudget） —— 任务内文本/图片预算与降级说明（AG-4/21）；
 *   DispatchGate   （agentdispatch）—— 策略确认 → 执行 → 审计的固定顺序单点；
 *   AgentSession   （本文件）     —— runTurn 编排、轮次上限、complete 边界回滚。
 */

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "agentbudget.hpp"
#include "agentdispatch.hpp"
#include "agenthistory.hpp"
#include "agentproto.hpp"
#include "agenttools.hpp"

namespace bot {
namespace agent {

struct AgentOptions {
	int maxTurns = 8;		/* 单次任务的最大模型调用轮数 */
	bool verbose = true;		/* 工具调用与结果打印到日志流 */
	std::ostream* log = nullptr;	/* 空 = std::cerr */
	std::string systemPrompt;	/* 系统提示词（含机器人结构与脚本清单） */
	/* 会话历史的字节预算（估重，见 agenthistory.cpp 的 messageWeight），超出时
	 * 在下一轮任务开始前按整块裁掉最老的任务块；0 = 不裁剪。4 MiB：base64
	 * 图片与 64KB 截断的工具输出是历史体积主项；仅约束「已完结」的旧轮块，
	 * 单块超预算不封顶（正在进行的对话不能裁空，否则无法推进）。
	 * 口径（C4，2026-09-22）：记账不含 system 提示词（另有 64KiB 独立上限）
	 * 与工具 schema（注册期固定），估算本身是线上字节下界——线上请求体
	 * 真实总量略高于该预算（≈ 预算 + system + schema），量级仍远低于常见
	 * 网关限额。 */
	std::size_t maxHistoryBytes = 4 * 1024 * 1024;
	/* —— 单轮工具调用数上限（2026-09-21 审计第 4 轮）——
	 * maxTurns 只管「模型回合数」，而模型单个响应里可以带任意多条 tool_calls，
	 * 旧实现对它们**无界顺序执行**——对物理机器人，一次返回几十条调用就全部
	 * 动作，既烧轮次预算也烧历史体积。此上限在 executeToolRound 派发前把
	 * 多出来的调用一律折叠成一条 tool 回传说明（模型据此知道被截断），
	 * 不给硬件一次性下发失控数量的动作。0 = 不限（为旧行为兜底）。 */
	int maxCallsPerRound = 16;
	/* ---- 任务内聚合预算（2026-09-21 审计 AG-4）----
	 * maxHistoryBytes 只管「已完结的旧任务块」，且最新块无条件保留——于是
	 * **正在进行的任务**没有任何体积上限：模型只要多调几次工具，就能在一个
	 * 任务里把 maxTurns × 每轮调用数 × 单次上限 的字节灌进请求体（capture_image
	 * 单张就 2 MiB base64）。一旦网关以 413/400 拒绝，那块历史既不在回滚范围内、
	 * 也不能被裁掉（trim 不裁最新块），REPL 从此每条任务都以同一个请求体失败
	 * ——与 AG-1 同一失效形态。这两条是**任务内**的上限，超限时把该次工具结果
	 * 换成一句说明（见 TaskBudget::admit），模型据此知道「这一步没拿到数据、
	 * 换小一点的步子」，而不是让整个会话进入不可恢复状态。0 = 不限制。 */
	std::size_t maxTaskToolResultBytes = 2 * 1024 * 1024;
	std::size_t maxTaskImageBytes = 4 * 1024 * 1024;
	/* 审计轨迹（--audit-log，默认空 = 关闭）。挂在 AgentOptions 上而非工具层，
	 * 因为唯一漏斗是派发点（工具层不知道轮次，也兜不住参数解析失败那条路径）；
	 * shared_ptr 便于配置与测试共享同一实例。 */
	std::shared_ptr<AuditLog> audit;
	/* SAN-9（2026-09-22）：审计的**会话边界**与**模型轮次**记录需要几个只读事实。
	 * 它们是"这一次会话到底跑的是什么"的答案，专供 session 记录使用；
	 * 空值表示未提供（离线夹具手搓 AgentOptions 时不必填）。 */
	std::string auditDriver;	/* 协议驱动表名 */
	std::string auditModel;		/* 模型名（空 = 用驱动缺省） */
	std::string auditEndpoint;	/* 接入点（写入时由 AuditLog 去掉 query/userinfo） */
	std::string auditKeySource;	/* API key 来源枚举名（只报来源，不报值） */
	/* 策略钩子（--agent-policy，P3；默认空 = 现状，全部放行）。命中
	 * require_confirm 的工具在派发前先过外部确认程序（退出 0 = 放行），
	 * 拒绝/超时折成一句工具回执——模型知道「没执行、为什么」，循环继续。
	 * 同样挂派发漏斗：确认发生与否取决于工具名+参数，工具层与轮次层都不合适。 */
	std::shared_ptr<const AgentPolicy> policy;
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

	/* 清空会话历史（C1，REPL /clear）：只保留 messages_[0] 的 system 提示词，
	 * 其余（任务、工具回传、图片、最终回答）全部丢弃。硬件**不**释放——与
	 * 「无记忆持久化」的既有边界一致：上下文被一次错误结论污染后，此前唯一
	 * 出路是重启进程，而重启会丢掉硬件常驻状态；/clear 让「忘掉对话」与
	 * 「保住上电状态」解耦。任务内预算计数随下一轮 runTurn 开头的 resetTask
	 * 归零，无需在此处理。 */
	void clearHistory() {
		if (history_.messages().size() > 1) {
			history_.messages().resize(1);
		}
	}

	const std::vector<ChatMessage>& history() const { return history_.messages(); }

	/* SAN-9：会话结束记录（status = "end" / "error"）。由会话外壳在退出路径上
	 * 显式调用——它比"析构时补一条"更可靠：析构可能发生在审计日志已关闭之后，
	 * 而且"会话怎么结束的"需要调用方才知道的退出原因（正常 / 异常 + 原因）。 */
	void writeSessionEnd(const char* status, const std::string& error = "") {
		writeSessionRecord(status, error);
	}

private:
	ChatTransport& transport_;
	const AgentToolRegistry& tools_;
	AgentOptions opts_;
	/* 会话开始记录只写一次（惰性：第一条记录之前补写） */
	bool sessionStartWritten_ = false;

	HistoryManager history_;
	TaskBudget budget_;
	DispatchGate gate_;
	/* 当前任务已注入的工具结果/图片字节数在 budget_（AG-4）。每轮任务开始时
	 * 清零——预算约束的是**单个任务**的上下文体积，跨任务由 maxHistoryBytes 管。 */

	void log(const std::string& text) const;

	/* ---- SAN-9：审计的会话边界与模型轮次 ----
	 * writeSessionRecord：一条 kind="session" 记录（start / end / error）。
	 * 首次写任何记录之前自动补 start（惰性），所以调用方不必关心顺序——
	 * 一次在模型调用阶段就失败的会话，日志里**也会有** start 与那条 model 记录
	 * （此前那种会话一条记录都没有，而那恰恰是最需要复盘的时候）。
	 * writeModelRecord：一条 kind="model" 记录（耗时/字节/尝试次数/token 用量）。 */
	void writeSessionRecord(const char* status, const std::string& error = "");
	void writeModelRecord(int turn, const ChatResponse& resp, long long ms, bool ok,
		const std::string& error);

	/* 派发单个工具调用 → 一条 tool 消息：DispatchGate（策略/执行/审计）
	 * → TaskBudget（任务内预算判定）→ verbose 日志 → 写 tool 消息。
	 * 返回该结果携带的图片 data URL（无图片时为空串）。 */
	std::string dispatchOneCall(const ToolCall& call, int turn);

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

/* ---- 历史必须能被服务端接受（SAN-1，2026-09-22）----
 * 「assistant.tool_calls 与 tool.tool_call_id 一一对应」此前只是注释里的约定，
 * 于是单轮截断通知复用最后一条已派发调用的 id 时没有任何东西会报错——
 * 而服务端会以 400 `tool_call_id 'x' answered more than once` 拒绝，
 * 且那条消息落在**不可回滚**的历史里（该轮派发已完成、回滚边界已越过），
 * 此后每个任务都重发同一段历史、拿同一个 400，只有 /clear 或重启能恢复。
 *
 * 本函数把那条约定变成**可执行判据**，供用例直接调用：
 *   ① 每个 tool.toolCallId 必须出现在某条 assistant.toolCalls 里（无孤儿）；
 *   ② 同一个 id 只能被回答**一次**（这条正是 SAN-1 的正判据）；
 *   ③ 某条 assistant 声明的每个工具调用都必须被回答过（无漏答）。
 * 与 scripts/fake_llm_server.py 的 protocol_error 是同一套判据的两份实现
 * （C++ 侧供单测、Python 侧供 E2E）——改一处必须改另一处。
 * 返回空串 = 合法；否则返回人话原因。 */
std::string historyProtocolViolation(const std::vector<ChatMessage>& history);

} // namespace agent
} // namespace bot
