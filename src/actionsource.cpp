#include "actionsource.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace bot {

/* ---- 动作脚本字节上限（kMaxScriptBytes）----
 * 上限常量与理由见 actionsource.hpp（动作/结构脚本共用；RSS 实测见该处注释）。 */

/* ---- TCP 空闲超时缺省值（秒）：连上后不再发字节的对端不能永久挂住进程 ---- */
static constexpr int kTcpIdleSecDefault = 30;

/* ---- TcpStreamReader::read：区分 EOF / 超时 / 信号 / 链路错误 ----
 * 原实现把「出错」与「对端关闭」一律折成 -1（EOF），脚本被静默截断后
 * 只报 "No @Action matrix found"，无从定位；此处分别提示。 */
int TcpStreamReader::read() {
	if (fd_ < 0) { return -1; }
	char c;
	while (true) {
		ssize_t n = ::recv(fd_, &c, 1, 0);
		if (n == 1) { return static_cast<unsigned char>(c); }
		if (n == 0) { return -1; }	/* 对端正常关闭写端 → EOF */
		if (errno == EINTR) {
			/* 信号打断：停止标志置位则按 EOF 收尾，让 deactivate 正常走完 */
			if (stopRequested()) { return -1; }
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (!timedOut_) {
				timedOut_ = true;
				std::cout << "TCP 动作源空闲超过 " << idleSec_
					<< "s，按输入结束处理（对端已连接但未继续发送）" << std::endl;
			}
			return -1;
		}
		std::cout << "TCP 动作源读错误：" << std::strerror(errno)
			<< "，按输入结束处理（脚本可能被截断）" << std::endl;
		return -1;
	}
}

/* ---- 可选口令校验：对端必须先送一行 "<token>\n" 才允许下发动作脚本 ----
 * 口令为空时跳过（不破坏既有单连接直发脚本的用法）。 */
static bool checkTcpToken(int client, const std::string& token) {
	std::string line;
	char c;
	while (line.size() <= token.size() + 1) {
		ssize_t n = ::recv(client, &c, 1, 0);
		if (n != 1) {
			std::cout << "TCP 口令校验失败：对端未提供凭据行" << std::endl;
			return false;
		}
		if (c == '\n') { break; }
		line += c;
	}
	if (line != token) {
		std::cout << "TCP 口令校验失败：凭据不匹配，拒绝接收动作脚本" << std::endl;
		return false;
	}
	std::cout << "TCP 口令校验通过" << std::endl;
	return true;
}

/* ---- TCP 动作源：监听 → 接受一个客户端连接 → 可选口令 → 逐字节读脚本 ---- */
static std::unique_ptr<Reader> openTcpActionSource(long long port, const std::string& host,
		const std::string& token, int idleSec) {
	in_addr addr{};
	if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) {
		std::cout << "--tcp-host 地址无效：" << host << "（需点分十进制 IPv4）" << std::endl;
		return nullptr;
	}

	std::cout << "TCP 动作源：监听 " << host << ":" << port << "，等待客户端连接..." << std::endl;

	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		throw std::runtime_error("无法创建 TCP 套接字");
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr = addr;
	sa.sin_port = htons(static_cast<uint16_t>(port));

	if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
		::close(fd);
		throw std::runtime_error("TCP 监听端口绑定失败");
	}
	/* D5：listen 返回值检查 */
	if (::listen(fd, 1) < 0) {
		::close(fd);
		throw std::runtime_error("TCP 监听失败");
	}

	/* 对外暴露且无口令时显式告警：动作脚本会直接驱动真实舵机 */
	if (addr.s_addr != htonl(INADDR_LOOPBACK) && token.empty()) {
		std::cout << "警告：监听地址 " << host << " 非回环且未设 --tcp-token，"
			"任何可达本端口的对端都能下发动作脚本。请确认这是预期配置。" << std::endl;
	}

	/* D5/D4：accept 可被信号（EINTR）打断——响应停止标志退出，其余 EINTR 重试 */
	int client = -1;
	while (true) {
		client = ::accept(fd, nullptr, nullptr);
		if (client >= 0) { break; }
		if (errno == EINTR) {
			if (stopRequested()) {
				::close(fd);
				throw std::runtime_error("TCP 等待客户端连接被停止信号中断");
			}
			continue;
		}
		::close(fd);
		throw std::runtime_error("TCP 接受客户端失败");
	}
	::close(fd);

	sockaddr_in peer{};
	socklen_t plen = sizeof(peer);
	char ip[INET_ADDRSTRLEN] = "?";
	if (::getpeername(client, reinterpret_cast<sockaddr*>(&peer), &plen) == 0) {
		::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
	}
	std::cout << "TCP 客户端已连接：" << ip << ":" << ntohs(peer.sin_port) << std::endl;

	/* 空闲超时同时覆盖口令校验与脚本读取两段，避免「连上不发」挂死进程 */
	timeval tv{};
	tv.tv_sec = idleSec;
	::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (!token.empty() && !checkTcpToken(client, token)) {
		::close(client);
		throw std::runtime_error("TCP 客户端口令校验未通过");
	}

	return std::make_unique<TcpStreamReader>(client, idleSec);
}

/* ---- 取选项值（缺失返回空串） ---- */
static std::string optValue(const std::map<std::string, std::string>& options, const char* key) {
	auto it = options.find(key);
	return it == options.end() ? std::string() : it->second;
}

/* ---- 解析整数选项：非法即返回 false，由调用方给出人话报错 ---- */
static bool parseLong(const std::string& s, long long& out) {
	char* end = nullptr;
	errno = 0;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0' || errno != 0) { return false; }
	out = v;
	return true;
}

std::unique_ptr<Reader> openActionSource(const std::map<std::string, std::string>& options) {
	std::unique_ptr<Reader> raw;

	/* 1) TCP 通道 */
	if (options.count("tcp")) {
		long long port = -1;
		if (!parseLong(optValue(options, "tcp"), port) || port < 1 || port > 65535) {
			std::cout << "--tcp 端口号无效：" << optValue(options, "tcp")
				<< "（合法范围 1-65535）" << std::endl;
			return nullptr;
		}
		std::string host = optValue(options, "tcp-host");
		if (host.empty()) { host = "127.0.0.1"; }	/* 缺省只监听回环，对外暴露需显式指定 */

		int idleSec = kTcpIdleSecDefault;
		const std::string idleStr = optValue(options, "tcp-idle");
		if (!idleStr.empty()) {
			long long v = 0;
			if (!parseLong(idleStr, v) || v < 1 || v > 86400) {
				std::cout << "--tcp-idle 无效：" << idleStr << "（合法范围 1-86400 秒）" << std::endl;
				return nullptr;
			}
			idleSec = static_cast<int>(v);
		}

		raw = openTcpActionSource(port, host, optValue(options, "tcp-token"), idleSec);
	}
	/* 2) 文件通道 */
	else if (options.count("input")) {
		const std::string& path = options.at("input");
		auto f = std::make_unique<FileReader>(path);
		if (!f->good()) {
			std::cout << "无法打开动作脚本文件 '" << path << "'" << std::endl;
			return nullptr;
		}
		std::cout << "从文件读取动作脚本：" << path << std::endl;
		raw = std::move(f);
	}
	/* 3) 标准输入（管道 cat scripts/xxx.mt | ./build/mybot）
	 * 分块读并在超限处立即拒绝：整体 istreambuf_iterator 读法会在限流之前
	 * 就把全部输入吃进内存，上限形同虚设。 */
	else {
		std::string data;
		char buf[65536];
		for (;;) {
			std::cin.read(buf, static_cast<std::streamsize>(sizeof buf));
			data.append(buf, static_cast<std::size_t>(std::cin.gcount()));
			if (data.size() > kMaxScriptBytes) {
				std::cout << "动作脚本超过 " << kMaxScriptBytes
					<< " 字节上限，已拒绝（正常 .mt 为 KB 级）" << std::endl;
				return nullptr;
			}
			if (std::cin.gcount() < static_cast<std::streamsize>(sizeof buf)) { break; }
		}
		raw = std::make_unique<MemoryReader>(std::move(data));
	}

	if (!raw) { return nullptr; }
	return std::make_unique<LimitedReader>(std::move(raw), kMaxScriptBytes);
}

} // namespace bot
