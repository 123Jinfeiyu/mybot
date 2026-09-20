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

/* 环境清洗：剔除名称含敏感关键词的变量。模型可能被外部输入（画面/文件内容）
 * 诱导执行任意命令，不能让子进程继承 LLM API key。
 * 注意 BOT4CJ_LLM_KEY 含 "key"，天然被本规则覆盖。 */
std::vector<char*> buildCleanEnvp() {
	static const char* sensitive[] = {"key", "token", "secret", "password",
					  "passwd", "credential"};
	std::vector<char*> envp;
	for (char** e = ::environ; e && *e; ++e) {
		std::string_view entry(*e);
		std::size_t eq = entry.find('=');
		if (eq == std::string_view::npos) { continue; }
		std::string name(entry.substr(0, eq));
		std::transform(name.begin(), name.end(), name.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		bool drop = false;
		for (const char* s : sensitive) {
			if (name.find(s) != std::string::npos) { drop = true; break; }
		}
		if (!drop) { envp.push_back(*e); }
	}
	envp.push_back(nullptr);
	return envp;
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
				while ((n = read(fd.fd, buf.data(), buf.size())) > 0) {
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
			pid_t w = waitpid(pid, &status, WNOHANG);
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
	/* 收尾回收绝不无超时阻塞：kill 未生效（D 态 / 权限）时原实现会在无超时的
	 * waitpid 上无限等待，把 agent 会话挂死。改为 WNOHANG 轮询到固定宽限，
	 * 仍未回收则放弃（进程交由 init 收尸），exitCode 保持 -1。 */
	if (!exited) {
		const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		for (;;) {
			pid_t w = waitpid(pid, &status, WNOHANG);
			if (w == pid) { exited = true; break; }
			if (w < 0) { break; }	/* ECHILD 等：已无可回收 */
			if (std::chrono::steady_clock::now() >= reapDeadline) { break; }
			poll(nullptr, 0, 10);	/* 10ms 退避，避免忙等 */
		}
	}
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
