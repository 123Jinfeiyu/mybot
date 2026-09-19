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

/* ---- TCP 口令连续失败上限（C-M1）：口令失败不再直接终止进程——任意对端
 * 先 connect 一次即可让一次运行崩溃退出（单次性 DoS + 硬件走一遍
 * deactivate）。改为关连接回 accept 等待，连败到上限才放弃（防无限爆破）。 */
static constexpr int kTcpAuthMaxFails = 5;

/* ---- 连接总时长预算（C-M2，秒）：SO_RCVTIMEO 只约束单次 recv，慢滴对端
 * 每 <idle 秒发 1 字节即可击穿空闲超时、把进程挂到天级。从连接建立起算
 * wall-clock 上限：真实脚本 KB 级、秒级传完；600s 已覆盖极慢链路，随
 * --tcp-idle 放大（用户显式调长超时视为有意容忍慢链路），下限 600s。 */
static int tcpTotalBudgetSec(int idleSec) {
	int budget = idleSec * 20;
	if (budget < 600) { budget = 600; }
	return budget;
}

/* ---- fd RAII 守卫（F-25 同族：持 fd 者禁止拷贝）----
 * openTcpActionSource 原有近十条手工 ::close，任一条遗漏即 fd 泄漏；客户端 fd
 * 更在 make_unique 抛出时无人关闭。守卫让每个出口（throw/return/分配失败）自动收口。 */
namespace {

class FdGuard {
	int fd_;

public:
	explicit FdGuard(int fd = -1) : fd_(fd) {}
	~FdGuard() { close(); }
	FdGuard(const FdGuard&) = delete;
	FdGuard& operator=(const FdGuard&) = delete;

	int get() const { return fd_; }

	/* 关闭当前持有的 fd（空则无操作） */
	void close() {
		if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
	}

	/* 接管新 fd：先关闭旧的（accept 重试轮之间用） */
	void reset(int fd) { close(); fd_ = fd; }

	/* 交还所有权：仅在接管方已确定持有该 fd 后调用，此后守卫不再负责关闭 */
	int release() { int f = fd_; fd_ = -1; return f; }
};

} // namespace

/* ---- TcpStreamReader::read：区分 EOF / 超时 / 信号 / 链路错误 ----
 * 内部 64KiB 缓冲（审计 M2）：缓冲区空时一次 recv 尽量填满，之后逐字节
 * 从缓冲出队；8MiB 上限的脚本从 ~800 万次系统调用降到 ~130 次。
 * 错误/超时/信号语义与旧逐字节版完全一致。 */
int TcpStreamReader::read() {
	if (fd_ < 0) { return -1; }
	if (pos_ < buf_.size()) { return static_cast<unsigned char>(buf_[pos_++]); }
	/* C-M2：总时长预算超限——慢滴对端每次 recv 都在 idle 窗口内到达，
	 * 空闲超时永不触发；wall-clock 上限是唯一的硬性防线 */
	if (std::chrono::steady_clock::now() > deadline_) {
		if (!overBudget_) {
			overBudget_ = true;
			std::cerr << "TCP 动作源超出连接总时长预算，按输入结束处理"
				<< "（脚本可能被截断；慢滴发送不能无限期挂住进程）" << std::endl;
		}
		return -1;
	}
	buf_.resize(kBufCap);
	while (true) {
		ssize_t n = ::recv(fd_, buf_.data(), buf_.size(), 0);
		if (n > 0) { pos_ = 0; buf_.resize(static_cast<std::size_t>(n)); return static_cast<unsigned char>(buf_[pos_++]); }
		if (n == 0) { buf_.clear(); return -1; }	/* 对端正常关闭写端 → EOF */
		buf_.clear();
		if (errno == EINTR) {
			/* 信号打断：停止标志置位则按 EOF 收尾，让 deactivate 正常走完 */
			if (stopRequested()) { return -1; }
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (!timedOut_) {
				timedOut_ = true;
				std::cerr << "TCP 动作源空闲超过 " << idleSec_
					<< "s，按输入结束处理（对端已连接但未继续发送）" << std::endl;
			}
			return -1;
		}
		std::cerr << "TCP 动作源读错误：" << std::strerror(errno)
			<< "，按输入结束处理（脚本可能被截断）" << std::endl;
		return -1;
	}
}

/* ---- 可选口令校验：对端必须先送一行 "<token>\n" 才允许下发动作脚本 ----
 * 口令为空时跳过（不破坏既有单连接直发脚本的用法）。
 * 比较走 util.hpp 的 safeEquals 恒时实现（审计 M1）。 */
static bool checkTcpToken(int client, const std::string& token) {
	std::string line;
	char c;
	while (line.size() <= token.size() + 1) {
		ssize_t n = ::recv(client, &c, 1, 0);
		if (n != 1) {
			std::cerr << "TCP 口令校验失败：对端未提供凭据行" << std::endl;
			return false;
		}
		if (c == '\n') { break; }
		line += c;
	}
	if (line.size() > token.size() + 1) {
		/* 审计 #6：凭据行超长，显式 fail-closed 拒绝。复核结论：line 是
		 * std::string 累积、safeEquals 带长度比较，超长行本就因长度不等
		 * 被拒，不存在截断放行；此处提前拦截只为给出明确诊断（而非笼统
		 * 的「凭据不匹配」），并让 +1 的循环上限语义自解释。 */
		std::cerr << "TCP 口令校验失败：凭据行超长（fail-closed，不截断比较）" << std::endl;
		return false;
	}
	if (!safeEquals(line, token)) {
		std::cerr << "TCP 口令校验失败：凭据不匹配，拒绝接收动作脚本" << std::endl;
		return false;
	}
	std::cerr << "TCP 口令校验通过" << std::endl;
	return true;
}

/* ---- TCP 动作源：监听 → 接受一个客户端连接 → 可选口令 → 逐字节读脚本 ---- */
static std::unique_ptr<Reader> openTcpActionSource(long long port, const std::string& host,
		const std::string& token, int idleSec) {
	in_addr addr{};
	if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) {
		std::cerr << "--tcp-host 地址无效：" << host << "（需点分十进制 IPv4）" << std::endl;
		return nullptr;
	}

	std::cerr << "TCP 动作源：监听 " << host << ":" << port << "，等待客户端连接..." << std::endl;

	/* 监听 fd 交给守卫：此后所有出口（throw / return / 客户端的 make_unique
	 * 分配失败）都自动关闭，不再依赖逐条手工 ::close。 */
	FdGuard listenFd(::socket(AF_INET, SOCK_STREAM, 0));
	if (listenFd.get() < 0) {
		throw std::runtime_error("无法创建 TCP 套接字");
	}
	int one = 1;
	::setsockopt(listenFd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/* 审计 H-2 防线：非回环监听必须设口令（缺省即拒绝 bind，不再仅告警）。
	 * 动作脚本会直接驱动真实硬件，未鉴权暴露到网络等价于把舵机交给任何
	 * 可达本端口的对端。回环 127.0.0.1 不受限。 */
	if (addr.s_addr != htonl(INADDR_LOOPBACK) && token.empty()) {
		throw std::runtime_error("监听地址 " + host + " 非回环且未设 --tcp-token："
			"动作脚本会直接驱动真实硬件，跨机监听必须显式提供口令"
			"（仅本机调试请用缺省回环地址 127.0.0.1）");
	}

	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr = addr;
	sa.sin_port = htons(static_cast<uint16_t>(port));

	if (::bind(listenFd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
		throw std::runtime_error("TCP 监听端口绑定失败");
	}
	/* D5：listen 返回值检查 */
	if (::listen(listenFd.get(), 1) < 0) {
		throw std::runtime_error("TCP 监听失败");
	}

	/* D5/D4：accept 可被信号（EINTR）打断——响应停止标志退出，其余 EINTR 重试。
	 * C-M1：口令失败不再 throw 终止进程（任意对端 connect 一次即 DoS）——
	 * 关连接回 accept 等待，连续 kTcpAuthMaxFails 次失败才放弃。 */
	FdGuard clientFd;		/* 每轮重置；本轮失败时由 close() 关闭 */
	int authFails = 0;
	while (true) {
		int client = ::accept(listenFd.get(), nullptr, nullptr);
		if (client < 0) {
			if (errno == EINTR) {
				if (stopRequested()) {
					throw std::runtime_error("TCP 等待客户端连接被停止信号中断");
				}
				continue;
			}
			throw std::runtime_error("TCP 接受客户端失败");
		}
		clientFd.reset(client);

		sockaddr_in peer{};
		socklen_t plen = sizeof(peer);
		char ip[INET_ADDRSTRLEN] = "?";
		if (::getpeername(clientFd.get(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0) {
			::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		}
		std::cerr << "TCP 客户端已连接：" << ip << ":" << ntohs(peer.sin_port) << std::endl;

		/* 空闲超时同时覆盖口令校验与脚本读取两段，避免「连上不发」挂死进程。
		 * 审计 #9：设置失败必须显式失败——该选项正是空闲 DoS 的防线，静默
		 * 失效等于防线整体消失。 */
		timeval tv{};
		tv.tv_sec = idleSec;
		if (::setsockopt(clientFd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
			throw std::runtime_error(std::string("设置 TCP 接收空闲超时失败：")
				+ std::strerror(errno));
		}

		if (!token.empty() && !checkTcpToken(clientFd.get(), token)) {
			clientFd.close();	/* 关连接，回 accept 等待下一个 */
			++authFails;
			if (authFails >= kTcpAuthMaxFails) {
				throw std::runtime_error("TCP 口令连续 " + std::to_string(authFails)
					+ " 次校验失败，停止等待（可能为扫描/爆破）");
			}
			std::cerr << "关闭该连接，继续等待下一个客户端（剩余尝试 "
				<< (kTcpAuthMaxFails - authFails) << " 次）" << std::endl;
			continue;
		}
		break;
	}

	/* C-M2：总时长预算从连接建立起算（含口令校验段；口令行长度有上限且
	 * 每次 recv 受 idle 超时约束，主防线覆盖无界的脚本读取段） */
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(tcpTotalBudgetSec(idleSec));

	/* 先以 get() 构造（此刻仍由守卫持有），构造成功后才交还所有权：
	 * 若构造或分配抛异常，守卫负责关闭客户端 fd，不再泄漏（原实现此处
	 * 把裸 client 交给 make_unique，分配失败即泄漏）。 */
	auto reader = std::make_unique<TcpStreamReader>(clientFd.get(), idleSec, deadline);
	clientFd.release();
	return reader;
}

/* ---- 打开动作输入源：tcp 优先 > 文件 > 标准输入 ---- */
std::unique_ptr<Reader> openActionSource(const CliOptions& options) {
	std::unique_ptr<Reader> raw;

	/* 1) TCP 通道 */
	if (options.tcp) {
		/* 审计中-3：裸 --tcp（漏写端口号）在此明确报错，而非静默按端口 1
		 * 走到 bind 特权端口失败、给出与用户意图相去甚远的错误 */
		if (options.tcpPort.empty()) {
			std::cerr << "--tcp 需要端口号，例如 --tcp 9000（合法范围 1-65535）" << std::endl;
			return nullptr;
		}
		auto portParsed = parseLongStrict(options.tcpPort);
		if (portParsed.first != IntParse::OK || portParsed.second < 1 || portParsed.second > 65535) {
			std::cerr << "--tcp 端口号无效：" << options.tcpPort
				<< "（合法范围 1-65535）" << std::endl;
			return nullptr;
		}
		std::string host = options.tcpHost;
		if (host.empty()) { host = "127.0.0.1"; }	/* 缺省只监听回环，对外暴露需显式指定 */

		int idleSec = kTcpIdleSecDefault;
		const std::string& idleStr = options.tcpIdle;
		if (!idleStr.empty()) {
			auto idleParsed = parseLongStrict(idleStr);
			if (idleParsed.first != IntParse::OK || idleParsed.second < 1 || idleParsed.second > 86400) {
				std::cerr << "--tcp-idle 无效：" << idleStr << "（合法范围 1-86400 秒）" << std::endl;
				return nullptr;
			}
			idleSec = static_cast<int>(idleParsed.second);
		}

		/* C-L4：口令可经环境变量传递——--tcp-token 出现在 /proc/<pid>/cmdline，
		 * 多用户机器上凭据对同机其他用户可见；环境变量只在进程环境里。 */
		std::string token = options.tcpToken;
		if (token.empty()) {
			if (const char* envToken = std::getenv("BOT4CJ_TCP_TOKEN")) { token = envToken; }
		}

		raw = openTcpActionSource(portParsed.second, host, token, idleSec);
	}
	/* 2) 文件通道 */
	else if (options.input) {
		const std::string& path = *options.input;
		auto f = std::make_unique<FileReader>(path);
		if (!f->good()) {
			std::cerr << "无法打开动作脚本文件 '" << path << "'" << std::endl;
			return nullptr;
		}
		std::cerr << "从文件读取动作脚本：" << path << std::endl;
		raw = std::move(f);
	}
	/* 3) 标准输入（管道 cat scripts/xxx.mt | ./build/mybot）
	 * 分块读 + 超限即抛（readAllLimited 单点，与 mtmerge 文件输入共用）：
	 * 整体 istreambuf_iterator 读法会在限流之前就把全部输入吃进内存，
	 * 上限形同虚设。超限从「打印后放弃」改为抛 ScriptTooLarge——与文件
	 * 通道（LimitedReader 同一异常）统一为同一用户可见契约。 */
	else {
		raw = std::make_unique<MemoryReader>(readAllLimited(std::cin, kMaxScriptBytes));
	}

	if (!raw) { return nullptr; }
	return std::make_unique<LimitedReader>(std::move(raw), kMaxScriptBytes);
}

} // namespace bot
