/* mcptransport.cpp —— MCP 后端的帧传输与纯协议件（拆自 mcpbackend.cpp，2026-10-01）
 * 内容：4 字节 BE 长度前缀帧的编解码与 deadline 收发（防 Slowloris）、
 * 首帧共享密钥校验、SO_PEERCRED 对端校验、启动前 socket 路径占用探测。
 * mcpbackend.cpp 留连接编排（serveConnection/acceptLoop/runMcpBackend）与
 * McpBackend 路由；跨 TU 接缝（FrameResult/readFrame/sendFrame/常量）在
 * mcpbackend.hpp。 */

#include "mcpbackend.hpp"

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "jsonparse.hpp"	/* parseJson */
#include "util_env.hpp"		/* stopRequested / errnoText */
#include "util_file.hpp"	/* FdGuard */
#include "util_str.hpp"		/* safeEquals（凭据比对安全单点） */
#include "util_utf8.hpp"	/* isValidUtf8 */

namespace bot {
namespace agent {

namespace {

/* 启动探测的 connect 等待上限（A-1）：AF_UNIX 正常路径用不满它，只挡
 * 「既不接也不拒」的异常对端把启动挂死。 */
constexpr int kProbeWaitMs = 500;

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

/* 读一帧。两段式：
 *   ① 空闲等首字节：长生命周期控制通道，两次工具调用之间可能长时间空闲，
 *      缺省不设 deadline——但 idleDeadline（--mcp-idle-timeout 与每连接墙钟
 *      预算的较早者，见 McpBackend::idleDeadline）给了值就到点断连（A-2，
 *      2026-09-25 审计：哑对端不能永远占住单连接串行模型下唯一的 accept 槽）；
 *      周期性 poll 以便响应停止信号；
 *   ② 一旦帧开始（首字节到达），帧头与载荷共享 recvTimeoutSec 的绝对 deadline，
 *      这正是 Slowloris 的修复点（评估 §7.1）。 */
FrameResult readFrame(int fd, std::string& payload, int recvTimeoutSec,
		Clock::time_point idleDeadline, std::ostream& log) {
	for (;;) {
		if (stopRequested()) { return FrameResult::PeerClosed; }
		if (Clock::now() >= idleDeadline) { return FrameResult::IdleTimeout; }
		struct pollfd p{};
		p.fd = fd;
		p.events = POLLIN;
		const int r = ::poll(&p, 1, kMcpPollIntervalMs);
		if (r < 0) {
			if (errno == EINTR) { continue; }
			return FrameResult::Violation;
		}
		if (r == 0) { continue; }	/* 空闲，继续等（deadline 在循环顶收口） */
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

/* ---- 首帧共享密钥校验（MCP-1，审计 P2-1）----
 * 恒时比对收敛到 util_str.hpp 的 safeEquals 单点（2026-10-01）：此处取
 * 严格版语义——长度差异折叠进累计差，全长走完才判，回应时间不泄露已
 * 匹配前缀的信息。TCP 口令侧（actionsource）同一单点。 */
McpAuthOutcome mcpCheckAuthFrame(const std::string& expected, const std::string& payload) {
	Json req;
	try {
		req = parseJson(payload);
	} catch (const std::exception&) {
		return McpAuthOutcome::Failed;
	}
	if (!req.isObj()) { return McpAuthOutcome::Failed; }
	const Json* route = req.find("_route");
	if (route == nullptr || !route->isStr() || route->getValue() != "auth") {
		return McpAuthOutcome::NotAuthFrame;
	}
	const Json* token = req.find("token");
	if (token == nullptr || !token->isStr()) { return McpAuthOutcome::Failed; }
	return safeEquals(expected, token->getValue())
		? McpAuthOutcome::Ok : McpAuthOutcome::Failed;
}

bool mcpCheckPeerCred(int fd, std::ostream& log) {
	struct ucred cred{};
	socklen_t len = sizeof(cred);
	if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
		log << "MCP 后端：SO_PEERCRED 读取失败（" << errnoText(errno)
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

McpSocketProbe mcpProbeSocketPath(const std::string& path, std::string& why) {
	struct stat st{};
	if (::lstat(path.c_str(), &st) != 0) {
		if (errno == ENOENT) { return McpSocketProbe::PathFree; }	/* 路径还没人用 */
		why = std::string("lstat 失败: ") + errnoText(errno);
		return McpSocketProbe::PathUnusable;
	}
	if (!S_ISSOCK(st.st_mode)) {
		why = "路径已存在但不是 socket 文件（可能是普通文件/目录——不替操作者删除）";
		return McpSocketProbe::PathUnusable;
	}

	/* 存在 socket 文件：connect 一次区分「活监听」与「陈旧残留」。
	 * 非阻塞 + 短 poll：AF_UNIX 的 connect 通常立刻完成，但活实例的积压满
	 * （listen(1) 且槽位被哑连接占着）时阻塞 connect 会无限等——探测自己
	 * 不能被它挂死。 */
	FdGuard probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
	if (probe.get() < 0) {
		why = std::string("探测 socket 创建失败: ") + errnoText(errno);
		return McpSocketProbe::PathUnusable;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
	const int c = ::connect(probe.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (c == 0) {
		why = "已有另一个实例在监听（connect 直接成功）";
		return McpSocketProbe::PathBusy;
	}
	if (errno == ECONNREFUSED) {
		return McpSocketProbe::PathFree;	/* 陈旧残留：上次实例没清理，可安全覆盖 */
	}
	if (errno == ENOENT) {
		/* lstat 与 connect 之间被人清掉：与不存在同等对待 */
		return McpSocketProbe::PathFree;
	}
	if (errno == EINPROGRESS || errno == EAGAIN) {
		/* EAGAIN（Linux 对积压满的 uds 非阻塞 connect）= 活实例但暂时接不进；
		 * EINPROGRESS 走 poll 短等确认。两者都**按忙处理**：宁可拒绝启动，
		 * 不抢一个可能在用的路径（fail-closed）。 */
		if (errno == EAGAIN) {
			why = "已有实例在监听（对端积压满，connect 返回 EAGAIN）";
			return McpSocketProbe::PathBusy;
		}
		struct pollfd p{};
		p.fd = probe.get();
		p.events = POLLOUT;
		int r = ::poll(&p, 1, kProbeWaitMs);
		if (r < 0 && errno == EINTR) { r = 0; }
		if (r == 0) {
			why = "探测超时（路径上有 socket 但既不接也不拒——按活实例处理）";
			return McpSocketProbe::PathBusy;
		}
		int soErr = 0;
		socklen_t sl = sizeof(soErr);
		if (r > 0 && ::getsockopt(probe.get(), SOL_SOCKET, SO_ERROR, &soErr, &sl) == 0) {
			if (soErr == 0) {
				why = "已有另一个实例在监听（connect 完成）";
				return McpSocketProbe::PathBusy;
			}
			if (soErr == ECONNREFUSED) { return McpSocketProbe::PathFree; }
			why = std::string("connect 异步失败: ") + errnoText(soErr);
			return McpSocketProbe::PathUnusable;
		}
		why = std::string("connect 结果无法判定: ") + errnoText(errno);
		return McpSocketProbe::PathUnusable;
	}
	why = std::string("connect 失败: ") + errnoText(errno);
	return McpSocketProbe::PathUnusable;
}

} // namespace agent
} // namespace bot
