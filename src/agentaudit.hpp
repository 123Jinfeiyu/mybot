#pragma once
/* agentaudit.hpp —— agent 工具调用的审计轨迹（JSON Lines 追加写）
 *
 * 动机（原 docs/proposals/audit-approval-cancel.md P1，2026-09-23 移至 .tmp/）：
 * agent 会话此前唯一的痕迹是
 * 日志流（stderr）上的工具回显——无时间戳、无持久化、无结构化（该日志**恒开**，
 * 见 docs/agent.md；`--verbose` 不是本项目的选项）。现场机器人（部署形态 B）
 * 出事后「模型到底做了什么」无从复盘。本模块在**宿主工具派发的唯一漏斗**
 * （DispatchGate::run，见 agentdispatch）落一条 JSON Lines 记录。MCP 后端
 * （--mcp-backend）不发起模型调用，但工具条目仍经同一漏斗记账，会话头尾由
 * mcpbackend 自己记。
 *
 * 设计约束：
 *   * 默认关闭（--audit-log 不给 = 不写任何东西），开启即 append-only，
 *     不做轮转（交给外部工具）；
 *   * 图片**不进日志**：capture_image 的 base64 只记字节数与 64 位指纹——
 *     否则每次拍照数百 KB 灌进磁盘，且画面可能含隐私；
 *   * run_command 的命令原文要记，但 KEY=VALUE 形态的敏感值先遮蔽
 *     （maskSecretsInText；判据与 agent_subprocess 的环境清洗同源）；
 *   * 失败也记（含参数错误/工具异常）：「模型想做什么但没做成」同样是审计信息；
 *   * 只在 agent 主线程写（工具循环无并发派发），故不加锁。
 */

#include <memory>
#include <optional>
#include <string>

#include "json.hpp"

namespace bot {
namespace agent {

/* 一条工具调用审计记录。字段与 JSON 行的映射见 AuditLog::write。 */
struct AuditEntry {
	/* SAN-9（2026-09-22）：记录类型。**加法演进**——既有工具行的字段名、
	 * 顺序与语义一个不动，新增两类记录靠这个字段区分（老消费方按
	 * `tool == "run_command"` 之类的过滤表达式解析，结果逐字节不变）。
	 *   "tool"    —— 既有形态：一次工具调用；
	 *   "model"   —— 一轮模型调用（耗时、请求/响应字节、provider 给的 token 数）；
	 *   "session" —— 会话开始/结束（含驱动、模型、接入点、各项上限）。
	 * 为什么需要：审计此前**只记工具调用**，于是"模型说了什么、花了多久、
	 * 这一轮到底有没有发出请求"完全不可查；而一次在模型调用阶段中断的会话
	 * 在日志里**一条记录都没有**——那恰恰是最需要复盘的时候。 */
	std::string kind = "tool";
	int turn = -1;			/* 任务内的模型轮次（未知为 -1） */
	std::string tool;		/* 工具名（kind="tool"）/ 事件名（kind="session"） */
	Json args;			/* 解析成功的参数对象（否则空） */
	std::string argsRaw;		/* 原始参数文本（截断到 kMaxAuditArgsRaw） */
	bool ok = true;			/* false = 参数错误/未知工具/工具异常/脚本执行失败 */
	std::string error;		/* ok=false 时的人话原因 */
	long long ms = 0;		/* 耗时（工具执行 / 模型调用） */
	std::size_t bytes = 0;		/* 回传模型的文本字节数 */
	std::size_t imageBytes = 0;	/* 图片 data URL 字节数（0 = 无图） */
	std::string imageFp;		/* 图片指纹（FNV-1a 64 hex，非密码学摘要） */
	std::optional<long long> exitCode;	/* run_command 专属（自回执文本解析） */
	/* G2（2026-09-23）：工具自报的**生效摘要**（apply_sql 等热插/高危工具）。
	 * 只做加法：老工具不给，工具行的既有字段与顺序不动。 */
	std::string summary;

	/* ---- SAN-9：kind="session" 专用（全部只在会话记录里出现）---- */
	std::string driver;		/* 协议驱动表名 */
	std::string model;		/* 模型名（可为空 = 用驱动缺省） */
	std::string endpoint;		/* 接入点（**去掉 query 与 userinfo**，见 write） */
	std::string status;		/* "start" / "end" / "error" */
	std::string apiKeySource;	/* 只记来源枚举名，绝不记值 */
	/* 批次 1（2026-09-23）：该会话由 --resume 恢复而来。加法字段——只在
	 * kind="session" 的 start 记录上出现（"resumed": true），既有字段与
	 * 顺序不动，老消费方按既有字段过滤的行为逐字节不变（schema 演进已在
	 * changelog 声明）。 */
	bool resumed = false;

	/* ---- SAN-9：kind="model" 专用 ---- */
	std::size_t reqBytes = 0;	/* 本次请求体估算字节（可 0 = 未统计） */
	std::size_t respBytes = 0;	/* 本次响应文本字节 */
	int attempts = 0;		/* 实际尝试次数（>1 = 发生过重试） */
	std::optional<long long> promptTokens;		/* provider 给的用量，缺则无 */
	std::optional<long long> completionTokens;
};

/* 参数原文进审计前的截断上限：审计要能对账，但一条 2 MiB 的参数串会把日志
 * 本身变成体积源；超限截断并在值里显式标注（不静默）。 */
constexpr std::size_t kMaxAuditArgsRaw = 4096;

class AuditLog {
public:
	AuditLog() = default;
	AuditLog(const AuditLog&) = delete;
	AuditLog& operator=(const AuditLog&) = delete;
	~AuditLog();

	/* path == "-" 时写 stderr；其余为追加打开的文件路径。
	 * 打开失败返回 false 并把原因写进 err（调用方在上电前报错退出）。 */
	bool open(const std::string& path, std::string& err);

	/* 追加一条记录。日志流损坏（磁盘满等）时向 stderr 告警一次，不再静默。 */
	void write(const AuditEntry& e);

	bool enabled() const { return opened_; }
	const std::string& sessionId() const { return sessionId_; }
	const std::string& path() const { return path_; }

	/* 批次 1（会话恢复）：沿用被恢复会话的原 id，让审计里恢复前后的行能
	 * 对上同一段对话。只生效一次（重复调用忽略），须在 open 之后、首次
	 * write 之前调用——首条记录一旦带上了旧 id，再改反而制造断层。 */
	void adoptSessionId(const std::string& id);

private:
	std::string path_;
	std::string sessionId_;
	bool sessionIdAdopted_ = false;	/* adoptSessionId 只生效一次 */
	bool opened_ = false;
	bool toStderr_ = false;
	bool warnBroken_ = false;	/* 损坏告警只打一次 */
	void* file_ = nullptr;		/* FILE*（避免在头里拖 <cstdio>） */
};

/* 把文本里「敏感名=值」形态的值遮蔽成 ***（run_command 的命令原文进日志前调用）。
 * 名字判据与 agent_subprocess 的环境清洗同源（isSensitiveEnvName），另加
 * authorization/bearer 两个只对文本遮蔽有意义的词。值边界：空白或引号。 */
std::string maskSecretsInText(const std::string& text);

/* 从 run_command 的回执文本解析退出码（"exit_code=N" 前缀）；非该形态返回空 */
std::optional<long long> extractExitCode(const std::string& text);

/* FNV-1a 64 位指纹的 hex 文本（16 字符）。用途是把图片在日志里对上号，
 * 不是防篡改——审计日志的完整性靠文件系统权限与外部收集，不靠摘要。 */
std::string fingerprint64(const std::string& s);

/* ISO-8601 本地时间戳（毫秒 + UTC 偏移），如 2026-09-22T17:04:11.238+08:00 */
std::string auditTimestamp();

} // namespace agent
} // namespace bot
