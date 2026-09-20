/* pipe2 需要 _GNU_SOURCE（glibc 仅在 __USE_GNU 下声明）。g++/clang 编 C++ 时通常
 * 已定义，此处显式兜底并置于任何头文件之前（重复定义会触发 -Wpedantic 告警）。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* agent_subprocess.cpp —— run_command 的子进程实现（拆自 agenttools.cpp）
 * 超时 / 进程组终止 / 输出截断 / 环境清洗。 */

#include "agent_subprocess.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "util.hpp"

/* 进程环境块。glibc 不在 <unistd.h> 里声明它，必须在**全局作用域**声明：
 * 放进匿名命名空间会变成另一个符号（bot::agent::{anon}::environ），链接期
 * 报 undefined reference。 */
extern char** environ;

namespace bot {
namespace agent {

void appendCapped(std::string& dst, const char* p, std::size_t n, bool& truncated) {
	std::size_t room = kMaxCommandOutput > dst.size() ? kMaxCommandOutput - dst.size() : 0;
	if (n > room) {
		dst.append(p, room);
		truncated = true;
	} else {
		dst.append(p, n);
	}
}

/* 敏感词：按**整词**匹配（词 = 名字里连续的非字母数字分隔出的片段）。
 * 旧实现是子串匹配，"KEY" 会连带剔除 KEYSTONE / MONKEY / HOCKEY / TOKENIZER——
 * 误伤与漏网同时存在（"APIKEY" / "PGPASSWORD" 这类无分隔符的复合名又恰好
 * 逃过词表？不，旧实现会命中；改成整词后它们必须显式列举）。 */
static bool isSensitiveWord(const std::string& word) {
	static const char* kWORDS[] = {"key", "token", "secret", "password", "passwd",
				       "credential"};
	for (const char* w : kWORDS) {
		if (word == w) { return true; }
	}
	/* 不含分隔符的已知复合密钥名：整词匹配抓不到，又不能退回子串匹配
	 * （会误伤 keystone/monkey），故显式列举——宁可短表可控，也不做模糊匹配。 */
	static const char* kCOMPOUNDS[] = {"apikey", "accesskey", "accesskeyid", "secretkey",
					   "clientsecret", "privatekey", "authkey", "authtoken",
					   "accesstoken", "refreshtoken", "pgpassword", "passphrase"};
	for (const char* w : kCOMPOUNDS) {
		if (word == w) { return true; }
	}
	return false;
}

namespace {

/* 敏感名判据：按分隔符切词后逐词查表（BOT4CJ_LLM_KEY → {bot4cj, llm, key} →
 * 命中 key）。buildCleanEnvp 与 collectEnvSecrets 共用同一份，否则「子进程清洗
 * 名单」与「宿主抹除名单」会各自漂移。 */
bool isSensitiveEnvName(const std::string& lowerName) {
	std::size_t i = 0;
	while (i < lowerName.size()) {
		if (!std::isalnum(static_cast<unsigned char>(lowerName[i]))) { ++i; continue; }
		std::size_t j = i;
		while (j < lowerName.size() && std::isalnum(static_cast<unsigned char>(lowerName[j]))) { ++j; }
		if (isSensitiveWord(lowerName.substr(i, j - i))) { return true; }
		i = j;
	}
	return false;
}

/* 环境变量名（'=' 之前）转小写；无 '=' 的畸形条目返回空串表示跳过 */
std::string lowerEnvName(const char* entry) {
	std::string_view s(entry);
	std::size_t eq = s.find('=');
	if (eq == std::string_view::npos) { return {}; }
	std::string name(s.substr(0, eq));
	std::transform(name.begin(), name.end(), name.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return name;
}

} // namespace

/* 环境清洗：剔除名字**含敏感词（整词）**的变量。模型可能被外部输入（画面/文件
 * 内容）诱导执行任意命令，不能让子进程继承 LLM API key。
 * 判定按分隔符切词，因此不误伤 KEYSTONE/MONKEY；无分隔符的复合名由
 * isSensitiveWord 的复合表覆盖（APIKEY / PGPASSWORD 仍被剔除）。 */
std::vector<char*> buildCleanEnvp() {
	std::vector<char*> envp;
	for (char** e = ::environ; e && *e; ++e) {
		if (!isSensitiveEnvName(lowerEnvName(*e))) { envp.push_back(*e); }
	}
	envp.push_back(nullptr);
	return envp;
}

std::vector<char*> collectEnvSecrets() {
	std::vector<char*> out;
	for (char** e = ::environ; e && *e; ++e) {
		if (isSensitiveEnvName(lowerEnvName(*e))) { out.push_back(*e); }
	}
	return out;
}

/* 逐字节抹零。用 volatile 目的指针，防止编译器把「之后不再读取」的存储优化掉
 * （被优化掉就等于没抹）。
 * 抹的是整条 "NAME=VALUE"（连名字一起），而 glibc getenv 按 "NAME=" 前缀匹配，
 * 于是数组槽位没被 setenv 换掉的那些变量会直接从环境里**消失**（getenv 返回
 * NULL）。这不影响取值：密钥一律以 BOT4CJ_LLM_KEY 重新注入（堆上的新串，槽位已
 * 替换），而驱动按 BOT4CJ_LLM_KEY → ROBOT_AGENT_API_KEY → DEEPSEEK_API_KEY 的
 * 同一顺序取值，排第一的那个一定有值。 */
void scrubEnvSecrets(const std::vector<char*>& pointers) {
	for (char* p : pointers) {
		if (p == nullptr) { continue; }
		const std::size_t n = std::strlen(p);
		volatile char* v = p;
		for (std::size_t i = 0; i < n; ++i) { v[i] = '\0'; }
	}
}

/* 有界回收：绝不无超时阻塞——kill 未生效（D 态 / 权限）时无超时 waitpid 会把
 * agent 会话永久挂死。WNOHANG 轮询到固定宽限，仍未回收即放弃（交由 init 收尸）。
 * EINTR 经 retryEintr 重试（阶段 4.2）：旧实现把 waitpid 的 -1/EINTR 当
 * 「已无可回收」直接 break，于是留下僵尸且 exitCode 停在 -1。 */
static bool reapWithGrace(pid_t pid, int& status, int graceMs = 2000) {
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(graceMs);
	for (;;) {
		pid_t w = bot::retryEintr([&] { return ::waitpid(pid, &status, WNOHANG); });
		if (w == pid) { return true; }
		if (w < 0) { return false; }	/* ECHILD 等：确已无可回收 */
		if (std::chrono::steady_clock::now() >= deadline) { return false; }
		::poll(nullptr, 0, 10);		/* 10ms 退避，避免忙等 */
	}
}

/* 子进程 exec 前吊销继承来的 fd（≥3）。
 * HAL 侧已给每个器件句柄加 O_CLOEXEC，但宿主不知道来源的句柄同样会被
 * /bin/sh 继承：第三方 Lua 驱动自己 open 的 fd、dlopen 的 libcurl socket。
 * 子 shell 一旦拿到绑定 /dev/i2c-1 的 fd，模型驱动的命令就能直接 ioctl 拨动
 * 硬件——而且是在 robot->deactivate() 之后依然有效，「关掉机器人就安全」这条
 * 预期会落空。故在子进程侧再兜一层，不依赖调用方是否守规矩。
 * close_range(2)（Linux ≥ 5.9）一次系统调用给 [3, UINT_MAX] 全部打上
 * CLOSE_RANGE_CLOEXEC：只打标志不真关，0/1/2 已 dup2 到管道，其余留到 exec
 * 由内核关闭，语义与 O_CLOEXEC 一致。不可用时退化到逐个 close。
 * 两条路径都在 fork 之后、exec 之前，且**不分配堆内存**：本进程有守护线程会
 * 并发 fork，子进程里 malloc 可能撞上父进程持有且永不会释放的锁。 */
static void revokeInheritedFds() {
#ifdef SYS_close_range
	if (::syscall(SYS_close_range, 3U, ~0U, 1U /* CLOSE_RANGE_CLOEXEC */) == 0) { return; }
#endif
	/* 退化路径（内核 < 5.9 或 close_range 不可用）：按软上限逐个关。
	 * 封顶 4096 —— 无上限的 rlim_cur（容器里常见 1048576）会让这段循环
	 * 跑上百万次系统调用；超出的部分不覆盖，属退化路径的已知缺口。 */
	long max = 4096;
	struct rlimit rl{};
	if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
		&& static_cast<long>(rl.rlim_cur) < max) {
		max = static_cast<long>(rl.rlim_cur);
	}
	for (long fd = 3; fd < max; ++fd) { ::close(static_cast<int>(fd)); }
}

RunResult runShell(const std::string& cmd, int timeoutSec) {
	int po[2] = {-1, -1};
	int pe[2] = {-1, -1};
	/* O_CLOEXEC 用 pipe2 原子设置（无 pipe→fork 之间的窗口）：本进程有守护线程
	 * 会并发 fork/exec，未标 CLOEXEC 的读端会被无关子进程继承，写端迟迟不 EOF
	 * ——只能靠硬上限兜底（且被继承的 fd 本身即泄漏）。 */
	if (pipe2(po, O_CLOEXEC) != 0) { throw std::runtime_error("pipe 失败"); }
	if (pipe2(pe, O_CLOEXEC) != 0) {
		close(po[0]);
		close(po[1]);
		throw std::runtime_error("pipe 失败");
	}
	std::vector<char*> envp = buildCleanEnvp();	/* fork 前构建，子进程直接继承 */
	pid_t pid = fork();
	if (pid < 0) {
		close(po[0]);
		close(po[1]);
		close(pe[0]);
		close(pe[1]);
		throw std::runtime_error("fork 失败");
	}
	if (pid == 0) {
		setpgid(0, 0);	/* 独立进程组：超时/中断可整组终止（含孙进程） */
		/* fix-plan 3-1：stdin 重定向到 /dev/null——子命令不再与主进程（agent REPL）
		 * 抢终端输入；打开失败按 fail-closed 直接放弃执行（127）。 */
		int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (devnull < 0) { _exit(127); }
		dup2(devnull, STDIN_FILENO);
		if (devnull != STDIN_FILENO) { close(devnull); }
		dup2(po[1], STDOUT_FILENO);
		dup2(pe[1], STDERR_FILENO);
		close(po[0]);
		close(po[1]);
		close(pe[0]);
		close(pe[1]);
		revokeInheritedFds();	/* 必须在 dup2 之后：0/1/2 已指向管道与 /dev/null */
		execle("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr), envp.data());
		_exit(127);
	}
	setpgid(pid, pid);	/* 与子进程竞争设置，谁先成功效果一致 */
	close(po[1]);
	close(pe[1]);
	/* fix-plan 3-1：fcntl 失败即 fatal（对齐 SO_RCVTIMEO 在 actionsource 的
	 * fail-closed 口径）——O_NONBLOCK 设置失败会让下方 read 循环在无数据时
	 * 阻塞挂死整个 agent 会话，绝不能静默跳过。 */
	if (fcntl(po[0], F_SETFL, O_NONBLOCK) != 0 || fcntl(pe[0], F_SETFL, O_NONBLOCK) != 0) {
		const int savedErrno = errno;
		close(po[0]);
		close(pe[0]);
		/* 子进程已 fork 且在跑：抛错前必须整组终止并回收，否则留下孤儿/僵尸，
		 * 且它仍持着管道写端（阶段 4.2 补）。宽限取短：本条路径本身已是
		 * 「参数/资源异常」，不该再让调用方多等。 */
		::kill(-pid, SIGKILL);
		int reaped = 0;
		(void)reapWithGrace(pid, reaped, 200);
		errno = savedErrno;
		throw std::runtime_error(std::string("fcntl 失败: ") + std::strerror(errno));
	}

	RunResult r;
	bool exited = false, outEof = false, errEof = false, killed = false;
	int status = 0;
	std::array<char, 4096> buf;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
	auto hardDeadline = deadline + std::chrono::seconds(3);

	while (!(exited && outEof && errEof)) {
		struct pollfd fds[2] = {
			{outEof ? -1 : po[0], POLLIN, 0},
			{errEof ? -1 : pe[0], POLLIN, 0},
		};
		if (poll(fds, 2, 100) > 0) {
			for (const auto& fd : fds) {
				/* POLLERR/POLLNVAL 必须纳入：这两个位只报错误、不置 POLLIN/POLLHUP，
				 * 原实现只认后两者 → 出错 fd 既不读也不关，每轮重新就绪，一直空转
				 * 到硬上限才退出（白耗 3s 且不报告真实成因）。 */
				if (fd.fd < 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
					continue;
				}
				std::string& sink = (fd.fd == po[0]) ? r.out : r.err;
				bool& truncated = (fd.fd == po[0]) ? r.outTruncated : r.errTruncated;
				ssize_t n = 0;
				/* 阶段 4.2：read 的 EINTR 必须重试而非当终态——信号处理器未装
				 * SA_RESTART，Ctrl-C/SIGTERM 期间 EINTR 会真实发生；旧实现把它
				 * 判成 EOF：丢掉子进程剩余输出，并提前关闭读端给仍在跑的子进程
				 * 送 SIGPIPE。 */
				while ((n = bot::retryEintr(
						[&] { return ::read(fd.fd, buf.data(), buf.size()); })) > 0) {
					appendCapped(sink, buf.data(), static_cast<std::size_t>(n), truncated);
				}
				/* 终态＝错误位 / 正常 EOF / 非 EAGAIN 的读错误：关闭并标记该流结束 */
				const bool terminal = (fd.revents & (POLLERR | POLLNVAL))
					|| n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
				if (terminal) {
					close(fd.fd);
					if (fd.fd == po[0]) { outEof = true; } else { errEof = true; }
				}
			}
		}
		if (!exited) {
			pid_t w = bot::retryEintr([&] { return ::waitpid(pid, &status, WNOHANG); });
			if (w == pid) { exited = true; }
		}
		auto now = std::chrono::steady_clock::now();
		bool stop = stopRequested();
		if (!killed && (now >= deadline || stop)) {
			/* 成因只在首次越限时记录：此后 now 会越过 deadline，若每轮重算会把
			 * 「先收到停止信号」误翻成 timedOut。 */
			r.timedOut = now >= deadline;
			r.interrupted = stop && now < deadline;
			/* 整组 SIGKILL。失败（EPERM 等）不置 killed、下轮重试——原实现只尝试
			 * 一次且不查返回值，一旦 kill 未生效就靠下方 waitpid 无限等待挂死。
			 * ESRCH 表示进程组已不存在（已自行退出），等价于终止成功。 */
			if (kill(-pid, SIGKILL) == 0 || errno == ESRCH) { killed = true; }
		}
		/* 命令自身忽略 SIGKILL 也不可能，但管道可能迟迟不 EOF：硬上限兜底，
		 * 不让 agent 会话卡在一个 pid 上 */
		if (now >= hardDeadline) { break; }
	}

	if (!outEof) { close(po[0]); }
	if (!errEof) { close(pe[0]); }
	/* 收尾回收绝不无超时阻塞：kill 未生效（D 态 / 权限）时无超时的 waitpid 会把
	 * agent 会话挂死。轮询到固定宽限仍未回收即放弃（交由 init 收尸），
	 * exitCode 保持 -1。EINTR 重试见 reapWithGrace。 */
	if (!exited) { exited = reapWithGrace(pid, status); }
	r.exitCode = (exited && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
	return r;
}

std::string formatRunResult(const RunResult& r) {
	std::string s = "exit_code=" + std::to_string(r.exitCode);
	if (r.timedOut) { s += "（超时，已终止）"; }
	if (r.interrupted) { s += "（被中断，已终止）"; }
	s += "\nstdout:\n" + (r.out.empty() ? std::string("(空)") : r.out);
	s += "\nstderr:\n" + (r.err.empty() ? std::string("(空)") : r.err);
	/* 上限取自 kMaxCommandOutput，避免文案与实现漂移（原先硬编码 "64KB"） */
	const std::string cap = std::to_string(kMaxCommandOutput / 1024) + "KB";
	if (r.outTruncated) { s += "\n[stdout 超过 " + cap + "，已截断]"; }
	if (r.errTruncated) { s += "\n[stderr 超过 " + cap + "，已截断]"; }
	return s;
}

} // namespace agent
} // namespace bot
