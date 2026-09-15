#include "actionsource.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bot {

/* ---- TCP 动作源：0.0.0.0 监听 → 接受一个客户端连接 ---- */
static std::unique_ptr<Reader> openTcpActionSource(long long port) {
	std::cout << "TCP 动作源：监听 0.0.0.0:" << port << "，等待客户端连接..." << std::endl;

	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		throw std::runtime_error("无法创建 TCP 套接字");
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(static_cast<uint16_t>(port));

	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error("TCP 监听端口绑定失败");
	}
	/* D5：listen 返回值检查 */
	if (::listen(fd, 1) < 0) {
		::close(fd);
		throw std::runtime_error("TCP 监听失败");
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

	return std::make_unique<TcpStreamReader>(client);
}

std::unique_ptr<Reader> openActionSource(const std::map<std::string, std::string>& options) {
	/* 1) TCP 通道 */
	if (options.count("tcp")) {
		const std::string& portStr = options.at("tcp");
		long long port = -1;
		char* end = nullptr;
		long long p = std::strtoll(portStr.c_str(), &end, 10);
		if (end != portStr.c_str() && *end == '\0') { port = p; }
		if (port < 1 || port > 65535) {
			std::cout << "--tcp 端口号无效：" << portStr << "（合法范围 1-65535）" << std::endl;
			return nullptr;
		}
		return openTcpActionSource(port);
	}

	/* 2) 文件通道 */
	if (options.count("input")) {
		const std::string& path = options.at("input");
		auto f = std::make_unique<FileReader>(path);
		if (!f->good()) {
			std::cout << "无法打开动作脚本文件 '" << path << "'" << std::endl;
			return nullptr;
		}
		std::cout << "从文件读取动作脚本：" << path << std::endl;
		return f;
	}

	/* 3) 标准输入（管道 cat scripts/xxx.mt | ./build/mybot） */
	std::string data((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
	return std::make_unique<MemoryReader>(std::move(data));
}

} // namespace bot