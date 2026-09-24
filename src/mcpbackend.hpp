#pragma once
/* mcpbackend.hpp —— MCP 后端（--mcp-backend，方案 F / 拓扑 F3）
 *
 * 把 agent 工具层经一条本地 AF_UNIX socket 暴露给 Rust MCP sidecar：sidecar 对上
 * 说 MCP（stdio），对下说这里的**pymsg 兼容帧**（4 字节 BE 长度 + UTF-8 JSON）。
 * 后端**不跑 agent 工具循环、不发起任何模型调用**——每次工具调用都由外部 MCP
 * 客户端驱动，后端只负责把调用穿过既有的唯一漏斗 DispatchGate::run
 * （策略确认 → 执行 → 审计），于是审计连续性与能力门控与 agent 模式逐字节一致。
 *
 * 线程归属（评估 §2.3）：runMcpBackend 在**调用者的线程（main 的主线程）**上同步
 * 服务，单连接、lock-step、严格串行——这正是 libmsg MsgServer(workers=0) 的性质，
 * 于是 callEchoSink / topDepth / luaExecBudget 三个 thread_local 天然正确，硬件
 * 调用自动串行（物理安全要的语义）。后端自己实现帧服务而不链接 libmsg：MsgServer
 * 硬编码 AF_INET（MsgServer.hpp:157-167）且全库无 SO_PEERCRED，加 UDS 要改共享头。
 *
 * 认证（评估 §3.1，用户定为「仅 SO_PEERCRED」）：socket 落 0700 目录、文件 0600，
 * accept 后用内核 SO_PEERCRED 校验对端 uid/gid == 本进程，不匹配即断连。
 *
 * ⚠ 残留风险（评估 §3.2 的尖锐推论，必须**响亮**告知、不得藏）：
 *   SO_PEERCRED 只挡**别的用户**，挡不住**同 uid** 的进程——而模型驱动的
 *   run_command shell 恰恰与后端同 uid。理论上它若发现 socket 路径，就能直连后端、
 *   绕过 agent loop 的 maxCallsPerRound/maxTaskSeconds 这些物理安全上限（因为这些
 *   上限是在 loop 里强制的，直打 socket 就跳过了 loop）。已就位的缓解：
 *     ① listening fd 是 CLOEXEC（markAllFdsCloexec 在 fork 前枚举 /proc/self/fd 覆盖），
 *        run_command 子进程**不继承**它（但仍可自己 connect 到路径——CLOEXEC 挡不住）；
 *     ② 0700 目录 + 不可预测路径提高发现成本；
 *     ③ 每连接预算再强制（--mcp-call-budget/--mcp-wall-clock）限制跑飞；
 *     ④ 后端开 run_command/apply_sql 时**强制** --agent-policy 逐次外部确认（buildAgentConfig 11b）。
 *   诚实建议：后端模式下**尽量不要开 --agent-shell**；若必须开，把后端跑在专用
 *   用户 / 容器里（真正的隔离），或将来加回首帧共享密钥 BOT4CJ_MCP_TOKEN——该名字
 *   含 token，会被 isSensitiveEnvName 自动从 run_command 子进程环境剔除并抹零，
 *   模型驱动的 shell 拿不到它（评估 §3.1 的「免费隔离」协同点）。
 */

#include <chrono>
#include <cstddef>
#include <memory>
#include <ostream>
#include <string>

#include "agentaudit.hpp"
#include "agentdispatch.hpp"
#include "agentpolicy.hpp"
#include "agenttools.hpp"
#include "json.hpp"

namespace bot {

class RobotStruct;

namespace agent {

struct AgentModeConfig;	/* 定义在 agentmode.hpp；此处只按引用/常量引用用 */

/* 帧上限：**收紧到 8 MiB**，对齐 bot4cj 自己的 JSON 解析器（jsonparse.hpp 的
 * kJsonMaxBytes）。libmsg 的上限是 64 MiB，但一个 64 MiB 的合法帧会被我们的
 * 解析器以一个离真实成因很远的错误拒掉（评估 §2.3 缺点 4），故在帧层就先收紧。 */
inline constexpr std::size_t kMcpMaxFrameBytes = 8u << 20;

/* 帧头长度：4 字节 big-endian uint32 载荷字节长（与 pymsg/gomsg/libmsg 字节兼容，
 * 无 magic / 无版本 / 无 flags / 无校验和）。 */
inline constexpr std::size_t kMcpHeaderSize = 4;

struct McpBackendOptions {
	std::string socketPath;		/* AF_UNIX 路径（必填；长度受 sun_path 限制） */
	/* 每连接的工具调用上限（评估 §3.2 的「预算再强制」）：<=0 = 不限。
	 * 后端**不得假设调用方是 agent loop**，故自己再强制一遍——超限时拒绝派发
	 * 并如实回报（被拒的调用无副作用）。 */
	long callBudget = 0;
	long wallClockBudgetSec = 0;	/* 每连接墙钟预算（秒）；<=0 = 不限 */
	bool requirePeerCred = true;	/* accept 后 SO_PEERCRED 校验 uid/gid（单测可关） */
	int recvTimeoutSec = 10;	/* deadline recv 空闲超时（防 Slowloris，评估 §7.1） */
};

/* ---- 纯协议件（不碰 socket，可离线单测）---- */

/* 把载荷封成一帧：4 字节 BE 长度前缀 + 载荷字节。 */
std::string mcpEncodeFrame(const std::string& payload);

/* 严格 UTF-8 校验：拒 overlong、代理区 D800–DFFF、>U+10FFFF（与 libmsg 的
 * isValidUtf8 同判据；接收侧校验，评估 §7.1）。合法返回 true。 */
bool mcpValidUtf8(const std::string& s);

/* accept 后校验对端身份：内核 SO_PEERCRED 给出的 uid/gid 必须与本进程一致
 * （不可伪造，评估 §3.1）。匹配返回 true；不匹配或 getsockopt 失败返回 false
 * 并把原因写进 log。暴露为公开函数便于白盒单测（socketpair 造对端），与
 * actionsource.hpp 的 TcpAuthStatus 同惯例。 */
bool mcpCheckPeerCred(int fd, std::ostream& log);

/* 后端会话状态 + 路由。持有工具注册表、派发漏斗（DispatchGate）与每连接预算。
 * handleRequest / handleFrame 是纯逻辑（Json/文本 → Json/文本），不碰 socket，
 * 便于单测；socket 收发在 runMcpBackend 里。 */
class McpBackend {
public:
	/* cfg/robot 为**可空对**（要么都给、要么都空）：给了则 context.get 路由可用
	 * （Rust agent 以它为系统提示词素材的唯一来源，实时取材——热插拔后 hardware
	 * 反映当前注册表）；都空时 context.get 回错误（单测的手搓后端即此形态）。
	 * 生命周期：cfg 与 robot 都由 runMcpBackend 的调用链持有，跨越整个服务期。 */
	McpBackend(AgentToolRegistry tools,
			std::shared_ptr<const AgentPolicy> policy,
			std::shared_ptr<AuditLog> audit,
			long callBudget, long wallClockBudgetSec,
			const AgentModeConfig* cfg = nullptr, RobotStruct* robot = nullptr);

	/* gate_ 持有对本对象**自己的** tools_ 成员的引用，故 McpBackend 绝不可拷贝/
	 * 移动（移动会让 gate_ 的引用悬垂到被移走的旧对象）。就地构造；从工厂函数
	 * 按值返回时依赖 C++17 保证的复制消除（guaranteed copy elision），不触发移动。 */
	McpBackend(const McpBackend&) = delete;
	McpBackend& operator=(const McpBackend&) = delete;
	McpBackend(McpBackend&&) = delete;
	McpBackend& operator=(McpBackend&&) = delete;

	/* 每接受一条新连接调用一次：重置每连接的调用计数与墙钟起点。 */
	void beginConnection();

	/* 路由一个**已解析**的请求对象 → 响应对象（{"success":...}）。不抛：任何
	 * 异常都折成 {"success":false,"error":...}（评估 §7.6「handler 异常 ⇒ 错误
	 * 回复，绝不崩溃」的不变式）。 */
	Json handleRequest(const Json& req);

	/* 处理一帧载荷文本：parseJson → handleRequest → toString。非法 JSON 或顶层
	 * 非对象 → 固定串 "invalid JSON request"（**从不回显输入**，与三份 libmsg
	 * 实现同口径，评估 §7.4）。 */
	std::string handleFrame(const std::string& payload);

	/* 整服务端**一** session（用户定）：在 accept 循环外写 start，停机时写 end。
	 * driver/model/endpoint 留空（后端不调模型）；audit 未开启时 no-op。 */
	void writeSessionRecord(const char* status) const;

	const AgentToolRegistry& tools() const { return tools_; }

private:
	Json routeToolList();
	Json routeToolCall(const Json& req);
	/* 只读素材路由（rig agent 的系统提示词来源）：与 tool.list 同口径——不走
	 * DispatchGate、不计每连接预算（不发硬件指令、无副作用、无密钥）。 */
	Json routeContextGet();
	Json okReply(Json data) const;		/* {"success":true,"data":...} */
	Json errReply(const std::string& msg) const;	/* {"success":false,"error":...} */
	bool budgetExceeded(std::string& reason) const;

	/* 声明顺序即初始化顺序：gate_ 引用 tools_，故 tools_ 必须在 gate_ 之前。 */
	AgentToolRegistry tools_;
	std::shared_ptr<const AgentPolicy> policy_;
	std::shared_ptr<AuditLog> audit_;
	DispatchGate gate_;
	const AgentModeConfig* cfg_ = nullptr;	/* context.get 素材（可空，见构造器注） */
	RobotStruct* robot_ = nullptr;		/* 同上；非 const：describeHardware 取可变引用 */
	long callBudget_;
	long wallClockBudgetSec_;
	long calls_ = 0;		/* 本连接已派发的 tool.call 数 */
	int turn_ = 0;		/* 传给 DispatchGate::run 的轮次（审计用，逐次自增） */
	std::chrono::steady_clock::time_point connStart_;
};

/* 在主线程同步服务：绑定 AF_UNIX（0700 目录 / 0600 文件）→ accept 单连接 →
 * SO_PEERCRED 校验 → 帧循环（deadline recv + 8 MiB 上限 + 严格 UTF-8）→ 回帧。
 * 对端断开后回到 accept 等下一条（sidecar 可重连）；收到停止信号（stopRequested）
 * 优雅退出。返回进程退出码（0 正常 / 1 运行时错误 / 2 配置错误）。
 * 前提：robot 已 active()（工具作用于常驻机器人实例）。 */
int runMcpBackend(RobotStruct& robot, const AgentModeConfig& cfg,
		const McpBackendOptions& opts, std::ostream& log);

} // namespace agent
} // namespace bot
