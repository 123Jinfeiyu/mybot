/* mcpbackend.cpp —— MCP 后端实现（见 mcpbackend.hpp 的设计说明）
 *
 * 线格式与 pymsg/gomsg/libmsg 字节兼容（4 字节 BE 长度 + UTF-8 JSON），但**不链接
 * libmsg**：它的 MsgServer 硬编码 AF_INET 且无 SO_PEERCRED，加 UDS 要改共享头。
 * 这里自研约 150 行帧服务，socket 建立沿用 actionsource.cpp 动作源 TCP 的
 * 先例（FdGuard + poll + 停止信号响应），只是把 AF_INET 换成 AF_UNIX 并加
 * SO_PEERCRED（更早的先例 TcpAgentSession 已随 agent-over-TCP 路径删除，
 * 见 git 历史）。审计加固清单（评估 §7.6）逐条落地：分配前查上限、帧头与载荷共享
 * 同一 deadline（防 Slowloris）、发送超时（防慢读端）、接收侧校验 UTF-8、错误串
 * 脱敏绝对路径、解析错误不回显输入、handler 异常 ⇒ 错误回复绝不崩溃。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE	/* struct ucred / SO_PEERCRED / accept4 / SOCK_CLOEXEC */
#endif

#include "mcpbackend.hpp"

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "agentmode.hpp"	/* AgentModeConfig / makeToolContext / structureDisplayName */
#include "agentprompt.hpp"	/* structureScriptHead / quoteUntrusted（context.get 素材） */
#include "jsonparse.hpp"	/* parseJson */
#include "robot.hpp"		/* RobotStruct 完整定义（context.get 的 registry()） */
#include "util_cli.hpp"		/* AuthFailGate（认证爆破失败门单点） */
#include "util_env.hpp"
#include "util_file.hpp"
#include "util_str.hpp"		/* safeEquals 恒时比较（凭据比对安全单点） */
#include "util_utf8.hpp"		/* stopRequested / isValidUtf8 / FdGuard */

namespace fs = std::filesystem;

namespace bot {
namespace agent {

namespace {

/* 帧传输件（writeBe32/readBe32/sanitizeError/recvAllDeadline/readFrame/
 * sendFrame）与纯协议件（mcpEncodeFrame/mcpCheckAuthFrame/mcpCheckPeerCred/
 * mcpProbeSocketPath）已整体迁至 mcptransport.cpp（2026-10-01 TU 拆分），
 * 跨 TU 接缝见 mcpbackend.hpp 的「帧传输接缝」。 */

using Clock = std::chrono::steady_clock;


/* serveConnection 的认证记账结果（AuthFailGate 的输入，2026-09-30 审计 P1）：
 *   Failed——给出了首帧但认证不通过（连续失败计数 +1）；
 *   Passed——认证通过（业务循环无论怎么结束都算，连续失败计数归零）；
 *   Neutral——没走到认证判定（哑对端/无 token 模式），计数不动。 */
enum class ConnAuth { Neutral, Passed, Failed };

/* serveConnection 的连接结局（2026-10-01 审计 P2 前只有 ConnAuth，断连安全动作
 * 无条件执行——语义缺口见 acceptLoop 的门）。两个判据服务两件事，必须分开记：
 *   auth——AuthFailGate 爆破计数口径（见上）；
 *   admitted——是否真正进入过业务循环：无 token 模式恒 true；token 模式认证
 *     通过才 true。断连安全动作只对 admitted 连接执行——探连（连上就断/
 *     认证帧未到）与认证失败从未被服务，不该拥有触发硬件动作的通道。 */
struct ConnOutcome {
	ConnAuth auth = ConnAuth::Neutral;
	bool admitted = false;
};

/* 服务单条连接：帧循环直到对端关闭 / 协议违规 / 空闲超时 / 停止信号。
 * 空闲 deadline 逐帧重取（--mcp-idle-timeout 是「从现在起再等这么久」，
 * 墙钟预算是从连接起的绝对终点——两者由 idleDeadline 合成）。
 * MCP-1（审计 P2-1）：authToken 非空时第一帧是认证帧——通过才进入正常循环，
 * 不通过回执后断连（对端可修正重连）。失败的认证要落审计（kind=session，
 * status=auth_failed）：它是「有进程在试探这条 socket」的信号，事后复盘
 * 必须看得见；token 本身绝不进日志/审计/回执。返回 ConnOutcome：auth 供
 * acceptLoop 记账连续失败（见 AuthFailGate），admitted 供断连安全动作门控。 */
ConnOutcome serveConnection(int fd, McpBackend& backend, const McpBackendOptions& opts,
		std::ostream& log) {
	bool admitted = opts.authToken.empty();
	if (!opts.authToken.empty()) {
		std::string payload;
		const FrameResult fr = readFrame(fd, payload, opts.recvTimeoutSec,
			backend.idleDeadline(opts.idleTimeoutSec), log);
		if (fr != FrameResult::Ok) {
			/* 对端连上后连认证帧都没发就消失/超时——按违规口径断连即可，
			 * 空闲超时的既有文案已覆盖「哑对端让位」的语义。 */
			if (fr == FrameResult::IdleTimeout) {
				log << "MCP 后端：认证帧未到，空闲超时/墙钟预算耗尽，断开连接\n";
			} else if (fr == FrameResult::Violation) {
				log << "MCP 后端：认证帧违规，断开连接\n";
			}
			return ConnOutcome{ConnAuth::Neutral, false};
		}
		const McpAuthOutcome a = mcpCheckAuthFrame(opts.authToken, payload);
		if (a == McpAuthOutcome::Ok) {
			/* lock-step：认证帧也要回执（Rust 侧 authenticate 在等这一帧） */
			if (!sendFrame(fd, "{\"success\":true,\"data\":{\"auth\":true}}")) {
				log << "MCP 后端：认证回帧发送失败，断开连接\n";
				return ConnOutcome{ConnAuth::Neutral, false};
			}
			log << "MCP 后端：首帧认证通过\n";
			admitted = true;
		} else {
			/* 回执不回显 token、不区分「错密钥」与「没带密钥」——对试探者
			 * 两者给同一句话，信息量最小化。 */
			(void)sendFrame(fd, "{\"success\":false,\"error\":\"auth required:"
				" first frame must be {\\\"_route\\\":\\\"auth\\\","
				"\\\"token\\\":...}\"}");
			log << "MCP 后端：首帧认证失败（" << (a == McpAuthOutcome::NotAuthFrame
				? "第一帧不是 auth 帧" : "共享密钥不匹配")
				<< "），断开连接\n";
			backend.recordAuthRejected();
			return ConnOutcome{ConnAuth::Failed, false};
		}
	}
	const ConnAuth result = opts.authToken.empty() ? ConnAuth::Neutral : ConnAuth::Passed;
	while (!stopRequested()) {
		std::string payload;
		const FrameResult fr = readFrame(fd, payload, opts.recvTimeoutSec,
			backend.idleDeadline(opts.idleTimeoutSec), log);
		if (fr == FrameResult::PeerClosed) { return ConnOutcome{result, admitted}; }
		if (fr == FrameResult::IdleTimeout) {
			log << "MCP 后端：空闲超时/每连接墙钟预算耗尽，断开连接（对端可重连）\n";
			return ConnOutcome{result, admitted};
		}
		if (fr == FrameResult::Violation) {
			log << "MCP 后端：协议违规，断开连接\n";
			return ConnOutcome{result, admitted};
		}
		const std::string resp = backend.handleFrame(payload);
		if (!sendFrame(fd, resp)) {
			log << "MCP 后端：回帧发送失败，断开连接\n";
			return ConnOutcome{result, admitted};
		}
	}
	return ConnOutcome{result, admitted};
}

} // namespace

/* ---- McpBackend ---- */

McpBackend::McpBackend(AgentToolRegistry tools,
		std::shared_ptr<const AgentPolicy> policy,
		std::shared_ptr<AuditLog> audit,
		long callBudget, long wallClockBudgetSec,
		const AgentModeConfig* cfg, RobotStruct* robot)
	: tools_(std::move(tools)),
	  policy_(std::move(policy)),
	  audit_(std::move(audit)),
	  gate_(tools_, policy_, audit_),
	  cfg_(cfg),
	  robot_(robot),
	  callBudget_(callBudget),
	  wallClockBudgetSec_(wallClockBudgetSec),
	  connStart_(Clock::now()) {}

void McpBackend::beginConnection() {
	calls_ = 0;
	turn_ = 0;
	connStart_ = Clock::now();
}

Clock::time_point McpBackend::idleDeadline(int idleTimeoutSec) const {
	Clock::time_point deadline = Clock::time_point::max();
	if (wallClockBudgetSec_ > 0) {
		const Clock::time_point wall = connStart_
			+ std::chrono::seconds(wallClockBudgetSec_);
		if (wall < deadline) { deadline = wall; }
	}
	if (idleTimeoutSec > 0) {
		const Clock::time_point idle = Clock::now() + std::chrono::seconds(idleTimeoutSec);
		if (idle < deadline) { deadline = idle; }
	}
	return deadline;
}

Json McpBackend::okReply(Json data) const {
	Json o = JsonObject();
	o.put("success", JsonBool(true));
	o.put("data", std::move(data));
	return o;
}

Json McpBackend::errReply(const std::string& msg) const {
	Json o = JsonObject();
	o.put("success", JsonBool(false));
	o.put("error", JsonString(msg));
	return o;
}

bool McpBackend::budgetExceeded(std::string& reason) const {
	if (callBudget_ > 0 && calls_ >= callBudget_) {
		reason = "已达到每连接工具调用上限（" + std::to_string(callBudget_)
			+ " 次）；后端不假设调用方是 agent loop，故自行强制预算。"
			  "请重连或调高上限。";
		return true;
	}
	if (wallClockBudgetSec_ > 0) {
		const long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
			Clock::now() - connStart_).count();
		if (elapsed >= wallClockBudgetSec_) {
			reason = "已超过每连接墙钟预算（" + std::to_string(wallClockBudgetSec_)
				+ " 秒）；请重连。";
			return true;
		}
	}
	return false;
}

Json McpBackend::routeToolList() {
	Json arr = JsonArray();
	for (const ToolSpec& t : tools_.specs()) {
		Json o = JsonObject();
		o.put("name", JsonString(t.name));
		o.put("description", JsonString(t.description));
		/* parameters 本就是 JSON Schema object，直接当 MCP 的 inputSchema。 */
		o.put("inputSchema", t.parameters);
		arr.add(o);
	}
	Json data = JsonObject();
	data.put("tools", arr);
	return okReply(std::move(data));
}

Json McpBackend::routeToolCall(const Json& req) {
	/* 预算再强制在**派发之前**：被拒的调用没有副作用（评估 §3.2）。 */
	std::string reason;
	if (budgetExceeded(reason)) { return errReply(reason); }

	const Json* nameJ = req.find("name");
	if (nameJ == nullptr || !nameJ->isStr()) {
		return errReply("tool.call 需要字符串字段 name");
	}
	std::string argsStr = "{}";
	if (const Json* argsJ = req.find("arguments"); argsJ != nullptr) {
		if (!argsJ->isObj()) { return errReply("tool.call 的 arguments 必须是 JSON 对象"); }
		argsStr = argsJ->toString();
	}

	ToolCall call;
	call.id.clear();		/* MCP call_tool 无需 id；DispatchGate 不消费它 */
	call.name = nameJ->getValue();
	call.arguments = std::move(argsStr);

	++calls_;
	/* 唯一漏斗：策略确认 → 执行 → 审计。未知工具/参数错/异常都由 gate 折成
	 * 文本回执并记审计（评估 §3.4「所有 MCP 调用必穿 DispatchGate::run」）。 */
	/* 七字段回执的 JSON 序列化点：字段清单钉在 AgentToolResult（agenttools.hpp），
	 * Rust 侧对应 ToolOutcome（agent/src/bridge.rs）——三处一致性由 test_mcpbackend
	 * 的 mcpbackend_toolresult_schema_matches_rust_outcome 读源比对钉住。 */
	const AgentToolResult res = gate_.run(call, turn_++);

	Json data = JsonObject();
	data.put("text", JsonString(res.text));
	data.put("imageDataUrl", JsonString(res.imageDataUrl));
	data.put("ok", JsonBool(res.ok));
	data.put("error", JsonString(res.error));
	data.put("executed", JsonBool(res.executed));
	data.put("paramError", JsonBool(res.paramError));
	data.put("summary", JsonString(res.summary));
	return okReply(std::move(data));
}

Json McpBackend::routeContextGet() {
	if (cfg_ == nullptr || robot_ == nullptr) {
		return errReply("context.get 不可用：后端未绑定配置与机器人上下文");
	}
	/* 磁盘内容三段的读法/消毒/定界与原 buildAgentSystemPrompt 完全一致（单一
	 * 事实源；提示词最终拼装在 Rust 侧 agent/src/prompt.rs，语义与之逐字对齐）：
	 * 后端不拼系统提示词、无配置组装期缓存，故每次 context.get 实时读。
	 * 区别只在呈现：这里返回**已定界的块**，由 Rust agent 拼进它自己的系统
	 * 提示词——「同一份磁盘内容，两条进上下文的通道必须有同一定界强度」。 */
	const std::string sql = structureScriptHead(cfg_->structurePath, kStructureHeadBytes);
	const std::string scripts = describeActionScripts(cfg_->scriptsDir);

	Json data = JsonObject();
	data.put("robotName", JsonString(structureDisplayName(cfg_->structurePath)));
	data.put("structurePath", JsonString(cfg_->structurePath));
	data.put("scriptsDir", JsonString(cfg_->scriptsDir));
	/* 工作方式段的条件开关：Rust agent 按这两个旗标拼条件句（与
	 * 原 buildAgentSystemPrompt 的 allowShell/allowHotSql 分支同判据）。 */
	data.put("allowShell", JsonBool(cfg_->allowShell));
	data.put("allowHotSql", JsonBool(cfg_->allowHotSql));
	/* REPL 横幅与 /status 的硬件计数（实时取：热插拔后不漂移）。 */
	data.put("hardwareCount",
		JsonInt(static_cast<std::int64_t>(robot_->registry().byName().size())));
	data.put("structureExcerpt", JsonString(sql.empty()
			? std::string("(结构脚本不可读)\n")
			: quoteUntrusted("结构脚本 " + cfg_->structurePath + " 摘录：\n", sql)));
	data.put("hardwareList",
		JsonString(quoteUntrusted("硬件变量清单：\n", describeHardware(*robot_))));
	data.put("scriptsList", JsonString(quoteUntrusted(
			"动作脚本清单（相对路径 + 首行注释）：\n", scripts)));
	/* 可读变量是内联短清单（进工作方式条目，非引用块），空 = 一个字段都不给。 */
	data.put("readableVars", JsonString(describeReadableVars(cfg_->readVars, *robot_)));
	return okReply(std::move(data));
}

Json McpBackend::handleRequest(const Json& req) {
	try {
		if (!req.isObj()) {
			/* 顶层非对象（如 [1,2]）：与 C++ RpcServer 同口径判为非法请求，
			 * 且**绝不回显输入**（评估 §7.4）。 */
			return errReply("invalid JSON request");
		}
		const Json* route = req.find("_route");
		if (route == nullptr) { return errReply("missing _route"); }
		if (!route->isStr()) { return errReply("_route must be a string"); }
		const std::string r = route->getValue();
		if (r == "ping") {
			Json d = JsonObject();
			d.put("pong", JsonBool(true));
			return okReply(std::move(d));
		}
		if (r == "tool.list") { return routeToolList(); }
		if (r == "tool.call") { return routeToolCall(req); }
		if (r == "context.get") { return routeContextGet(); }
		return errReply("unknown route: " + r);
	} catch (const std::exception& e) {
		return errReply(sanitizeError(e.what()));
	} catch (...) {
		/* LuaJIT 用 C++ 异常实现 luaD_throw，非 std::exception 也要接住，绝不
		 * 让异常穿出 handler（判据锚点与边界清单：util.hpp 的
		 * kNonStdExceptionNote 注释块；本站点文案经 sanitize 刻意定容）。 */
		return errReply("internal error");
	}
}

std::string McpBackend::handleFrame(const std::string& payload) {
	Json req;
	try {
		req = parseJson(payload);	/* 8 MiB / 64 层上限由 parseJson 强制 */
	} catch (const std::exception&) {
		return errReply("invalid JSON request").toString();
	}
	return handleRequest(req).toString();
}

void McpBackend::writeSessionRecord(const char* status) const {
	if (!audit_ || !audit_->enabled()) { return; }
	AuditEntry e;
	e.kind = "session";
	e.status = status;
	/* driver/model/endpoint 留空：后端不发起模型调用。AuditLog::write 对空字段
	 * 不输出，故审计行里不会出现这些键（schema 与 agent 会话记录一致）。 */
	audit_->write(e);
}

/* MCP-1（审计 P2-1）：认证失败落审计——「有进程在试探这条 socket」是事后复盘
 * 必须看得见的信号；token 与失败详情都不进审计（信息量最小化，日志侧已有）。 */
void McpBackend::recordAuthRejected() const {
	if (!audit_ || !audit_->enabled()) { return; }
	AuditEntry e;
	e.kind = "session";
	e.status = "auth_failed";
	audit_->write(e);
}

/* ---- socket 服务 ---- */

/* ---- 断连安全动作（2026-09-25，--on-disconnect）----
 * 连接结束（任何原因：对端关闭/崩溃/空闲或墙钟超时/协议违规/发送失败）后，
 * 后端自己把硬件带到操作者预设的安全姿态。此前 agent 进程一死，硬件就停在
 * **最后一个姿态**带电常驻（docs/agent.md 已知限制）。
 *   * 不走 DispatchGate：这不是模型的请求，是操作者的配置；策略确认钩子
 *     （require_confirm）是给"模型想做什么"设的闸门，安全动作等它反而危险；
 *   * 执行判据与 run_action_script 同源（runActionScriptFile：沙箱/体积/预算）；
 *   * 失败**不能**让后端退出——退出即摘电清理，反倒是另一种"安全"，但会把
 *     操作者的重连机会一起带走；故只记日志 + 审计（kind=session，
 *     status=disconnect_action，error 字段即失败原因），继续 accept；
 *   * 停机（stopRequested）不触发：SIGINT/SIGTERM 路径由 deactivate() 的
 *     断电清理链负责，再跑一个动作脚本只会拖慢停机。 */
static void runDisconnectAction(const AgentToolContext& tctx, const std::string& script,
		const std::shared_ptr<AuditLog>& audit, std::ostream& log) {
	if (script.empty() || stopRequested()) { return; }
	log << "MCP 后端：连接结束，执行断连安全动作 " << script << "\n";
	AuditEntry e;
	e.kind = "session";
	e.status = "disconnect_action";
	e.summary = script;
	const Clock::time_point t0 = Clock::now();
	try {
		std::string echo;
		const std::size_t bytes = runActionScriptFile(tctx, script, echo);
		log << "MCP 后端：断连安全动作完成（" << bytes << " 字节）\n";
	} catch (const std::exception& ex) {
		e.error = ex.what();
		log << "MCP 后端：断连安全动作失败: " << ex.what() << "（后端继续服务）\n";
	} catch (...) {
		e.error = "unknown exception";
		log << "MCP 后端：断连安全动作失败: 未知异常（后端继续服务）\n";
	}
	e.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
	if (e.ms <= 0) { e.ms = 1; }	/* ms>0 才落字段；0 会让"跑过"这件事在审计里消失 */
	if (audit && audit->enabled()) { audit->write(e); }
}

namespace {

/* AF_UNIX 监听套接字的准备结果（拆自 runMcpBackend，2026-09-26 批次 5）：
 * fd 为已 bind+listen 的套接字（get() < 0 = 失败），rc 为失败时的进程退出码
 * （0 = 成功；配置类错误 2、系统调用类错误 1，与 runMcpBackend 原口径一致）。 */
struct ListenSocket {
	FdGuard fd;
	int rc = 0;
};

/* 准备 AF_UNIX 监听套接字：父目录创建/权限体检 → 启动前探测（A-1）→
 * socket/bind/chmod 0600/listen。成功返回已监听的 fd（rc=0）；失败把原因写进
 * log 并返回相应退出码，fd 为空。 */
ListenSocket prepareListenSocket(const std::string& socketPath, std::ostream& log) {
	/* 父目录：缺失则以 0700 建（单级）；已存在但权限过松则告警（不静默）。 */
	const fs::path sp(socketPath);
	fs::path parent = sp.parent_path();
	if (parent.empty()) { parent = "."; }
	std::error_code ec;
	if (!fs::exists(parent, ec)) {
		if (::mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST) {
			log << "错误: 无法创建 socket 父目录 '" << parent.string() << "': "
				<< errnoText(errno) << "\n";
			return ListenSocket{FdGuard(), 2};
		}
	} else {
		struct stat st{};
		if (::stat(parent.c_str(), &st) == 0 && (st.st_mode & 0077) != 0) {
			log << "警告: socket 父目录 '" << parent.string()
				<< "' 权限过松（建议 0700）；同机其他用户可能连上后端\n";
		}
	}

	/* 启动前探测（A-1，2026-09-25 审计）：不再无条件 unlink——误配同一路径的
	 * 第二个后端会静默偷走 socket 文件、让先启动者永远不可达。三态处置：
	 * 活实例/无法判定 → 响亮拒绝（fail-closed，换路径或先停占用者）；
	 * 陈旧残留（上次实例没清理）→ 安全 unlink 后绑定；非 socket 文件 →
	 * 不替操作者删陌生文件。 */
	std::string probeWhy;
	switch (mcpProbeSocketPath(socketPath, probeWhy)) {
	case McpSocketProbe::PathBusy:
		log << "错误: socket 路径 '" << socketPath << "' 已被占用（" << probeWhy
			<< "）。两个后端共用一条路径会让后启动者静默偷走 socket、先启动者"
			"继续监听却永远不可达。请给本实例换 --mcp-socket，或先停掉占用者。\n";
		return ListenSocket{FdGuard(), 2};
	case McpSocketProbe::PathUnusable:
		log << "错误: socket 路径 '" << socketPath << "' 不可用（" << probeWhy
			<< "）；不会自动删除它，请手工处理后重试。\n";
		return ListenSocket{FdGuard(), 2};
	case McpSocketProbe::PathFree:
		break;
	}
	::unlink(socketPath.c_str());	/* 陈旧 socket（探测确认无人监听）：清掉再绑定 */

	FdGuard listenFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
	if (listenFd.get() < 0) {
		log << "错误: 无法创建 AF_UNIX 套接字: " << errnoText(errno) << "\n";
		return ListenSocket{FdGuard(), 1};
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socketPath.c_str());
	if (::bind(listenFd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
		log << "错误: 绑定 '" << socketPath << "' 失败: " << errnoText(errno) << "\n";
		return ListenSocket{FdGuard(), 1};
	}
	/* 0600 收紧失败要告警（不阻断）：目录 0700 已挡住其他用户，但静默缺位
	 * 会让「以为收紧了」与「实际靠 umask」漂移。 */
	if (::chmod(socketPath.c_str(), 0600) != 0) {
		log << "警告: socket 文件 chmod 0600 失败（" << errnoText(errno)
			<< "）；文件权限取决于当前 umask，建议检查 " << socketPath << "\n";
	}
	if (::listen(listenFd.get(), 1) < 0) {
		log << "错误: 监听失败: " << errnoText(errno) << "\n";
		::unlink(socketPath.c_str());
		return ListenSocket{FdGuard(), 1};
	}
	return ListenSocket{std::move(listenFd), 0};
}

/* accept 循环（拆自 runMcpBackend，2026-09-26 批次 5）：poll 等连接（带超时以便
 * 轮询 stopRequested）→ accept4 → SO_PEERCRED 校验 → SO_SNDTIMEO 防慢读端 →
 * serveConnection 服务 → 关连接 → 断连安全动作（仅 admitted 连接）。单连接串行。
 * 返回退出码（0 = 收到停止信号正常退出，1 = poll/accept 出错）。 */
int acceptLoop(FdGuard& listenFd, McpBackend& backend, const AgentToolContext& tctx,
		const AgentModeConfig& cfg, const McpBackendOptions& opts, std::ostream& log) {
	int rc = 0;
	/* 首帧认证连续失败门（2026-09-30 审计 P1）：此前同 UID 对端可无限次重连
	 * 试探 token（每次成本仅一个连接周期），与 TCP 口令通道的 5 次爆破上限
	 * 不对称。语义见 AuthFailGate；到顶 = 停止服务（fail-closed，走正常
	 * 停机断电链），操作者重启后端恢复。 */
	AuthFailGate authGate;
	while (!stopRequested()) {
		struct pollfd p{};
		p.fd = listenFd.get();
		p.events = POLLIN;
		const int r = ::poll(&p, 1, kMcpPollIntervalMs);
		if (r < 0) {
			if (errno == EINTR) { continue; }
			log << "MCP 后端：poll 失败: " << errnoText(errno) << "\n";
			rc = 1;
			break;
		}
		if (r == 0) { continue; }	/* 超时：回去检查 stopRequested */
		const int client = ::accept4(listenFd.get(), nullptr, nullptr, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EINTR) { continue; }
			log << "MCP 后端：accept 失败: " << errnoText(errno) << "\n";
			rc = 1;
			break;
		}
		FdGuard clientFd(client);
		if (opts.requirePeerCred && !mcpCheckPeerCred(clientFd.get(), log)) {
			continue;	/* 关掉这条，回去等下一个（可能是扫描/误连） */
		}
		/* 发送超时：防慢读端把 send() 永久阻塞（评估 §7.6 加固清单）。
		 * 设置失败要告警：防慢读端的那条防线静默缺位时，症状只会是将来某次
		 * send 永久挂住——届时可用的第一条线索就是这行告警。 */
		timeval snd{};
		snd.tv_sec = kMcpSendTimeoutSec;
		if (::setsockopt(clientFd.get(), SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)) != 0) {
			log << "警告: SO_SNDTIMEO 设置失败（" << errnoText(errno)
				<< "）；慢读端可能挂住回帧发送\n";
		}

		backend.beginConnection();
		log << "MCP 后端：sidecar 已连接，开始服务\n";
		const ConnOutcome oc = serveConnection(clientFd.get(), backend, opts, log);
		/* 先关连接再跑安全动作：对端若还挂着（半关闭），不该看见动作期间的
		 * 任何回帧；且动作脚本可能跑几秒，不占着 fd。 */
		clientFd.close();
		/* 断连安全动作的门（2026-10-01 审计 P2）：只对「进入过业务循环」的连接
		 * 执行。此前无条件执行——同 UID 对端反复 connect+close（或反复发错
		 * 密钥）可以反复驱动执行器到"安全姿态"，连败 5 次还会借停机断电链把
		 * 后端打停；认证门（MCP-1/AuthFailGate）只盖住了工具派发路径，没盖住
		 * 断连路径。admitted 语义见 ConnOutcome。跳过要留痕：这是安全相关的
		 * 可观测行为，不该静默。 */
		if (oc.admitted) {
			runDisconnectAction(tctx, cfg.onDisconnectScript, cfg.audit, log);
		} else if (!cfg.onDisconnectScript.empty()) {
			log << "MCP 后端：连接未通过认证，跳过断连安全动作 " << cfg.onDisconnectScript
				<< "\n";
		}
		if (oc.auth == ConnAuth::Passed) {
			authGate.recordPassed();
		} else if (oc.auth == ConnAuth::Failed) {
			if (authGate.recordRejected()) {
				log << "MCP 后端：首帧认证连续 " << authGate.max
					<< " 次失败，停止服务（可能为爆破；重启后端恢复，"
					"必要时更换 BOT4CJ_MCP_TOKEN）\n";
				rc = 1;
				break;
			}
			log << "MCP 后端：认证失败（连续 " << authGate.fails << "/"
				<< authGate.max << "），回到 accept 等待\n";
			continue;
		}
		log << "MCP 后端：连接结束，回到 accept 等待\n";
	}
	return rc;
}

} // namespace

int runMcpBackend(RobotStruct& robot, const AgentModeConfig& cfg,
		const McpBackendOptions& opts, std::ostream& log) {
	if (opts.socketPath.empty()) {
		log << "错误: --mcp-backend 需要 --mcp-socket <AF_UNIX 路径>\n";
		return 2;
	}
	if (opts.socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
		log << "错误: --mcp-socket 路径过长（" << opts.socketPath.size() << " 字节，上限 "
			<< (sizeof(sockaddr_un::sun_path) - 1) << "）\n";
		return 2;
	}

	/* 工具层：与 agent 模式共用 makeToolContext + makeAgentTools，能力门控
	 * （--agent-shell / --agent-hot-sql / --read-var / camera）逐字节一致，
	 * 于是 tool.list 天然只宣告已放行的工具（评估 §3.1）。 */
	AgentToolContext tctx = makeToolContext(cfg, robot, log);
	AgentToolRegistry tools = makeAgentTools(tctx);
	McpBackend backend(std::move(tools), cfg.policy, cfg.audit,
		opts.callBudget, opts.wallClockBudgetSec, &cfg, &robot);

	/* 监听套接字准备（父目录 / A-1 探测 / bind / chmod / listen）单点在
	 * prepareListenSocket；失败已写 log，这里只传导退出码。 */
	ListenSocket ls = prepareListenSocket(opts.socketPath, log);
	if (ls.rc != 0) { return ls.rc; }

	backend.writeSessionRecord("start");
	log << "MCP 后端监听 " << opts.socketPath
		<< "（单连接串行；SO_PEERCRED 校验 uid/gid"
		<< (opts.authToken.empty()
			? "；未设 BOT4CJ_MCP_TOKEN，同 UID 直连仅靠 loop 预算兜底"
			: "；首帧共享密钥已启用（BOT4CJ_MCP_TOKEN）")
		<< "；等待 sidecar 连接）\n";

	const int rc = acceptLoop(ls.fd, backend, tctx, cfg, opts, log);

	backend.writeSessionRecord("end");
	::unlink(opts.socketPath.c_str());
	log << "MCP 后端：已停机\n";
	return rc;
}

} // namespace agent
} // namespace bot
