/* pipe2 需要 _GNU_SOURCE（glibc 仅在 __USE_GNU 下声明）。g++/clang 编 C++ 时通常
 * 已定义，此处显式兜底并置于任何头文件之前（重复定义会触发 -Wpedantic 告警）。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agenttools.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "actionsource.hpp"
#include "hardware.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

/* 进程环境块。glibc 不在 <unistd.h> 里声明它，必须在**全局作用域**声明：
 * 放进匿名命名空间会变成另一个符号（bot::agent::{anon}::environ），链接期
 * 报 undefined reference。 */
extern char** environ;

namespace bot {
namespace agent {

namespace {

/* ---- 参数取值 ---- */

/* double → int 的合法值域：int 的两个端点都可被 double 精确表示，故直接取界。
 * 越界或非有限一律拒绝，绝不落到 static_cast（那才是 UB 所在）。 */
constexpr double kMinIntAsDouble = static_cast<double>(std::numeric_limits<int>::min());
constexpr double kMaxIntAsDouble = static_cast<double>(std::numeric_limits<int>::max());

std::string requireString(const Json& args, const std::string& key) {
	if (!args.isObj()) { throw IllegalArgumentException("工具参数必须是 JSON 对象"); }
	auto v = args.get(key);
	if (!v || !v->isStr() || v->getValue().empty()) {
		throw IllegalArgumentException("缺少字符串参数 " + key);
	}
	return v->getValue();
}

int optionalInt(const Json& args, const std::string& key, int fallback) {
	auto v = args.get(key);
	/* 键缺失或显式 null 都算「未提供」：模型常给可选字段填 null，
	 * 这与参数类型写错是两回事，不应混为一谈。 */
	if (!v || v->isNull()) { return fallback; }
	if (v->isInt()) {
		long long i = v->getInt();
		/* 整数路径同样收口：Json 的 int64 直接窄化到 int 会静默回绕 */
		if (i < std::numeric_limits<int>::min() || i > std::numeric_limits<int>::max()) {
			throw IllegalArgumentException("参数 " + key + " 超出整数范围");
		}
		return static_cast<int>(i);
	}
	if (v->isFloat()) {
		double d = v->getFloat();
		/* P1-1 防线（2026-09-19 评估）：模型返回的 "timeout_seconds": 1e30 这类值
		 * 直接 static_cast<int> 是 float-cast-overflow UB（非有限同理）。与
		 * luabridge.cpp 的 int64 转换同一族判据，此处按 int 宽度取界。 */
		if (!std::isfinite(d) || d < kMinIntAsDouble || d > kMaxIntAsDouble) {
			throw IllegalArgumentException("参数 " + key + " 数值超出范围或非有限");
		}
		return static_cast<int>(d);
	}
	/* 类型不符不再静默回落（原实现 return fallback）：与 requireString 同款严格。
	 * 静默用缺省值会让模型把「参数被忽略」误读成「参数已生效」，而工具报错会
	 * 折成文本回传，模型据此就能自我修正。 */
	throw IllegalArgumentException("参数 " + key + " 类型不符：需要整数");
}

/* ---- 文件读取（带上限，拒绝而非截断）---- */

/* 中危 #5 防线（2026-09-19 审计）：打开必须带 O_NOFOLLOW，拒绝符号链接。
 * 路径的词法校验（resolveActionScriptPath）只保证「请求的名字」在 scripts 目录
 * 之下，而 open/ifstream 会跟随软链——能写 scripts/ 的本地进程预置一条软链，
 * 就能让任意文件被当动作脚本执行，且其首行注释还会经 list_action_scripts 回显
 * 进模型上下文。这里用 open + O_NOFOLLOW 一步到底（不做 lstat 后再 open 的
 * check-then-use，那中间有 TOCTOU 窗口）。
 * 残余：O_NOFOLLOW 只看最后一段，路径中间的目录若是软链仍会被穿过——不过能写
 * scripts/ 的进程本就能直接放真脚本，这不构成增量能力；要彻底收紧得改 openat
 * 逐段打开，收益不抵复杂度。 */
std::string readFileLimited(const std::string& path, std::size_t cap) {
	int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ELOOP) { throw IllegalArgumentException("拒绝符号链接: " + path); }
		throw IllegalArgumentException("无法打开文件 " + path);
	}
	std::string data;
	std::array<char, 8192> buf;
	for (;;) {
		ssize_t n = ::read(fd, buf.data(), buf.size());
		if (n < 0) {
			if (errno == EINTR) { continue; }
			::close(fd);
			throw IllegalArgumentException("读取文件失败: " + path);
		}
		if (n == 0) { break; }
		data.append(buf.data(), static_cast<std::size_t>(n));
		if (data.size() > cap) { break; }	/* 超限即停，别再往下读 */
	}
	::close(fd);
	if (data.size() > cap) {
		throw IllegalArgumentException("文件超过 " + std::to_string(cap) + " 字节上限: " + path);
	}
	return data;
}

/* 动作脚本首行注释作为说明（"# 标准动作" → "标准动作"）。 */
std::string firstComment(const std::string& content) {
	std::istringstream ss(content);
	std::string line;
	for (int i = 0; i < 20 && std::getline(ss, line); ++i) {
		std::string t = trim(line);
		if (t.empty()) { continue; }
		if (t[0] == '#') { return trim(t.substr(1)); }
		break;	/* 首行非注释即认为无说明 */
	}
	return "";
}

/* ---- run_command 的实现（超时 / 进程组终止 / 输出截断 / 环境清洗） ---- */

constexpr std::size_t kMaxCommandOutput = 64u * 1024;

struct RunResult {
	int exitCode = -1;
	bool timedOut = false;
	bool interrupted = false;
	/* 截断按流独立记账：kMaxCommandOutput 是「每流」上限（总量可达 2 倍），
	 * 原实现用单一 truncated 标志，stdout 触顶会连带把 stderr 也报成截断。 */
	bool outTruncated = false;
	bool errTruncated = false;
	std::string out;
	std::string err;
};

void appendCapped(std::string& dst, const char* p, std::size_t n, bool& truncated) {
	std::size_t room = kMaxCommandOutput > dst.size() ? kMaxCommandOutput - dst.size() : 0;
	if (n > room) {
		dst.append(p, room);
		truncated = true;
	} else {
		dst.append(p, n);
	}
}

extern char** environ;

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
	fcntl(po[0], F_SETFL, O_NONBLOCK);
	fcntl(pe[0], F_SETFL, O_NONBLOCK);

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

/* ---- JSON Schema 小工具 ---- */

Json stringParam(const std::string& desc) {
	Json p = JsonObject();
	p.put("type", JsonString("string"));
	p.put("description", JsonString(desc));
	return p;
}

Json intParam(const std::string& desc) {
	Json p = JsonObject();
	p.put("type", JsonString("integer"));
	p.put("description", JsonString(desc));
	return p;
}

Json toolSchema(const std::vector<std::pair<std::string, Json>>& props,
		const std::vector<std::string>& required) {
	Json properties = JsonObject();
	for (const auto& [name, schema] : props) { properties.put(name, schema); }
	Json req = JsonArray();
	for (const auto& r : required) { req.add(JsonString(r)); }
	Json s = JsonObject();
	s.put("type", JsonString("object"));
	s.put("properties", properties);
	s.put("required", req);
	return s;
}

/* 2.3：种类判定走虚 kindName（叶子类单点），此处只做展示文案映射 */
const char* kindOf(const MatrixVars_Hardware* hw) {
	if (hw == nullptr) { return "未知"; }
	std::string_view k = hw->kindName();
	if (k == "alias") { return "别名"; }
	if (k == "vars") { return "数值变量"; }
	if (k == "lua") { return "驱动"; }
	if (k == "daemon") { return "守护"; }
	return "未知";
}

} // namespace

bool AgentToolRegistry::contains(const std::string& name) const {
	for (const auto& t : tools_) {
		if (t.name == name) { return true; }
	}
	return false;
}

AgentToolResult AgentToolRegistry::execute(const std::string& name, const Json& args) const {
	for (const auto& t : tools_) {
		if (t.name != name) { continue; }
		try {
			return t.execute(args);
		} catch (const std::exception& e) {
			return {std::string("工具执行失败: ") + e.what(), ""};
		}
	}
	std::string avail;
	for (const auto& t : tools_) {
		if (!avail.empty()) { avail += ", "; }
		avail += t.name;
	}
	return {"未知工具 " + name + "。可用工具: " + avail, ""};
}

std::vector<ToolSpec> AgentToolRegistry::specs() const {
	std::vector<ToolSpec> out;
	out.reserve(tools_.size());
	for (const auto& t : tools_) { out.push_back(ToolSpec{t.name, t.description, t.parameters}); }
	return out;
}

std::string resolveActionScriptPath(const std::string& scriptsDir, const std::string& requested) {
	std::string s = trim(requested);
	if (s.empty()) { throw IllegalArgumentException("缺少 script 参数（动作脚本相对路径）"); }
	if (s[0] == '/') {
		throw IllegalArgumentException("script 必须是脚本目录下的相对路径，不接受绝对路径: " + s);
	}
	if (s.find('\0') != std::string::npos) {
		throw IllegalArgumentException("script 含非法字符（NUL）");
	}
	std::vector<std::string> parts;
	std::size_t b = 0;
	while (b <= s.size()) {
		std::size_t e = s.find('/', b);
		std::string part = (e == std::string::npos) ? s.substr(b) : s.substr(b, e - b);
		if (part.empty() || part == ".") {
			throw IllegalArgumentException("script 路径含空段或 '.' : " + s);
		}
		if (part == "..") {
			throw IllegalArgumentException("script 不允许越出脚本目录: " + s);
		}
		parts.push_back(part);
		if (e == std::string::npos) { break; }
		b = e + 1;
	}
	if (parts.back().size() < 3 || parts.back().compare(parts.back().size() - 3, 3, ".mt") != 0) {
		throw IllegalArgumentException("只允许执行 .mt 动作脚本: " + s);
	}
	std::string full = scriptsDir;
	while (full.size() > 1 && full.back() == '/') { full.pop_back(); }
	for (const auto& p : parts) { full += "/" + p; }
	return full;
}

std::string describeActionScripts(const std::string& scriptsDir) {
	fs::path root(scriptsDir);
	std::error_code ec;
	if (!fs::is_directory(root, ec)) {
		return "(动作脚本目录不存在: " + scriptsDir + ")";
	}
	std::vector<std::string> rels;
	try {
		for (const auto& e :
		     fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
			std::error_code tec;
			if (!e.is_regular_file(tec)) { continue; }
			/* 软链一律不收（中危 #5）：脚本目录里放一条软链，就能把目录外的
			 * 任意文件拉进来当脚本执行，并让它的首注释经本清单回显给模型。 */
			if (e.is_symlink(tec)) { continue; }
			if (e.path().extension() != ".mt") { continue; }
			/* 纯词法求相对路径：fs::relative 内部会 weakly_canonical（跟随
			 * 软链），对软链目录项会算出带 `..` 的越界路径。 */
			fs::path rel = e.path().lexically_relative(root);
			if (rel.empty() || *rel.begin() == "..") { continue; }
			rels.push_back(rel.generic_string());
		}
	} catch (const fs::filesystem_error&) {
		/* 个别子目录不可读：已收集的照常输出，不因一处权限问题整段失败 */
	}
	std::sort(rels.begin(), rels.end());
	if (rels.empty()) { return "(脚本目录下没有 .mt 动作脚本: " + scriptsDir + ")"; }
	std::string out;
	for (const auto& rel : rels) {
		std::string desc;
		try {
			desc = firstComment(readFileLimited((root / rel).string(), 4096));
		} catch (const std::exception&) {
			desc.clear();
		}
		out += "  " + rel;
		if (!desc.empty()) { out += "  — " + desc; }
		out += "\n";
	}
	return out;
}

std::string describeHardware(RobotStruct& robot) {
	const auto& m = robot.registry().byName();
	if (m.empty()) { return "(没有注册任何硬件变量)"; }
	std::size_t width = 0;
	for (const auto& [name, hw] : m) {
		(void)hw;
		width = std::max(width, name.size());
	}
	std::string out = "已注册硬件变量（" + std::to_string(m.size()) + " 个）:\n";
	for (const auto& [name, hw] : m) {
		out += "  " + name + std::string(width - name.size() + 2, ' ') + kindOf(hw) + "\n";
	}
	return out;
}

/* ---- 各工具的装配函数（阶段 3 拆分）：makeAgentTools 只做组装 ---- */

static AgentTool makeListActionScriptsTool(const AgentToolContext& ctx) {
	return AgentTool{
		"list_action_scripts",
		"列出机器人可用的动作脚本（.mt 文件），返回相对路径与说明。"
		"需要让机器人做动作前先调用它，挑选与目标最匹配的脚本。",
		toolSchema({}, {}),
		[ctx](const Json&) -> AgentToolResult {
			return {describeActionScripts(ctx.scriptsDir), ""};
		}};
}

static AgentTool makeRunActionScriptTool(const AgentToolContext& ctx) {
	return AgentTool{
		"run_action_script",
		"在常驻的机器人上执行一个动作脚本（.mt），例如站立、前进、转向、举爪。"
		"脚本执行完毕后硬件保持上电常驻，可以接着执行下一个脚本。"
		"script 必须是 list_action_scripts 返回的相对路径。返回执行成功或报错原因；"
		"报错时机器人状态可能停在半途，请读清原因再决定下一步。",
		toolSchema({{"script", stringParam("动作脚本相对路径，如 std.mt 或 test/pwm.mt")}},
			{"script"}),
		[ctx](const Json& args) -> AgentToolResult {
			std::string req;
			try {
				if (ctx.robot == nullptr) { throw std::runtime_error("没有常驻机器人实例"); }
				req = requireString(args, "script");
				std::string path = resolveActionScriptPath(ctx.scriptsDir, req);
				std::string content = readFileLimited(path, kMaxScriptBytes);
				if (trim(content).empty()) { throw IllegalArgumentException("脚本内容为空: " + path); }
				const std::size_t bytes = content.size();
				auto reader = std::make_unique<LimitedReader>(
					std::make_unique<MemoryReader>(std::move(content)),
					kMaxScriptBytes, "action script");
				ctx.robot->run(*reader);
				return {"已执行动作脚本 " + req + "（" + std::to_string(bytes) + " 字节）", ""};
			} catch (const std::exception& e) {
				return {"动作脚本执行失败 [" + req + "]: " + e.what(), ""};
			}
		}};
}

static AgentTool makeListHardwareTool(const AgentToolContext& ctx) {
	return AgentTool{
		"list_hardware",
		"列出当前机器人的硬件变量清单（结构脚本注册的驱动、别名、数值变量）。"
		"只读清单，不会让任何器件动作。",
		toolSchema({}, {}),
		[ctx](const Json&) -> AgentToolResult {
			std::string out;
			if (!ctx.structurePath.empty()) { out += "结构脚本: " + ctx.structurePath + "\n"; }
			if (ctx.robot == nullptr) {
				out += "(没有常驻机器人实例)";
				return {out, ""};
			}
			out += describeHardware(*ctx.robot);
			return {out, ""};
		}};
}

static AgentTool makeCaptureImageTool(const AgentToolContext& ctx) {
	return AgentTool{
		"capture_image",
		"拍摄一张机器人当前环境的照片。图片会直接进入你的上下文，你能看到画面内容；"
		"任务涉及观察环境、寻找物体、判断前方是否有障碍时，先调用它再做结论。",
		toolSchema({}, {}),
		[ctx](const Json&) -> AgentToolResult {
			Json params = JsonObject();
			/* 只在显式配置时才传 path/src：结构脚本若把同名列声明成元数据列，
			 * 再传同名参数会被 rejectReservedKeys 拒掉（结构脚本优先）。 */
			if (!ctx.cameraDevice.empty()) {
				params.put("path", JsonString(ctx.cameraDevice));
			}
			if (!ctx.cameraSrc.empty()) {
				params.put("src", JsonString(ctx.cameraSrc));
				/* src_root 把「允许读取的目录」一并交给驱动（P1-2 防线）：
				 * 驱动据此只放行该目录内的文件，任意路径读（/etc/shadow 之类）
				 * 被拒。目录取配置路径的真实所在目录（canonical 展开软链与
				 * 相对段，与驱动侧 realpath 判据一致）；路径尚不存在时退一步
				 * 解析到最深的已存在父目录，好让驱动报「打不开」而不是
				 * 「未放行」。 */
				std::error_code ec;
				fs::path real = fs::canonical(ctx.cameraSrc, ec);
				if (ec) { real = fs::weakly_canonical(ctx.cameraSrc, ec); }
				std::string root = real.parent_path().string();
				if (root.empty()) { root = "."; }
				params.put("src_root", JsonString(root));
			}
			Json res = ctx.camera->playQuiet(params);	/* ERR 契约在此抛出 */
			auto b64 = res.get("b64");
			if (!b64 || !b64->isStr() || b64->getValue().empty()) {
				throw std::runtime_error("摄像头驱动没有返回图像数据（b64 为空）");
			}
			std::string desc = "已拍摄照片";
			auto w = res.get("width");
			auto h = res.get("height");
			if (w && h && w->isInt() && h->isInt()) {
				desc += "（" + std::to_string(w->getInt()) + "x"
					+ std::to_string(h->getInt()) + "）";
			}
			auto bytes = res.get("bytes");
			if (bytes && bytes->isInt()) {
				desc += "，" + std::to_string(bytes->getInt()) + " 字节";
			}
			auto src = res.get("source");
			if (src && src->isStr() && src->getValue() == "file") {
				desc += "，来源：模拟图片";
			}
			return {desc + "。画面已加入你的上下文。",
				"data:image/jpeg;base64," + b64->getValue()};
		}};
}

static AgentTool makeRunCommandTool(const AgentToolContext& ctx) {
	return AgentTool{
		"run_command",
		"在机器人主机上执行 shell 命令，返回退出码、标准输出与标准错误。"
		"适合查看系统状态、读取日志、检查设备节点；不要用它直接驱动硬件"
		"（驱动硬件请用 run_action_script）。命令有超时限制，输出超过 64KB 会截断。",
		toolSchema({{"command", stringParam("要执行的 shell 命令")},
			    {"timeout_seconds", intParam("超时秒数，默认 10，最大 60")}},
			{"command"}),
		[ctx](const Json& args) -> AgentToolResult {
			std::string cmd = requireString(args, "command");
			int timeoutSec = optionalInt(args, "timeout_seconds", ctx.commandTimeoutSec);
			if (timeoutSec < 1) { timeoutSec = 1; }
			if (timeoutSec > ctx.maxCommandTimeoutSec) { timeoutSec = ctx.maxCommandTimeoutSec; }
			try {
				return {formatRunResult(runShell(cmd, timeoutSec)), ""};
			} catch (const std::exception& e) {
				return {std::string("命令执行失败: ") + e.what(), ""};
			}
		}};
}

AgentToolRegistry makeAgentTools(const AgentToolContext& ctx) {
	AgentToolRegistry reg;
	reg.add(makeListActionScriptsTool(ctx));
	reg.add(makeRunActionScriptTool(ctx));
	reg.add(makeListHardwareTool(ctx));
	if (ctx.camera != nullptr) { reg.add(makeCaptureImageTool(ctx)); }
	if (ctx.allowShell) { reg.add(makeRunCommandTool(ctx)); }
	return reg;
}

} // namespace agent
} // namespace bot
