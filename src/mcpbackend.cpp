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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "agentmode.hpp"	/* AgentModeConfig / makeToolContext / structureDisplayName */
#include "agentprompt.hpp"	/* structureScriptHead / quoteUntrusted（context.get 素材） */
#include "jsonparse.hpp"	/* parseJson */
#include "robot.hpp"		/* RobotStruct 完整定义（context.get 的 registry()） */
#include "util.hpp"		/* stopRequested / isValidUtf8 / FdGuard */

namespace fs = std::filesystem;

namespace bot {
namespace agent {

namespace {

/* 主线程轮询粒度：accept 与逐帧接收共用（200ms = 对停止信号的响应上限，
 * 不是超时——空闲时不断连，靠 deadline 收口）。 */
constexpr int kPollIntervalMs = 200;
/* 发送超时：防慢读端把 send() 永久阻塞（评估 §7.6 加固清单）。 */
constexpr int kSendTimeoutSec = 5;

using Clock = std::chrono::steady_clock;

void writeBe32(char* out, std::uint32_t v) {
	out[0] = static_cast<char>((v >> 24) & 0xFFu);
	out[1] = static_cast<char>((v >> 16) & 0xFFu);
	out[2] = static_cast<char>((v >> 8) & 0xFFu);
	out[3] = static_cast<char>(v & 0xFFu);
}

std::uint32_t readBe32(const char* in) {
	return (static_cast<std::uint32_t>(static_cast<unsigned char>(in[0])) << 24)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(in[1])) << 16)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(in[2])) << 8)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(in[3])));
}

/* 错误串脱敏：把「以 / 开头的非空白串」（绝对路径形态）替换为 <redacted>，
 * 与 libmsg RpcServer 的 _sanitize_error 同意图（评估 §7.4 第 5 条）。完整异常
 * 另进后端日志/审计，返回客户端的只有脱敏串。 */
std::string sanitizeError(const std::string& in) {
	std::string out;
	out.reserve(in.size());
	std::size_t i = 0;
	while (i < in.size()) {
		if (in[i] == '/') {
			std::size_t j = i + 1;
			while (j < in.size()
				&& std::isspace(static_cast<unsigned char>(in[j])) == 0) { ++j; }
			out += "<redacted>";
			i = j;
		} else {
			out += in[i];
			++i;
		}
	}
	return out;
}

/* deadline 模式收满 n 字节：帧头与载荷共享同一绝对 deadline（防 Slowloris）。
 * 返回 true=收满；false=超时/错误/对端关闭（关闭时置 eof）。 */
bool recvAllDeadline(int fd, char* buf, std::size_t n, Clock::time_point deadline, bool& eof) {
	std::size_t got = 0;
	while (got < n) {
		const auto now = Clock::now();
		if (now >= deadline) { return false; }
		const long remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - now).count();
		struct pollfd p{};
		p.fd = fd;
		p.events = POLLIN;
		const int r = ::poll(&p, 1, remainMs > 0 ? static_cast<int>(remainMs) : 0);
		if (r < 0) {
			if (errno == EINTR) { continue; }
			return false;
		}
		if (r == 0) { return false; }	/* 超时 */
		if ((p.revents & (POLLERR | POLLNVAL)) != 0) { return false; }
		const ssize_t k = ::recv(fd, buf + got, n - got, 0);
		if (k == 0) { eof = true; return false; }	/* 对端关闭 */
		if (k < 0) {
			if (errno == EINTR) { continue; }
			return false;
		}
		got += static_cast<std::size_t>(k);
	}
	return true;
}

enum class FrameResult { Ok, PeerClosed, Violation };

/* 读一帧。两段式：
 *   ① 空闲等首字节**不设** deadline（这是长生命周期控制通道，两次工具调用之间
 *      可能长时间空闲）——只周期性 poll 以便响应停止信号；
 *   ② 一旦帧开始（首字节到达），帧头与载荷共享 recvTimeoutSec 的绝对 deadline，
 *      这正是 Slowloris 的修复点（评估 §7.1）。 */
FrameResult readFrame(int fd, std::string& payload, int recvTimeoutSec, std::ostream& log) {
	for (;;) {
		if (stopRequested()) { return FrameResult::PeerClosed; }
		struct pollfd p{};
		p.fd = fd;
		p.events = POLLIN;
		const int r = ::poll(&p, 1, kPollIntervalMs);
		if (r < 0) {
			if (errno == EINTR) { continue; }
			return FrameResult::Violation;
		}
		if (r == 0) { continue; }	/* 空闲，继续等（不断连） */
		if ((p.revents & (POLLERR | POLLNVAL)) != 0) { return FrameResult::Violation; }
		break;	/* 可读（或 HUP/RDHUP → 下面 recv 得 0 → PeerClosed） */
	}

	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(recvTimeoutSec);
	char hdr[kMcpHeaderSize] = {0};
	bool eof = false;
	if (!recvAllDeadline(fd, hdr, kMcpHeaderSize, deadline, eof)) {
		return eof ? FrameResult::PeerClosed : FrameResult::Violation;
	}
	const std::uint32_t len = readBe32(hdr);
	/* 上限在**分配载荷缓冲之前**检查（评估 §7.1）；u32 直接比较，无溢出暴露面。 */
	if (static_cast<std::size_t>(len) > kMcpMaxFrameBytes) {
		log << "MCP 后端：帧载荷 " << len << " 字节超过上限 " << kMcpMaxFrameBytes
			<< "，断开连接\n";
		return FrameResult::Violation;
	}
	payload.assign(static_cast<std::size_t>(len), '\0');
	if (len > 0
		&& !recvAllDeadline(fd, &payload[0], static_cast<std::size_t>(len), deadline, eof)) {
		return eof ? FrameResult::PeerClosed : FrameResult::Violation;
	}
	if (!isValidUtf8(payload)) {
		log << "MCP 后端：帧载荷不是合法 UTF-8，断开连接\n";
		return FrameResult::Violation;
	}
	return FrameResult::Ok;
}

bool sendFrame(int fd, const std::string& payload) {
	const std::string frame = mcpEncodeFrame(payload);
	std::size_t sent = 0;
	while (sent < frame.size()) {
		const ssize_t k = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
		if (k < 0) {
			if (errno == EINTR) { continue; }
			return false;
		}
		if (k == 0) { return false; }
		sent += static_cast<std::size_t>(k);
	}
	return true;
}

/* 服务单条连接：帧循环直到对端关闭 / 协议违规 / 停止信号。 */
void serveConnection(int fd, McpBackend& backend, const McpBackendOptions& opts,
		std::ostream& log) {
	while (!stopRequested()) {
		std::string payload;
		const FrameResult fr = readFrame(fd, payload, opts.recvTimeoutSec, log);
		if (fr == FrameResult::PeerClosed) { return; }
		if (fr == FrameResult::Violation) {
			log << "MCP 后端：协议违规，断开连接\n";
			return;
		}
		const std::string resp = backend.handleFrame(payload);
		if (!sendFrame(fd, resp)) {
			log << "MCP 后端：回帧发送失败，断开连接\n";
			return;
		}
	}
}

} // namespace

/* ---- 纯协议件 ---- */

std::string mcpEncodeFrame(const std::string& payload) {
	std::string frame(kMcpHeaderSize + payload.size(), '\0');
	writeBe32(&frame[0], static_cast<std::uint32_t>(payload.size()));
	if (!payload.empty()) {
		std::memcpy(&frame[kMcpHeaderSize], payload.data(), payload.size());
	}
	return frame;
}

bool mcpCheckPeerCred(int fd, std::ostream& log) {
	struct ucred cred{};
	socklen_t len = sizeof(cred);
	if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
		log << "MCP 后端：SO_PEERCRED 读取失败（" << std::strerror(errno)
			<< "），拒绝连接\n";
		return false;
	}
	const uid_t uid = ::getuid();
	const gid_t gid = ::getgid();
	if (cred.uid != uid || cred.gid != gid) {
		log << "MCP 后端：对端 uid/gid(" << cred.uid << "/" << cred.gid
			<< ") 与本进程(" << uid << "/" << gid << ") 不符，拒绝连接\n";
		return false;
	}
	return true;
}

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
	 * 有配置组装期的成品（structureHead / scriptsList）就用它，没有才实时读。
	 * 区别只在呈现：这里返回**已定界的块**，由 Rust agent 拼进它自己的系统
	 * 提示词——「同一份磁盘内容，两条进上下文的通道必须有同一定界强度」。 */
	const std::string sql = cfg_->structureHead ? *cfg_->structureHead
			: structureScriptHead(cfg_->structurePath, kStructureHeadBytes);
	const std::string scripts = cfg_->scriptsList ? *cfg_->scriptsList
			: describeActionScripts(cfg_->scriptsDir);

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
		/* LuaJIT 用 C++ 异常实现 luaD_throw，非 std::exception 也要接住
		 * （与 DispatchGate::run 的 catch(...) 同理），绝不让异常穿出 handler。 */
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

/* ---- socket 服务 ---- */

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

	/* 父目录：缺失则以 0700 建（单级）；已存在但权限过松则告警（不静默）。 */
	const fs::path sp(opts.socketPath);
	fs::path parent = sp.parent_path();
	if (parent.empty()) { parent = "."; }
	std::error_code ec;
	if (!fs::exists(parent, ec)) {
		if (::mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST) {
			log << "错误: 无法创建 socket 父目录 '" << parent.string() << "': "
				<< std::strerror(errno) << "\n";
			return 2;
		}
	} else {
		struct stat st{};
		if (::stat(parent.c_str(), &st) == 0 && (st.st_mode & 0077) != 0) {
			log << "警告: socket 父目录 '" << parent.string()
				<< "' 权限过松（建议 0700）；同机其他用户可能连上后端\n";
		}
	}

	::unlink(opts.socketPath.c_str());	/* 清掉陈旧 socket（ENOENT 忽略） */

	FdGuard listenFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
	if (listenFd.get() < 0) {
		log << "错误: 无法创建 AF_UNIX 套接字: " << std::strerror(errno) << "\n";
		return 1;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", opts.socketPath.c_str());
	if (::bind(listenFd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
		log << "错误: 绑定 '" << opts.socketPath << "' 失败: " << std::strerror(errno) << "\n";
		return 1;
	}
	::chmod(opts.socketPath.c_str(), 0600);	/* socket 文件 0600 */
	if (::listen(listenFd.get(), 1) < 0) {
		log << "错误: 监听失败: " << std::strerror(errno) << "\n";
		::unlink(opts.socketPath.c_str());
		return 1;
	}

	backend.writeSessionRecord("start");
	log << "MCP 后端监听 " << opts.socketPath
		<< "（单连接串行；SO_PEERCRED 校验 uid/gid；等待 sidecar 连接）\n";

	int rc = 0;
	while (!stopRequested()) {
		struct pollfd p{};
		p.fd = listenFd.get();
		p.events = POLLIN;
		const int r = ::poll(&p, 1, kPollIntervalMs);
		if (r < 0) {
			if (errno == EINTR) { continue; }
			log << "MCP 后端：poll 失败: " << std::strerror(errno) << "\n";
			rc = 1;
			break;
		}
		if (r == 0) { continue; }	/* 超时：回去检查 stopRequested */
		const int client = ::accept4(listenFd.get(), nullptr, nullptr, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EINTR) { continue; }
			log << "MCP 后端：accept 失败: " << std::strerror(errno) << "\n";
			rc = 1;
			break;
		}
		FdGuard clientFd(client);
		if (opts.requirePeerCred && !mcpCheckPeerCred(clientFd.get(), log)) {
			continue;	/* 关掉这条，回去等下一个（可能是扫描/误连） */
		}
		/* 发送超时：防慢读端把 send() 永久阻塞（评估 §7.6 加固清单）。 */
		timeval snd{};
		snd.tv_sec = kSendTimeoutSec;
		::setsockopt(clientFd.get(), SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

		backend.beginConnection();
		log << "MCP 后端：sidecar 已连接，开始服务\n";
		serveConnection(clientFd.get(), backend, opts, log);
		log << "MCP 后端：连接结束，回到 accept 等待\n";
	}

	backend.writeSessionRecord("end");
	::unlink(opts.socketPath.c_str());
	log << "MCP 后端：已停机\n";
	return rc;
}

} // namespace agent
} // namespace bot
