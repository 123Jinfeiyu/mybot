#pragma once
/* agenthistory.hpp —— 会话历史的所有权与字节预算裁剪（重构 A1）
 *
 * 从 AgentSession（agentloop）拆出的第一套职责：历史消息的所有权、
 * 单条消息线上体积估算（messageWeight）与按任务块的整块裁剪（trim）。
 * 拆分前这三者与任务内预算、工具派发、轮次回滚交织在 agentloop.cpp；
 * 独立后可以离线单测裁剪语义（任务块边界、最新块无条件保留等）。
 *
 * 只依赖 agentproto 的 ChatMessage，不认识传输与工具层。
 *
 * 会话持久化（批次 1，2026-09-23）：本文件同时承载会话文件的读写原语
 * （SessionHead / SessionFileWriter / loadSessionFile）——落盘单位与 trim
 * 的裁剪单位同为「任务块」，两套语义共享一处，不会分叉。
 */

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "agentproto.hpp"

namespace bot {
namespace agent {

class HistoryManager {
public:
	/* 构造即写入 system 消息（messages_[0]，恒不被裁剪） */
	explicit HistoryManager(std::string systemPrompt);

	std::vector<ChatMessage>& messages() { return messages_; }
	const std::vector<ChatMessage>& messages() const { return messages_; }

	/* ---- 历史按字节预算裁剪（REPL 长会话）----
	 * 单位是「任务块」：一条任务消息（user 且不带图片——图片消息属于它前面
	 * 的任务块）直到下一条任务消息之前的全部消息。整块裁剪保证 assistant 的
	 * tool_calls 与其 tool 消息、以及夹在 tool 消息之后的图片 user 消息不被
	 * 拆散——半截序列发给服务端会被 400 拒绝。
	 * 老块连同其中的 base64 图片一起释放：图片是历史体积主项（一张照片数百 KB），
	 * 只按条数裁剪挡不住它们。
	 * budget == 0 = 不裁剪；裁剪发生且 verbose 时经 log 打一条人话说明
	 * （文案与拆分前逐字一致）。
	 * 口径（C4，2026-09-22）：budget 记账只覆盖可裁的任务块消息，**不含**
	 * system 提示词（messages_[0]，恒保留，另有 64KiB 上限独立约束）与工具 schema
	 * （注册期固定）；单条体积估算（messageWeight）是线上字节的下界。 */
	void trim(std::size_t budget, bool verbose,
		const std::function<void(const std::string&)>& log);

	/* SAN-10：当前可裁任务块的估算字节（trim 的记账口径——**不含** system
	 * 提示词与工具 schema，与 C4 的诚实口径一致）。供 /status 显示"历史现在多大"，
	 * 而不必等到裁剪发生才知道（此前只有裁剪那一刻打一行日志）。 */
	std::size_t accountedBytes() const;

private:
	std::vector<ChatMessage> messages_;
};

/* ---- 会话持久化（批次 1，2026-09-23）----
 * 会话文件 = JSON Lines 追加写（与 --audit-log 同款惯例），首行会话头，
 * 其后每条消息一行。写入发生在 AgentSession 的 complete 边界（最终回答
 * 已入历史、本轮工具序列协议完整），因此**文件里的每个前缀都是可独立
 * 恢复的完整任务块序列**——半截的 tool_calls 序列永不落盘。
 *
 * 图片**写入期排除**：图片字节（data URL）不落盘，落盘的是占位文本
 * 「[图片 N 字节，未随会话恢复]」——「体积与隐私都倾向不落盘」只有在这一
 * 侧执行才成立（恢复期占位意味着磁盘上仍躺着 base64 画面，等于没排除）。
 *
 * system 提示词不落盘：恢复时由当前进程重建，经指纹（SessionHead）核对
 * 一致才放行——提示词包或脚本清单变了，旧上下文对不上号，拒绝恢复并说明。 */

struct SessionHead {
	int version = 1;
	std::string sessionId;	/* 审计会话 id（可空 = 审计未开；恢复时据此沿用） */
	std::string robot;		/* 结构脚本展示名（去目录去 .sql） */
	std::string toolsFp;	/* 工具集指纹（fingerprint64，注册表按序名字串） */
	std::string systemFp;	/* 系统提示词指纹（fingerprint64） */
};

/* 图片占位文本（写入期排除后的落盘形态；恢复后模型可见，据此知道图片不在） */
std::string sessionImagePlaceholder(std::size_t totalImageBytes);

/* 工具集指纹：按注册序拼接工具名后取 fingerprint64（agentaudit 单点）。
 * 指纹一致 ≈ 同一工具集；名字串撞车概率与既有 image_fp 同量级，可接受。 */
std::string sessionToolsFingerprint(const std::vector<ToolSpec>& specs);

/* 单条消息 → 会话文件的一行 JSON（不含换行）。system 消息不该走到这里
 * （调用方保证），走到也会被拒（返回空串）。 */
std::string encodeSessionMessage(const ChatMessage& m);

/* 追加式会话文件写入器。open(append=false) = 开新会话（截断旧文件并写会话头，
 * 调用方先经 sessionPermsWarning 提醒）；open(append=true) = 恢复（不写头，
 * 在既有内容后追加）。每行即刷（fflush）：崩溃后恢复最多丢最后一行。 */
class SessionFileWriter {
public:
	SessionFileWriter() = default;
	SessionFileWriter(const SessionFileWriter&) = delete;
	SessionFileWriter& operator=(const SessionFileWriter&) = delete;
	~SessionFileWriter();

	bool open(const std::string& path, bool append, std::string& err);
	void writeHeader(const SessionHead& head);
	/* 返回 false = 磁盘写失败（调用方应停用写入并响亮告知，不静默丢历史） */
	bool appendMessage(const ChatMessage& m);
	/* /clear 分界（批次 1）：落一行 type="session_clear" 标记。载入时该行
	 * **丢弃此前累积的全部消息**——否则恢复会把已被 /clear 遗忘的上下文
	 * 复活进模型上下文，与操作者在 REPL 里的意图相反。 */
	bool writeClearMarker();

private:
	std::FILE* file_ = nullptr;
	bool broken_ = false;
};

struct LoadedSession {
	SessionHead head;
	std::vector<ChatMessage> messages;
};

/* 读入会话文件。首行必须是合法会话头（没有头的文件拒绝整体恢复——
 * 指纹无从核对）；其后的行逐行解析，**损坏行跳过并点名**（含行号与人话
 * 原因，进 warnings），不整份作废——崩溃留下的最后一行残缺不该抹掉此前
 * 的完整历史。消息间协议自洽（孤儿/重复 tool_call_id）由调用方以
 * historyProtocolViolation 复核，本函数只管单行形态。 */
bool loadSessionFile(const std::string& path, LoadedSession& out,
		std::vector<std::string>& warnings, std::string& err);

/* ---- 历史必须能被服务端接受（SAN-1，2026-09-22）----
 * 「assistant.tool_calls 与 tool.tool_call_id 一一对应」此前只是注释里的约定，
 * 于是单轮截断通知复用最后一条已派发调用的 id 时没有任何东西会报错——
 * 而服务端会以 400 `tool_call_id 'x' answered more than once` 拒绝，
 * 且那条消息落在**不可回滚**的历史里（该轮派发已完成、回滚边界已越过），
 * 此后每个任务都重发同一段历史、拿同一个 400，只有 /clear 或重启能恢复。
 *
 * 本函数把那条约定变成**可执行判据**：
 *   ① 每个 tool.toolCallId 必须出现在某条 assistant.toolCalls 里（无孤儿）；
 *   ② 同一个 id 只能被回答**一次**（这条正是 SAN-1 的正判据）；
 *   ③ 某条 assistant 声明的每个工具调用都必须被回答过（无漏答）。
 * 与 scripts/fake_llm_server.py 的 protocol_error 是同一套判据的两份实现
 * （C++ 侧供单测、Python 侧供 E2E）——改一处必须改另一处。
 * 返回空串 = 合法；否则返回人话原因。
 * 归属沿革：原先住在 agentloop.hpp，随 C++ Agent REPL 一起被 d384a9b9 删掉，
 * 但上面 loadSessionFile 的契约仍以它为判据，故归位于此。Rust agent 侧
 * （agent/src）目前**没有**对等实现——恢复与续写历史的一致性只有这里管得住。 */
std::string historyProtocolViolation(const std::vector<ChatMessage>& history);

/* 恢复前的指纹核对。返回空串 = 一致；否则返回点名差异的人话原因
 * （sessionId 不参与比对——恢复本来就是要沿用它的）。 */
std::string sessionHeadMismatch(const SessionHead& expected, const SessionHead& found);

/* 文件存在且权限宽于 0600 时返回告警文本（对齐 mybot.llm.json 既有判据：
 * 会话文件含完整对话内容，同等敏感）。不存在/取不到权限 = 空串。 */
std::string sessionPermsWarning(const std::string& path);

} // namespace agent
} // namespace bot

