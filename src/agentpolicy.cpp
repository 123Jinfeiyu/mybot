/* agentpolicy.cpp —— 策略钩子实现（见 agentpolicy.hpp 的设计约束） */

#include "agentpolicy.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <utility>

#include "agentaudit.hpp"
#include "jsonparse.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

constexpr std::size_t kPolicyFileMaxBytes = 64u * 1024;
/* 确认程序超时区间（秒）：下限 1 防 0/负值把确认变成「永远等不到」或「立刻
 * 判死」，上限 86400 与 --tcp-idle 同族（部署方可能接人工工单，确认程序
 * 里睡一天等审批也是合法用法，但更长得先想想是不是配错了单位）。 */
constexpr int kConfirmTimeoutMin = 1;
constexpr int kConfirmTimeoutMax = 86400;

std::string typeError(const std::string& path, const std::string& key, const char* want) {
	return "策略文件 " + path + " 的字段 '" + key + "' 应为" + want;
}

/* 外部确认程序的一次运行结果 */
struct ConfirmRun {
	bool ok = false;		/* 正常退出（拿到退出码） */
	bool timedOut = false;		/* 超时（已整组 SIGKILL 并回收） */
	std::string failReason;		/* ok=false 且非超时：pipe/fork/写失败等 */
	int exitCode = -1;
	int termSig = 0;		/* 被信号终止时记录 */
};

/* 把 payload 写给 cmd 的 stdin（/bin/sh -c），在 timeoutSec 内等它退出。
 * 超时按进程组 SIGKILL（与 run_command 的 runShell 同一口径），并回收。 */
ConfirmRun runConfirmProgram(const std::string& cmd, const std::string& payload,
		int timeoutSec) {
	int inPipe[2] = {-1, -1};
	if (pipe2(inPipe, O_CLOEXEC) != 0) {
		return ConfirmRun{false, false, "创建 stdin 管道失败（errno=" + std::to_string(errno) + "）", -1, 0};
	}
	pid_t pid = fork();
	if (pid < 0) {
		const int saved = errno;
		::close(inPipe[0]);
		::close(inPipe[1]);
		return ConfirmRun{false, false, std::string("fork 失败: ") + std::strerror(saved), -1, 0};
	}
	if (pid == 0) {
		/* 子进程：stdin 接管道读端（dup2 落到 fd 0，CLOEXEC 随之清除），
		 * 独立进程组便于超时整组终止；exec 失败按约定退出码 127。 */
		if (::dup2(inPipe[0], STDIN_FILENO) < 0) { _exit(127); }
		::setpgid(0, 0);
		::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
		_exit(127);
	}
	/* 与子进程竞争设置进程组，谁先成功效果一致（runShell 同款） */
	::setpgid(pid, pid);
	::close(inPipe[0]);

	ConfirmRun out;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);

	/* 写 payload：非阻塞 + poll 循环，防「确认程序不读 stdin + 参数巨大」把
	 * 宿主管死。SIGPIPE 以 pthread_sigmask 局部屏蔽（不动进程级 disposition
	 * ——宿主还有别的管道写者，改全局处置是越权）。 */
	{
		sigset_t oldMask{}, blockMask{};
		sigemptyset(&blockMask);
		sigaddset(&blockMask, SIGPIPE);
		pthread_sigmask(SIG_BLOCK, &blockMask, &oldMask);
		std::size_t written = 0;
		fcntl(inPipe[1], F_SETFL, fcntl(inPipe[1], F_GETFL) | O_NONBLOCK);
		while (written < payload.size()) {
			ssize_t n = ::write(inPipe[1], payload.data() + written, payload.size() - written);
			if (n > 0) { written += static_cast<std::size_t>(n); continue; }
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				if (std::chrono::steady_clock::now() >= deadline) { break; }
				struct pollfd pfd = {inPipe[1], POLLOUT, 0};
				(void)::poll(&pfd, 1, 50);
				continue;
			}
			if (n < 0 && errno == EINTR) { continue; }
			/* EPIPE 等：确认程序不等输入就退了，照常走等待分支拿退出码 */
			break;
		}
		::close(inPipe[1]);
		pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
	}

	/* 等退出：WNOHANG + 短睡轮询到 deadline */
	int status = 0;
	bool exited = false;
	while (std::chrono::steady_clock::now() < deadline) {
		pid_t w = retryEintr([&] { return ::waitpid(pid, &status, WNOHANG); });
		if (w == pid) { exited = true; break; }
		struct timespec ts = {0, 20 * 1000 * 1000};	/* 20ms */
		nanosleep(&ts, nullptr);
	}
	if (!exited) {
		/* 超时：整组 SIGKILL 并回收（进程组已 setpgid；万一没设置成功，
		 * 再对裸 pid 补一枪，两发都允许失败） */
		::kill(-pid, SIGKILL);
		(void)::kill(pid, SIGKILL);
		pid_t w = retryEintr([&] { return ::waitpid(pid, &status, 0); });
		(void)w;
		out.timedOut = true;
		return out;
	}
	if (WIFEXITED(status)) {
		out.ok = true;
		out.exitCode = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		out.ok = true;
		out.termSig = WTERMSIG(status);
	} else {
		out.failReason = "waitpid 返回未知的子进程状态";
	}
	return out;
}

} // namespace

Json maskToolArgs(const std::string& tool, const Json& args) {
	if (tool != "run_command" || !args.isObj()) { return args; }
	const Json* cmd = args.find("command");
	if (cmd == nullptr || !cmd->isStr()) { return args; }
	const std::string masked = maskSecretsInText(cmd->s);
	if (masked == cmd->s) { return args; }	/* 没变化就不重写，保持原节点 */
	Json out = args;
	out.put("command", JsonString(masked));
	return out;
}

std::shared_ptr<const AgentPolicy> AgentPolicy::load(const std::string& path,
		std::string& err) {
	std::string text;
	if (!readWholeFileLimited(path, kPolicyFileMaxBytes, text, err, "策略文件")) {
		return nullptr;
	}
	Json root;
	try {
		root = parseJson(text);
	} catch (const std::exception& e) {
		err = "策略文件 " + path + " 不是合法 JSON: " + e.what();
		return nullptr;
	}
	if (!root.isObj()) {
		err = "策略文件 " + path + " 的顶层必须是 JSON 对象";
		return nullptr;
	}
	static const std::set<std::string> kKnown = {
		"require_confirm", "confirm_command", "confirm_timeout_sec", "on_timeout",
	};
	for (const auto& kv : root.items()) {
		if (kKnown.count(kv.first) == 0) {
			err = "策略文件 " + path + " 含未知字段 '" + kv.first + "'（拼写错误？）";
			return nullptr;
		}
	}

	auto policy = std::make_shared<AgentPolicy>();
	if (const Json* rc = root.find("require_confirm")) {
		if (!rc->isArr()) {
			err = typeError(path, "require_confirm", "字符串数组");
			return nullptr;
		}
		for (const Json& t : rc->arr) {
			if (!t.isStr() || t.getValue().empty()) {
				err = typeError(path, "require_confirm", "非空字符串数组（元素不得为空/非字符串）");
				return nullptr;
			}
			policy->requireConfirm_.push_back(t.getValue());
		}
	}
	if (const Json* cc = root.find("confirm_command")) {
		if (!cc->isStr() || cc->getValue().empty()) {
			err = typeError(path, "confirm_command", "非空字符串");
			return nullptr;
		}
		policy->confirmCommand_ = cc->getValue();
	}
	if (!policy->requireConfirm_.empty() && policy->confirmCommand_.empty()) {
		err = "策略文件 " + path + " 声明了 require_confirm 但缺 confirm_command："
			"需要确认却没有确认程序，等于每次调用都被拒——请补上该字段，"
			"或删掉 require_confirm（不启用确认）";
		return nullptr;
	}
	if (policy->requireConfirm_.empty() && !policy->confirmCommand_.empty()) {
		err = "策略文件 " + path + " 声明了 confirm_command 但 require_confirm 为空/缺失："
			"没有工具会走到确认，请补 require_confirm 数组";
		return nullptr;
	}
	if (const Json* ct = root.find("confirm_timeout_sec")) {
		if (!ct->isInt()) {
			err = typeError(path, "confirm_timeout_sec", "整数（秒）");
			return nullptr;
		}
		const long long v = ct->getInt();
		if (v < kConfirmTimeoutMin || v > kConfirmTimeoutMax) {
			err = "策略文件 " + path + " 的 confirm_timeout_sec 超出区间 ["
				+ std::to_string(kConfirmTimeoutMin) + ", "
				+ std::to_string(kConfirmTimeoutMax) + "]（实测 "
				+ std::to_string(v) + "）";
			return nullptr;
		}
		policy->timeoutSec_ = static_cast<int>(v);
	}
	if (const Json* ot = root.find("on_timeout")) {
		if (!ot->isStr() || (ot->getValue() != "deny" && ot->getValue() != "allow")) {
			err = typeError(path, "on_timeout", "\"deny\" 或 \"allow\"");
			return nullptr;
		}
		policy->timeoutDeny_ = (ot->getValue() == "deny");
	}
	return policy;
}

bool AgentPolicy::requiresConfirm(const std::string& tool) const {
	for (const auto& t : requireConfirm_) {
		if (t == tool) { return true; }
	}
	return false;
}

PolicyDecision AgentPolicy::confirm(const std::string& tool, const Json& args) const {
	if (!requiresConfirm(tool)) { return PolicyDecision{true, ""}; }

	/* 交给外部程序的参数先遮蔽：确认程序的输出/截图/转发不受本仓控制 */
	Json payload = JsonObject();
	payload.put("tool", JsonString(tool));
	payload.put("args", maskToolArgs(tool, args));

	const ConfirmRun r = runConfirmProgram(confirmCommand_, payload.toString(), timeoutSec_);
	if (r.timedOut) {
		const std::string why = "确认程序超时（>" + std::to_string(timeoutSec_) + "s）";
		if (timeoutDeny_) {
			return PolicyDecision{false, why + "，按 on_timeout=deny 拒绝"};
		}
		return PolicyDecision{true, why + "，按 on_timeout=allow 放行"};
	}
	if (!r.ok) {
		return PolicyDecision{false, "确认程序调用失败: " + r.failReason};
	}
	if (r.termSig != 0) {
		return PolicyDecision{false, "确认程序被信号 " + std::to_string(r.termSig) + " 终止，拒绝"};
	}
	if (r.exitCode != 0) {
		return PolicyDecision{false, "确认程序拒绝（退出码 " + std::to_string(r.exitCode) + "）"};
	}
	return PolicyDecision{true, ""};
}

} // namespace agent
} // namespace bot
