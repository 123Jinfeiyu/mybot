#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <sys/prctl.h>
#include <unistd.h>

#include "actionsource.hpp"
#include "agentmode.hpp"
#include "mcpbackend.hpp"
#include "agent_subprocess.hpp"
#include "cliargs.hpp"
#include "robot.hpp"
#include "daemonvar.hpp"	/* abandonedDaemonCount（2.2 后不再经 robot.hpp 传导） */
#include "util_cli.hpp"
#include "util_env.hpp"
#include "util_except.hpp"
#include "util_num.hpp"

namespace bot {

/* ---- D4：SIGINT/SIGTERM 处理器——首次仅置停止标志（async-signal-safe），
 * 让主流程走正常 deactivate（驱动 destroy / 守护停机）后再退出，
 * 避免 Ctrl-C 把舵机留在最后指令位置、I2C 从设备滞留中间态。
 * 第二次升级为强杀，见函数体注释。 */
extern "C" void onStopSignal(int sig) {
	bot::stopRequested().store(true);
	/* 第二次即升级为强杀：本处理器是永久安装的，而**首次按下已经能优雅停下来**
	 * （置标志 → 阻塞点经 EINTR 打断后走正常清理链；动作脚本预算窗口内的
	 * 指令计数钩子同样看这个标志，见 src/luaexec.hpp），所以第二级的存在理由是
	 * 「**总能停下来**」这个兜底，而不是「常规停不下来」——优雅路径本身若被
	 * 卡住（驱动阻塞在不可中断的系统调用里、守护拒绝退场），仍需要一条出路。
	 * 但它会跳过 deactivate，把执行器留在带电状态：**是逃生口，不是推荐操作**。
	 * 还原缺省处置再重新触发本信号：signal/raise 均为 async-signal-safe。
	 * 计数用 sig_atomic_t；同类信号在处理器执行期间默认被屏蔽，无自竞争。 */
	static volatile sig_atomic_t hits = 0;
	if (++hits >= 2) {
		::signal(sig, SIG_DFL);
		::raise(sig);
	}
}

/* ---- 命令行解析 / 校验 / 帮助文本已迁 src/cliargs.*（可单测化）----
 * 此前它们定义在本文件里，而 main.cpp 不参与 mybot_tests 链接，CLI 面零单测。 */

/* 阶段 3 拆分：结构装载 / 两条流水线（选项校验已迁 cliargs） */

/* ---- 进程自护（2026-09-25）：PR_SET_DUMPABLE=0 ----
 * 本进程内存里有不该被同 UID 邻居顺手读走的东西：--tcp-token 口令、（历史上）
 * 注入进程环境的模型密钥、以及 run_command 回执里可能出现的任何值。此前的两道
 * 环境防线（buildCleanEnvp 清洗子进程环境、scrubEnvSecrets 抹零自己的环境块）
 * 都挡不住 **ptrace / 读 /proc/<pid>/mem**——docs/agent.md 安全一节如实写着
 * 「同 UID 不构成隔离」。non-dumpable 让内核把这条路收窄：
 *   * 同 UID 的非特权进程 PTRACE_ATTACH 被拒（__ptrace_may_access 对 non-dumpable
 *     目标要求 CAP_SYS_PTRACE），gdb -p / strace -p / 读 /proc/<pid>/mem 一并失效；
 *   * 崩溃不落 core（core 里就是整份内存，含密钥）；
 *   * /proc/<pid>/ 的属主变 root，environ / maps 等对同 UID 不再可读。
 * 边界如实：它挡的是「顺手一读」，root 与持 CAP_SYS_PTRACE 者不受影响；
 * /proc/<pid>/cmdline 依旧全局可读（这正是 --llm-key 被移除、TCP 口令推荐走
 * BOT4CJ_TCP_TOKEN 环境变量的理由，与本条互补而非替代）。
 * 放在 main 的**第一行**：argv 里可能就有 --tcp-token，parseArgs 之后再设已经晚了。
 * 两个逃生口：
 *   * BOT4CJ_ALLOW_PTRACE=1 —— 开发者要 gdb attach / strace -p 时显式放开，
 *     stderr 明写「已放开」（不静默）；
 *   * MYBOT_SANITIZE_BUILD —— sanitizer 构建整体跳过（LSan 的 StopTheWorld
 *     tracer 需要 ptrace 回父进程，理由见 CMakeLists.txt）。
 * 失败只告警不退出：prctl 失败（非 Linux 语义的容器 seccomp 之类）不该让机器人
 * 起不来，但操作者必须知道这层防线此刻缺位。 */
void hardenAgainstInspection() {
#ifdef MYBOT_SANITIZE_BUILD
	return;
#else
	if (auto allow = getEnvVar("BOT4CJ_ALLOW_PTRACE"); allow && *allow == "1") {
		std::cerr << "注意: BOT4CJ_ALLOW_PTRACE=1，本进程保持可被同 UID ptrace / core dump"
			"（仅调试用；密钥与口令可自内存读出）" << std::endl;
		return;
	}
	if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
		std::cerr << "警告: prctl(PR_SET_DUMPABLE, 0) 失败（" << errnoText(errno)
			<< "）；同 UID 进程可 ptrace 本进程 / 崩溃会落 core" << std::endl;
	}
#endif
}

/* agent 配置组装 + 环境密钥抹零 + 告警打印；失败已打印，返回 false。
 * 2026-09-26：agent 循环迁 Rust 后，宿主（--mcp-backend）不再解析或注入模型
 * 密钥——密钥由 mybot-agent 进程自持。这里仍保留 collectEnvSecrets/scrubEnvSecrets：
 * 操作者可能把 BOT4CJ_LLM_KEY 放进了宿主继承的环境块，抹零它可收窄
 * /proc/<宿主 pid>/environ 对同 UID 进程的暴露（与 buildCleanEnvp 清洗
 * run_command 子进程环境互补）。 */
bool prepareAgentConfig(const CliOptions& options, agent::AgentModeConfig& agentCfg) {
	std::string cfgErr;
	if (!agent::buildAgentConfig(options, agentCfg, cfgErr)) {
		std::cerr << "错误: " << cfgErr << std::endl;
		return false;
	}
	std::vector<char*> envSecrets = agent::collectEnvSecrets();
	agent::scrubEnvSecrets(envSecrets);
	if (!agentCfg.configPath.empty()) {
		std::cerr << "配置文件: " << agentCfg.configPath << std::endl;
	}
	/* 审计轨迹的落点如实告知（含 "-"=stderr）：审计是给事后复盘用的，
	 * 操作者应当知道它此刻是否在记、记到哪里。 */
	if (agentCfg.audit) {
		std::cerr << "审计日志: " << agentCfg.auditLogPath
			<< (agentCfg.auditLogPath == "-" ? "（stderr）" : "") << std::endl;
	}
	/* 策略钩子如实告知：命中 require_confirm 的工具每次调用都要过外部确认程序，
	 * 操作者应当知道「机器人会停下来等人/等审批」这件事正在生效。 */
	if (agentCfg.policy) {
		std::cerr << "策略钩子: " << agentCfg.policyPath
			<< "（命中 require_confirm 的工具调用需过外部确认程序）" << std::endl;
	}
	return true;
}

/* 结构脚本装载（友好报错 + 字节上限）；失败已打印，返回 nullptr */
std::unique_ptr<RobotStruct> loadStructure(const std::string& rpath) {
	/* 友好报错：结构脚本打不开 → 人话提示，不裸栈回溯 */
	std::unique_ptr<Reader> fstream;
	{
		auto f = std::make_unique<FileReader>(rpath);
		if (!f->good()) {
			std::cerr << "无法打开结构脚本 '" << rpath << "'" << std::endl;
			std::cerr << "请确认 --robot 指向存在的结构脚本，示例：cat scripts/std.mt | ./build/mybot --robot=robots/spider1.sql" << std::endl;
			return nullptr;
		}
		/* F-02：结构脚本与动作脚本共用同一字节上限——readSQL 先把全部
		 * token 驻留内存，不设限即「输入多大、进程就吃多大内存」。 */
		fstream = std::make_unique<LimitedReader>(std::move(f), kMaxScriptBytes, "structure script");
	}

	try {
		return std::make_unique<RobotStruct>(*fstream);
	} catch (const std::exception& e) {
		std::cerr << "结构脚本解析失败 '" << rpath << "': " << e.what() << std::endl;
		std::cerr << "无硬件自检链路：cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql" << std::endl;
		return nullptr;
	}
}

/* 动作流水线：先取动作脚本、后上电。返回退出码，异常上抛。 */
int runActionPipeline(RobotStruct& robot, const CliOptions& options) {
	/* 审计 H-2 防线：先取动作脚本、后上电。TCP 模式下 accept/口令/
	 * 读取阶段任何失败（无客户端连接、口令错、停止信号）都发生在
	 * 硬件 build 之前——不再出现「硬件已 build、带电空等客户端」；
	 * 三种输入源本就一次性读全，open 失败时 deactivate 也不会执行
	 * 任何 destroy 之外的驱动副作用。 */
	std::unique_ptr<Reader> input = openActionSource(options);
	if (!input) {
		std::cerr << "动作输入源打开失败，退出" << std::endl;
		return 1;
	}
	robot.active();
	robot.run(*input);
	return 0;
}

/* MCP 后端流水线（--mcp-backend，方案 F / 拓扑 F3）：上电 → 在主线程同步服务
 * 工具调用（阻塞直到停止信号 stopRequested）。**不注入 LLM 驱动、不发起任何
 * 模型调用**——工具集与能力门控由 runMcpBackend 内部的 makeToolContext +
 * makeAgentTools 决定（与 agent 模式同源，逐字节一致）。返回退出码，异常上抛
 * 由 runRobotAndCleanup 统一折成 1（并照常 deactivate）。 */
int runMcpBackendPipeline(RobotStruct& robot, const agent::AgentModeConfig& agentCfg,
		const CliOptions& options) {
	robot.active();
	agent::McpBackendOptions bopts;
	bopts.socketPath = options.mcpSocket;
	/* 每连接预算（评估 §3.2「预算再强制」）：cliargs 已校验为 [0,上限] 的整数，
	 * 空 = 不限（保持 0）。这里只做转换。注意这与**单次**物理调用的
	 * scriptTimeoutSec / commandTimeoutSec 是不同维度——后者始终在工具层生效。 */
	if (!options.mcpCallBudget.empty()) {
		bopts.callBudget = static_cast<long>(parseLongStrict(options.mcpCallBudget).second);
	}
	if (!options.mcpWallClock.empty()) {
		bopts.wallClockBudgetSec =
			static_cast<long>(parseLongStrict(options.mcpWallClock).second);
	}
	/* 空闲超时（A-2，2026-09-25 审计）：同一单点口径——cliargs 校验区间，
	 * 这里只做转换；空 = 不限（保持 0）。 */
	if (!options.mcpIdleTimeout.empty()) {
		bopts.idleTimeoutSec = static_cast<int>(parseLongStrict(options.mcpIdleTimeout).second);
	}
	return agent::runMcpBackend(robot, agentCfg, bopts, std::cerr);
}

/* 未声明则注入摄像头驱动变量（capture_image 工具需要；LLM 驱动已随 C++ REPL
 * 迁 Rust 删除——模型调用在 Rust 进程直连 provider，宿主不再注入 LLM 驱动）。
 * 必须在 active() 之前——变量是在 active() 里 createHardVars 装配的；
 * 显式声明优先：脚本作者写了同名变量就照用，不重复注入。
 * 失败已打印，返回 false。 */
bool injectCameraDriver(RobotStruct& robot, const agent::AgentModeConfig& agentCfg) {
	/* 摄像头是可选外设：驱动 build 打不开设备也只记原因、不阻断上电，
	 * 抓拍时才如实报错。 */
	if (agentCfg.allowCamera && !robot.declaresVariable(agentCfg.cameraVar)) {
		try {
			robot.injectLuaDriver("Camera", agentCfg.cameraVar);
		} catch (const std::exception& e) {
			std::cerr << "错误: 无法注入摄像头驱动变量: " << e.what() << std::endl;
			return false;
		}
	}
	return true;
}

/* --daemon-grace-ms 解析与校验（2026-09-22 F-2）：全局选项，不限于某个
 * 输入源/模式，凡结构脚本含 @Daemon 都可能触发 deactivate() 的二段屏障。
 * 空串 = 用缺省值（daemon_limits::kGraceMsDefault），不调用 setter。
 * 失败已打印人话报错，返回 false（调用方按「配置错误不碰真机」退出 2）。 */
bool applyDaemonGraceMs(const std::string& raw, RobotStruct& robot) {
	if (raw.empty()) { return true; }
	auto [state, value] = parseLongStrict(raw);
	if (state != IntParse::OK) {
		std::cerr << "选项 --daemon-grace-ms 需要一个整数，实际是 '" << raw << "'" << std::endl;
		return false;
	}
	if (value < daemon_limits::kGraceMsMin || value > daemon_limits::kGraceMsMax) {
		std::cerr << rangeErrorText("选项 --daemon-grace-ms",
			daemon_limits::kGraceMsMin, daemon_limits::kGraceMsMax, raw) << std::endl;
		return false;
	}
	robot.setDeactivateGraceMs(value);
	return true;
}

/* D4：安装 SIGINT/SIGTERM 处理器（仅置标志），Ctrl-C 走正常清理链 */
void installStopSignalHandlers() {
	struct sigaction sa{};
	sa.sa_handler = onStopSignal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;	/* 不用 SA_RESTART：让 accept 等阻塞点被 EINTR 打断以便检查标志 */
	::sigaction(SIGINT, &sa, nullptr);
	::sigaction(SIGTERM, &sa, nullptr);
}

/* 跑流水线 + 无论成败都清理 + 退出前让弃管守护可见。
 * 异常两类都要接住：std::exception 与「非标准异常」——本机 LuaJIT 用 C++ 异常
 * 实现 luaD_throw（导入 _Unwind_RaiseException 而无 setjmp/longjmp），从没有
 * Lua CFRAME 的 C++ 帧抛出的 Lua 错误不是 std::exception——漏了它就直接
 * std::terminate，下面的 deactivate 被跳过，执行器停在最后指令位置。
 * 判据锚点与四处边界清单：util.hpp 的 kNonStdExceptionNote 注释块。 */
int runRobotAndCleanup(RobotStruct& robot, const CliOptions& options,
		const agent::AgentModeConfig& agentCfg) {
	int rc = 0;
	try {
		if (options.mcpBackend) {
			/* MCP 后端（方案 F / 拓扑 F3）：独立模式，不争 stdin、不跑 agent loop。
			 * 同步服务阻塞主线程正是正确行为（评估 §2.3）；deactivate 照常在其后。 */
			rc = runMcpBackendPipeline(robot, agentCfg, options);
		} else {
			/* 动作管线（--tcp/--input/stdin）。--agent 已在 cliargs 校验阶段拒绝
			 * （agent 循环迁 Rust：mybot-agent --socket …，见 docs/agent.md），
			 * 这里不会再见到它。 */
			rc = runActionPipeline(robot, options);
		}
	} catch (const std::exception& e) {
		std::cerr << "Aborted: " << e.what() << std::endl;
		rc = 1;
	} catch (...) {
		std::cerr << "Aborted: non-standard exception (Lua error or foreign throw)"
			<< std::endl;
		rc = 1;
	}

	/* 无论有没有成功激活，都进行清理（对应 Cangjie finally） */
	robot.deactivate();
	/* N-7：弃管守护的 lua_State 在 exit 路径不可回收——退出前让泄漏可见 */
	if (int abandoned = abandonedDaemonCount(); abandoned > 0) {
		std::cerr << "Warning: " << abandoned << " daemon thread(s) abandoned at shutdown"
			<< " (join timeout); their Lua states are released by process exit only"
			<< std::endl;
	}
	return rc;
}

} // namespace bot

int main(int argc, char** argv) {
	using namespace bot;

	hardenAgainstInspection();	/* 先于 parseArgs：argv 可能含 --tcp-token */

	auto args = parseArgs(argc, argv);
	const CliOptions& options = args.opts;

	/* ---- 启动前哨：`--help` 优先于校验（顺序判定在 cliargs::preflightCli，
	 * 单测钉住——此前这段顺序写在 main 里，而 main 链接不进 mybot_tests，
	 * 用例只能测到 parseArgs，把顺序调换照样绿）---- */
	switch (preflightCli(args)) {
	case Preflight::Help:
		printHelp(options.helpTopic);  /* 2026-09-22: 支持分段帮助 */
		return 0;
	case Preflight::ConfigError:
		return 2;	/* 具体错误 validateOptions 已打印 */
	case Preflight::Proceed:
		break;
	}

	/* ---- 结构脚本 + 动作输入源分支 ---- */
	if (options.robot) {
		std::string rpath = *options.robot;

		/* agent 配置先组装并校验（含配置文件读取与优先级合并）：
		 * 结构装载与上电都在其后，配置错误不碰真机 */
		agent::AgentModeConfig agentCfg;
		if (options.mcpBackend && !prepareAgentConfig(options, agentCfg)) { return 2; }

		/* PL-1：--check-config —— 配置此时已校验完毕（错误路径上面已 return 2）。
		 * 这里只打印摘要后退出 0：**不装载结构脚本、不上电、不发起任何网络请求**。
		 * 放在 loadStructure 之前是有意的——校验模式连结构都不装。 */
		if (options.checkConfig) {
			std::cout << agent::summarizeAgentConfig(agentCfg);
			return 0;
		}

		std::unique_ptr<RobotStruct> robot = loadStructure(rpath);
		if (!robot) { return 1; }

		/* --daemon-grace-ms：配置错误不碰真机——在 active() 之前校验并设置，
		 * 与 @Include 基准目录同一批「上电前收尾」步骤。 */
		if (!applyDaemonGraceMs(options.daemonGraceMs, *robot)) { return 2; }

		/* @Include 基准目录：--input 文件通道取脚本所在目录（相对路径直觉，
		 * 且与「include 名不得越出基准树」的词法规则自洽）；stdin/TCP 无脚本
		 * 位置，保持缺省 CWD。agent 模式的基准由 run_action_script 工具设为
		 * --scripts-dir（与该工具的相对路径语义一致），不在此处设置。 */
		if (options.input) {
			std::string parent = std::filesystem::path(*options.input).parent_path().string();
			robot->setActionIncludeBaseDir(parent.empty() ? "." : parent);
		}

		if (options.mcpBackend && !injectCameraDriver(*robot, agentCfg)) { return 2; }

		installStopSignalHandlers();
		return runRobotAndCleanup(*robot, options, agentCfg);
	}

	/* 没有输入机器人结构脚本，打印帮助文档 */
	printHelp();
	return 0;
}
