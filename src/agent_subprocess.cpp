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
#include <cstdlib>
#include <cstring>
#include <dirent.h>
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

/* 敏感名判据：按分隔符切词后逐词查表（BOT4CJ_LLM_KEY → {bot4cj, llm, key} →
 * 命中 key）。buildCleanEnvp 与 collectEnvSecrets 共用同一份，否则「子进程清洗
 * 名单」与「宿主抹除名单」会各自漂移；agentaudit 的命令遮蔽亦复用它。 */
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

/* 放弃回收时的告警文案（单点 + 纯函数，便于离线断言）。
 * 为什么值得单独成函数：这条路径**极难在离线环境构造**——要让子进程在 2s 宽限
 * 内收不回来，得让它处于 D 态（需要内核态挂起）或让 SIGKILL 无效（PID 1 /
 * 受限进程；容器里本进程常是 PID 1，但测试二进制不是，`trap "" KILL` 无效）。
 * 实测过该构造在容器内不成立，故不写易碎的行为用例，改为把「说了什么」钉住：
 * 文案必须同时给出 pid、宽限时长与「会留僵尸」的后果——静默的问题是操作者
 * 不知道发生过什么，这三点正是他需要的。 */
std::string reapGiveUpWarning(int pid, int graceMs) {
	return "警告: 子进程 " + std::to_string(pid) + " 在 " + std::to_string(graceMs)
		+ "ms 宽限内未回收（可能处于 D 态或 kill 未生效）；"
		  "本次放弃回收，该 pid 在宿主退出前会保持僵尸";
}

/* 有界回收：绝不无超时阻塞——kill 未生效（D 态 / 权限）时无超时 waitpid 会把
 * agent 会话永久挂死。WNOHANG 轮询到固定宽限，仍未回收即放弃（交由 init 收尸）。
 * EINTR 经 retryEintr 重试（阶段 4.2）：旧实现把 waitpid 的 -1/EINTR 当
 * 「已无可回收」直接 break，于是留下僵尸且 exitCode 停在 -1。
 *
 * 放弃回收时**响亮告警**（2026-09-21 审计 AG-10）：注释原来写「交由 init 收尸」，
 * 但常驻 agent 进程自己还活着，init 不会接手，该 pid 会一直是僵尸；反复触发
 * （D 态挂死的命令）就会在 agent 生命周期内累积。项目口径是「宁告警不静默」，
 * 且这条路径的另一半（RunResult::hardStopped）已进模型可见的回执，这里再补一行
 * stderr 让操作者也看得见 pid 与成因。 */
bool reapWithGrace(pid_t pid, int& status, int graceMs) {
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(graceMs);
	for (;;) {
		pid_t w = bot::retryEintr([&] { return ::waitpid(pid, &status, WNOHANG); });
		if (w == pid) { return true; }
		if (w < 0) { return false; }	/* ECHILD 等：确已无可回收 */
		if (std::chrono::steady_clock::now() >= deadline) {
			std::cerr << reapGiveUpWarning(static_cast<int>(pid), graceMs) << std::endl;
			return false;
		}
		::poll(nullptr, 0, 10);		/* 10ms 退避，避免忙等 */
	}
}

/* fork 前在**父进程**把 /proc/self/fd 里全部 ≥3 的句柄打上 FD_CLOEXEC。
 * （审计 P1 修复，2026-09-21）这是 revokeInheritedFds 之外的第二层：父进程
 * 允许分配堆内存（opendir 可用），按「实际打开的 fd」枚举，天然覆盖高编号
 * 句柄，不受退化路径数值封顶的限制；子进程侧 revokeInheritedFds 继续兜住
 * 「枚举之后、exec 之前」由并发守护线程新开的句柄。设置 CLOEXEC 不影响任何
 * fd 在本进程内的继续使用（主进程自身从不 exec），HAL 句柄本就 O_CLOEXEC。
 * 无 /proc 的环境（罕见）静默跳过，交由子进程侧退化路径兜底。 */
static void markAllFdsCloexec() {
	DIR* d = ::opendir("/proc/self/fd");
	if (d == nullptr) { return; }
	const int self = ::dirfd(d);
	while (struct dirent* e = ::readdir(d) ) {
		const int fd = ::atoi(e->d_name);
		if (fd < 3 || fd == self) { continue; }
		const int flags = ::fcntl(fd, F_GETFD);
		if (flags >= 0) { ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
	}
	::closedir(d);
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
 * 并发 fork，子进程里 malloc 可能撞上父进程持有且永不会释放的锁。
 * 该 flags **必须是 CLOSE_RANGE_CLOEXEC = 1U<<2 = 4**（2026-09-21 审计 AG-3）：
 * 它是位掩码，传未定义的位（旧实现写的 1U）内核直接 EINVAL，于是每次
 * run_command 都白跑一遍退化循环——soft limit 高的机器上约 40ms（实测
 * soft=524288），而注释却写着「一次系统调用」。安全结论不受影响（退化路径是
 * 真 close，比只打标志更强，且父进程侧 markAllFdsCloexec 独立承重），
 * 但「注释声称的路径从未执行」正是最难发现的一类缺陷。 */
/* 子进程侧路径自报（2026-09-21 审计 AG-11）。
 *
 * 为什么需要它：`revokeInheritedFds` / `execShellChild` 在 gcov 报告里**恒为 0%**，
 * 而且这不是「没测」而是「测了测不到」——gcov 计数在进程退出时落盘，而这两个函数
 * 永远以 execve 结束：exec 用新映像的计数页覆盖当前进程，子侧的执行记录不可能被
 * 写出。AG-3（CLOSE_RANGE_CLOEXEC 标志写成 1U、快速路径从未执行）在这个盲区里
 * 藏了很久，因为该区块对所有读者都显示 0%，所有人都以为「那是有意不测的部分」。
 *
 * 解法是不再试图测量它，而是让它**自己报告**选了哪条路径：在 fork 之后、exec 之前
 * 把一行文本写进父进程打开并传入的 fd。约束与 execShellChild 一致——不分配堆内存、
 * 只用栈上缓冲与 write(2)；fd 无效时静默跳过（该探针绝不能影响真实执行路径）。
 * 读取端只在测试里设置 BOT4CJ_TRACE_FDS（见 test_agenttools.cpp 的 AG-11 用例），
 * 不设时 path 为空、全程零开销。 */
/* fd 由调用方经参数传入而**不是**用静态量：宿主有守护线程会并发 fork/exec，
 * 静态量会被并发 run_command 互相踩（一个线程改、另一个线程的子进程读）。 */
static void traceChildPath(int traceFd, const char* path) {
	if (traceFd < 0 || path == nullptr) { return; }
	char buf[64];
	int n = 0;
	for (const char* p = path; *p != '\0' && n < static_cast<int>(sizeof buf) - 2; ++p) {
		buf[n++] = *p;
	}
	buf[n++] = '\n';
	ssize_t ignored = ::write(traceFd, buf, static_cast<std::size_t>(n));
	(void)ignored;
}

static void revokeInheritedFds(int traceFd) {
#ifdef SYS_close_range
	/* 取值来自内核 UAPI（linux/close_range.h：1U << 2）。不引头文件而写字面量，
	 * 是为了兼容没装新头文件的老发行版；该常量自 5.9 起未变，且下面有「失败即
	 * 退化」兜底——取值错也不会破功能，只会在性能上退化，正是修复前的状态。 */
	constexpr unsigned kCloseRangeCloexec = 1U << 2;
	/* 自报放在**调用之前**：无论成败都记录「尝试了快速路径」，退化路径再记一条。
	 * 两条都记才能分辨「快速路径没走」与「快速路径走了但失败」——AG-3 正是前者。 */
	traceChildPath(traceFd, "close_range-attempt");
	if (::syscall(SYS_close_range, 3U, ~0U, kCloseRangeCloexec) == 0) {
		traceChildPath(traceFd, "close_range-ok");
		return;
	}
	traceChildPath(traceFd, "close_range-failed");
#else
	traceChildPath(traceFd, "close_range-unavailable");
#endif
	/* 退化路径（内核 < 5.9 或 close_range 不可用）：按真实上限逐个关。
	 * （审计 P1 修复，2026-09-21）旧实现固定封顶 4096——soft limit 更高的
	 * 进程（容器常见 1048576）里编号 ≥4096 的继承 fd 不会被覆盖，模型驱动
	 * 的 shell 可能拿到硬件/内部 socket 句柄。现在取 RLIMIT_NOFILE 软上限；
	 * 仍保留绝对封顶 1048576：无上限 rlim（或极端配置）时这段循环最多百万
	 * 次 close 系统调用（fork/exec 一次性路径，代价可接受），超过封顶的
	 * 极端配置属残余已知缺口（且父进程侧 markAllFdsCloexec 已按实际打开
	 * 的 fd 覆盖，本循环只兜「枚举之后新开」的窗口）。 */
	long max = 1048576;
	struct rlimit rl{};
	if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
		&& static_cast<long>(rl.rlim_cur) < max) {
		max = static_cast<long>(rl.rlim_cur);
	}
	traceChildPath(traceFd, "fallback-close-loop");
	for (long fd = 3; fd < max; ++fd) { ::close(static_cast<int>(fd)); }
}

/* 子进程侧：独立进程组 + 按 spec 摆好 0/1/2 + 按 spec 吊销继承 fd。
 * **本函数不返回**：成功即 execle 成 /bin/sh，任何失败 _exit(127)。
 * 约束与既有的两道防线一致——fork 之后 **不分配堆内存**（宿主有守护线程会并发
 * fork，子进程里 malloc 可能撞上父进程持有且永不会释放的锁），只用已就绪的
 * 参数与栈上缓冲。
 * fd 收尾顺序有讲究：先把 0/1/2 指向目标，再关掉继承来的管道端，最后才
 * revokeInheritedFds——它会把 ≥3 的全部句柄打上/直接关闭，早跑会把 0/1/2
 * 之外刚 dup 好的东西也一起带走。 */
[[noreturn]] void execChild(const ChildSpec& spec, int stdinR, int outR, int outW, int errR,
		int errW, char* const* envp) {
	setpgid(0, 0);	/* 独立进程组：超时/中断可整组终止（含孙进程） */
	if (spec.pipeStdin) {
		if (::dup2(stdinR, STDIN_FILENO) < 0) { _exit(127); }
		/* 原读端已 dup 到 fd 0；显式关掉，不等 exec 时的 O_CLOEXEC——关掉之后
		 * 父进程侧写端能在子进程退出时更早看到 EPIPE。 */
		if (stdinR != STDIN_FILENO) { ::close(stdinR); }
	} else {
		/* fix-plan 3-1：stdin 重定向到 /dev/null——子命令不再与主进程（agent REPL）
		 * 抢终端输入；打开失败按 fail-closed 直接放弃执行（127）。 */
		int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (devnull < 0) { _exit(127); }
		::dup2(devnull, STDIN_FILENO);
		if (devnull != STDIN_FILENO) { ::close(devnull); }
	}
	if (spec.captureOutput) {
		::dup2(outW, STDOUT_FILENO);
		::dup2(errW, STDERR_FILENO);
		::close(outR);
		::close(outW);
		::close(errR);
		::close(errW);
	}
	if (spec.revokeFds) { revokeInheritedFds(spec.traceFd); }	/* 必须在 dup2 之后 */
	::execle("/bin/sh", "sh", "-c", spec.cmd.c_str(), static_cast<char*>(nullptr), envp);
	_exit(127);
}

/* 排空一个读端（fd 已置 O_NONBLOCK）：可读即循环读到 EAGAIN 为止，超限截断。
 * 返回该流是否**终态**（正常 EOF / 非 EAGAIN 的读错误）；判 POLLERR/POLLNVAL
 * 由调用方补。
 * EINTR 必须重试而非当终态（阶段 4.2）：信号处理器未装 SA_RESTART，
 * Ctrl-C/SIGTERM 期间 EINTR 会真实发生；旧实现把它判成 EOF——丢掉子进程剩余
 * 输出，并提前关闭读端给仍在跑的子进程送 SIGPIPE。 */
bool drainStream(int fd, std::string& sink, bool& truncated) {
	std::array<char, 4096> buf;
	ssize_t n = 0;
	while ((n = bot::retryEintr([&] { return ::read(fd, buf.data(), buf.size()); })) > 0) {
		appendCapped(sink, buf.data(), static_cast<std::size_t>(n), truncated);
	}
	return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

/* 处理本轮 poll 就绪的读端：排空 + 终态判定与关闭。
 * POLLERR/POLLNVAL 必须纳入就绪判据：这两个位只报错误、不置 POLLIN/POLLHUP，
 * 原实现只认后两者 → 出错 fd 既不读也不关，每轮重新就绪，一直空转到硬上限
 * 才退出（白耗 3s 且不报告真实成因）。 */
void pumpReadyStreams(const struct pollfd (&fds)[2], int outFd, ChildOutcome& r,
		bool& outEof, bool& errEof) {
	for (const auto& fd : fds) {
		if (fd.fd < 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) { continue; }
		/* fds[] 只由两个读端构成，且 EOF 过的一端已被置 -1——非负即本端 */
		const bool isOut = (fd.fd == outFd);
		std::string& sink = isOut ? r.out : r.err;
		bool& truncated = isOut ? r.outTruncated : r.errTruncated;
		const bool drained = drainStream(fd.fd, sink, truncated);
		/* 终态＝错误位 / 正常 EOF / 非 EAGAIN 的读错误：关闭并标记该流结束 */
		if ((fd.revents & (POLLERR | POLLNVAL)) || drained) {
			close(fd.fd);
			if (isOut) { outEof = true; } else { errEof = true; }
		}
	}
}

/* 超时之后再给 3s 硬上限（仅在捕获模式下可达：管道迟迟不 EOF 时兜底）。 */
constexpr int kHardStopGraceSec = 3;

/* 整组 SIGKILL + 裸 pid 补枪。返回值＝「本次终止是否已生效」：送达 0 即成功；
 * ESRCH 表示进程组已不存在（子进程已自行退出），等价于成功；EPERM 等失败返回
 * false，由调用方下一轮重试（旧实现只尝试一次且不查返回值，一旦 kill 未生效
 * 就靠无超时 waitpid 无限等待挂死）。裸 pid 补枪覆盖「子进程还没 setpgid，
 * kill(-pid) 报 ESRCH 但进程仍在」的窗口——确认程序路径原本就有，现为单点。 */
bool killGroup(pid_t pid) {
	const int rc = ::kill(-pid, SIGKILL);
	const int savedErrno = errno;
	(void)::kill(pid, SIGKILL);
	return rc == 0 || savedErrno == ESRCH;
}

/* 把 payload 写给子进程 stdin：非阻塞 + poll 循环，防「子进程不读 stdin + 参数
 * 巨大」把宿主管死。SIGPIPE 以 pthread_sigmask 局部屏蔽（不动进程级 disposition
 * ——宿主还有别的管道写者，改全局处置是越权）。
 * EPIPE 等错误直接放弃写入并关掉写端：子进程不等输入就先退了，收尾分支照常
 * 拿退出码（这条路径原属确认程序，2026-09-22 审计 C7 补用例钉住）。
 *
 * 屏蔽**不等于**没有信号：写撞 EPIPE 时内核同时产生一条 SIGPIPE，屏蔽期间它
 * 只是待决，解除屏蔽的那一刻就按默认处置**杀掉整个 agent 进程**。这是补 C7
 * 用例时当场抓到的：确认程序不读 stdin + payload 超过管道容量 → 测试进程
 * 收 SIGPIPE 退出（rc=141），而这条路径此前没有任何用例。故恢复掩码前先把
 * **自己制造的**那条待决信号吃掉（sigtimedwait 零超时）；判据用「屏蔽前是否
 * 已有待决 SIGPIPE」——别人的信号不动。 */
void writeStdinPayload(int fd, const std::string& payload,
		std::chrono::steady_clock::time_point deadline) {
	sigset_t oldMask{}, blockMask{}, preexisting{};
	sigemptyset(&blockMask);
	sigaddset(&blockMask, SIGPIPE);
	::sigpending(&preexisting);
	const bool hadPending = (::sigismember(&preexisting, SIGPIPE) == 1);
	pthread_sigmask(SIG_BLOCK, &blockMask, &oldMask);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	std::size_t written = 0;
	while (written < payload.size()) {
		ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
		if (n > 0) { written += static_cast<std::size_t>(n); continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (std::chrono::steady_clock::now() >= deadline) { break; }
			struct pollfd pfd = {fd, POLLOUT, 0};
			(void)::poll(&pfd, 1, 50);
			continue;
		}
		if (n < 0 && errno == EINTR) { continue; }
		break;
	}
	::close(fd);
	/* 吃掉自己制造的那条待决 SIGPIPE（见函数注释）：只在「屏蔽前本无待决」
	 * 时才动，免得顺手吞掉宿主别的写者正在等的那条。 */
	if (!hadPending) {
		sigset_t pending{};
		if (::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1) {
			const struct timespec zero = {0, 0};
			(void)::sigtimedwait(&blockMask, nullptr, &zero);
		}
	}
	pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
}

/* 收尾：关掉未 EOF 的读端、有界回收子进程、折算退出状态、UTF-8 消毒。
 * 回收绝不无超时阻塞：kill 未生效（D 态 / 权限）时无超时的 waitpid 会把 agent
 * 会话挂死——确认程序路径曾留下这一条（2026-09-22 审计 F1）。轮询到宽限仍未
 * 回收即放弃（交由 init 收尸）并按 reapGiveUpWarning 响亮告警，exitCode 保持 -1。
 * EINTR 重试见 reapWithGrace。 */
void finalizeChild(pid_t pid, const ChildSpec& spec, int outFd, int errFd, bool outEof,
		bool errEof, bool exited, int status, ChildOutcome& r) {
	if (!outEof) { close(outFd); }
	if (!errEof) { close(errFd); }
	if (!exited) { exited = reapWithGrace(pid, status, spec.graceMs); }
	r.reaped = exited;
	if (exited && WIFEXITED(status)) {
		r.normalExit = true;
		r.exitCode = WEXITSTATUS(status);
	} else if (exited && WIFSIGNALED(status)) {
		r.termSig = WTERMSIG(status);
	}

	/* UTF-8 消毒（2026-09-21 审计 AG-1）**必须在收尾做，不能边读边做**：
	 * 读循环是「可读即读」的增量排水，一次 read 可能把多字节序列从中间切断，
	 * 逐块消毒会把一个合法字符拆成两个 U+FFFD。此处两个流都已终态，序列完整；
	 * 截断发生在 kMaxCommandOutput 边界上时留下的半截序列也会被替换掉——
	 * 那正是我们要的行为（宁替换一个字符，不让网关 400 掉整个会话）。
	 * 消毒点放在这里而不是 formatRunResult：后者只管排版，且结果里的 out/err
	 * 可能被别的调用方直接读走。非捕获模式下两串为空，是一次空转。 */
	r.out = sanitizeUtf8(r.out);
	r.err = sanitizeUtf8(r.err);
}

/* 骨架单点：pipe → fork → setpgid →（写 stdin）→ 轮询 → 超时整组终止 →
 * 有界回收。两个调用点（run_command / 策略确认程序）的差异全部经 ChildSpec
 * 显式声明，这里不再有「只有一侧有」的防线。 */
ChildOutcome runChild(const ChildSpec& spec) {
	const bool capture = spec.captureOutput;
	int po[2] = {-1, -1};	/* stdout 管道 */
	int pe[2] = {-1, -1};	/* stderr 管道 */
	int pi[2] = {-1, -1};	/* stdin 管道（仅 pipeStdin） */
	/* 关一个 fd 并置 -1：所有失败路径都走它，重复调用安全（不会误关刚被复用的编号）。 */
	auto closeFd = [](int& fd) {
		if (fd >= 0) { ::close(fd); fd = -1; }
	};
	auto closeAll = [&] {
		closeFd(po[0]); closeFd(po[1]);
		closeFd(pe[0]); closeFd(pe[1]);
		closeFd(pi[0]); closeFd(pi[1]);
	};
	auto pipeFailed = [] {
		const int saved = errno;
		return std::runtime_error("pipe 失败（errno=" + std::to_string(saved) + "）");
	};
	/* O_CLOEXEC 用 pipe2 原子设置（无 pipe→fork 之间的窗口）：本进程有守护线程
	 * 会并发 fork/exec，未标 CLOEXEC 的读端会被无关子进程继承，写端迟迟不 EOF
	 * ——只能靠硬上限兜底（且被继承的 fd 本身即泄漏）。 */
	if (capture) {
		if (pipe2(po, O_CLOEXEC) != 0) { throw pipeFailed(); }
		if (pipe2(pe, O_CLOEXEC) != 0) { closeAll(); throw pipeFailed(); }
	}
	if (spec.pipeStdin && pipe2(pi, O_CLOEXEC) != 0) { closeAll(); throw pipeFailed(); }
	/* 环境在 fork 前备好（buildCleanEnvp 会分配堆内存，子进程里不能碰）：
	 * cleanEnv 是模型可驱动命令的口径，inherit 原样传 environ（确认程序口径）。 */
	std::vector<char*> envp;
	char* const* envpPtr = ::environ;
	if (spec.cleanEnv) {
		envp = buildCleanEnvp();
		envpPtr = envp.data();
	}
	if (spec.revokeFds) { markAllFdsCloexec(); }	/* fork 前父进程侧全量吊销（见函数注释） */
	pid_t pid = fork();
	if (pid < 0) {
		const int saved = errno;
		closeAll();
		throw std::runtime_error(std::string("fork 失败: ") + std::strerror(saved));
	}
	if (pid == 0) { execChild(spec, pi[0], po[0], po[1], pe[0], pe[1], envpPtr); }
	setpgid(pid, pid);	/* 与子进程竞争设置，谁先成功效果一致 */
	closeFd(po[1]);
	closeFd(pe[1]);
	closeFd(pi[0]);
	if (capture) {
		/* fix-plan 3-1：fcntl 失败即 fatal（对齐 SO_RCVTIMEO 在 actionsource 的
		 * fail-closed 口径）——O_NONBLOCK 设置失败会让下方 read 循环在无数据时
		 * 阻塞挂死整个 agent 会话，绝不能静默跳过。 */
		if (fcntl(po[0], F_SETFL, O_NONBLOCK) != 0 || fcntl(pe[0], F_SETFL, O_NONBLOCK) != 0) {
			const int savedErrno = errno;
			closeAll();
			/* 子进程已 fork 且在跑：抛错前必须整组终止并回收，否则留下孤儿/僵尸，
			 * 且它仍持着管道写端（阶段 4.2 补）。宽限取短：本条路径本身已是
			 * 「参数/资源异常」，不该再让调用方多等。 */
			(void)killGroup(pid);
			int reaped = 0;
			(void)reapWithGrace(pid, reaped, 200);
			throw std::runtime_error(std::string("fcntl 失败: ") + std::strerror(savedErrno));
		}
	}

	ChildOutcome r;
	int status = 0;
	bool exited = false, killed = false;
	bool outEof = !capture, errEof = !capture;	/* 非捕获模式没有流要等 */
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(spec.timeoutSec);
	if (spec.pipeStdin) { writeStdinPayload(pi[1], spec.stdinPayload, deadline); }
	auto hardDeadline = deadline + std::chrono::seconds(kHardStopGraceSec);

	while (!(exited && outEof && errEof)) {
		if (capture) {
			struct pollfd fds[2] = {
				{outEof ? -1 : po[0], POLLIN, 0},
				{errEof ? -1 : pe[0], POLLIN, 0},
			};
			if (poll(fds, 2, 100) > 0) {
				pumpReadyStreams(fds, po[0], r, outEof, errEof);
			}
		} else {
			/* 非捕获模式（确认程序）没有管道要排空：短睡等退出（沿用原 20ms 节拍） */
			::poll(nullptr, 0, 20);
		}
		if (!exited) {
			pid_t w = bot::retryEintr([&] { return ::waitpid(pid, &status, WNOHANG); });
			if (w == pid) { exited = true; }
		}
		const auto now = std::chrono::steady_clock::now();
		const bool stop = spec.watchStop && stopRequested();
		if (!killed && (now >= deadline || stop)) {
			/* 成因只在首次越限时记录：此后 now 会越过 deadline，若每轮重算会把
			 * 「先收到停止信号」误翻成 timedOut。 */
			r.timedOut = now >= deadline;
			r.interrupted = stop && now < deadline;
			if (killGroup(pid)) { killed = true; }	/* 失败下轮重试 */
		}
		/* 非捕获模式没有管道可泵：超时即进收尾，交给 reapWithGrace 做有界回收
		 * ——F1 的修复点（旧实现在这条分支打的是无超时 waitpid）。 */
		if (!capture && now >= deadline) { break; }
		/* 命令自身忽略 SIGKILL 也不可能，但管道可能迟迟不 EOF（子进程卡在 D 态、
		 * 或 kill 因权限失败）：硬上限兜底，不让 agent 会话卡在一个 pid 上。
		 * 必须留下标记——旧实现直接 break，回执看起来像「正常结束但退出码 -1」，
		 * 模型与操作者都看不出这里发生过放弃回收。 */
		if (now >= hardDeadline) {
			r.hardStopped = true;
			break;
		}
	}

	finalizeChild(pid, spec, po[0], pe[0], outEof, errEof, exited, status, r);
	return r;
}

RunResult runShell(const std::string& cmd, int timeoutSec) {
	/* 子进程路径自报的目标 fd（AG-11）：只在测试里经 BOT4CJ_TRACE_FDS 指定，
	 * 生产路径下这个变量不存在，traceFd 保持 -1、探针全程零开销。
	 * 注意顺序：先开 trace fd 再进 runChild——后者 fork 前会调 markAllFdsCloexec
	 * 给「已存在的 ≥3 句柄」打 FD_CLOEXEC，而 trace fd 必须活到 exec 之前才能被
	 * 子进程写（子进程侧 revokeInheritedFds 会关掉它，所以报告发生在关闭之前）。
	 * O_CLOEXEC 只影响 exec 之后（探针本就不需要活过 exec）；O_APPEND 让多次
	 * run_command 的记录按追加写入，单条报告是一次 write(2)，不会与其他写者交错。
	 * fd 归本函数持有（runChild 不关它），异常路径也由 guard 兜住。 */
	int traceFd = -1;
#ifdef MYBOT_TEST_TRACE_FDS
	/* A6：探针只在**测试构建**里存在（CMakeLists 给 mybot_tests 单独定义
	 * MYBOT_TEST_TRACE_FDS）。此前它无条件读环境变量并 open，于是生产二进制里
	 * 也有「按 BOT4CJ_TRACE_FDS 追加写任意路径（0644）」这条路径——能设置 mybot
	 * 启动环境的人即可让宿主以 mybot 权限写文件。威胁等级不高（这类攻击者本已
	 * 等同 mybot 用户，见 docs/agent.md 安全说明第 2/3 条），但「测试钩子在生产
	 * 里活着」没有理由，且与上面注释自陈的口径直接矛盾。
	 * 只 ifdef 这一处而不 ifdef traceChildPath/revokeInheritedFds：那两个函数在
	 * 关掉开关后会变成未使用函数，撞 -Wall -Werror；而 traceFd 恒为 -1 时
	 * traceChildPath 首行即返回，生产路径上零行为、零系统调用。 */
	if (auto tracePath = getEnvVar("BOT4CJ_TRACE_FDS"); tracePath && !tracePath->empty()) {
		traceFd = ::open(tracePath->c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	}
#endif
	struct TraceFdGuard {
		int fd;
		~TraceFdGuard() {
			if (fd >= 0) { ::close(fd); }
		}
	} traceGuard{traceFd};

	/* 选项全部取 runShell 的现状值（run_command 侧行为不变）；与确认程序不同的
	 * 那几项在这里一眼可见——这正是抽骨架要买到的东西。 */
	ChildSpec spec;
	spec.cmd = cmd;
	spec.cleanEnv = true;		/* 模型可驱动的命令必须清洗环境 */
	spec.pipeStdin = false;		/* stdin → /dev/null（fix-plan 3-1） */
	spec.captureOutput = true;	/* 截断 + UTF-8 消毒 */
	spec.timeoutSec = timeoutSec;
	spec.graceMs = 2000;		/* 回收宽限（有界：绝不无超时阻塞） */
	spec.revokeFds = true;		/* 父侧 + 子侧 fd 吊销 */
	spec.watchStop = true;		/* REPL 中断 → 整组终止 */
	spec.traceFd = traceFd;

	const ChildOutcome co = runChild(spec);
	RunResult r;
	r.exitCode = co.exitCode;
	r.timedOut = co.timedOut;
	r.interrupted = co.interrupted;
	r.hardStopped = co.hardStopped;
	r.outTruncated = co.outTruncated;
	r.errTruncated = co.errTruncated;
	r.out = co.out;
	r.err = co.err;
	return r;
}

std::string formatRunResult(const RunResult& r) {
	std::string s = "exit_code=" + std::to_string(r.exitCode);
	if (r.timedOut) { s += "（超时，已终止）"; }
	if (r.interrupted) { s += "（被中断，已终止）"; }
	/* 硬上限兜底：不能声称「已终止」（kill 可能没生效），如实说「无法确认」——
	 * 模型据此就知道不该假定命令已经停了，操作者也会去看进程表。 */
	if (r.hardStopped) {
		s += "（超过硬上限仍未确认退出：已放弃回收，命令可能仍在运行，请检查进程表）";
	}
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
