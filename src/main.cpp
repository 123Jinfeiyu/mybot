#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "actionsource.hpp"
#include "agentdriver.hpp"
#include "agentmode.hpp"
#include "agent_subprocess.hpp"
#include "cliargs.hpp"
#include "robot.hpp"
#include "daemonvar.hpp"	/* abandonedDaemonCount（2.2 后不再经 robot.hpp 传导） */
#include "util.hpp"

namespace bot {

/* ---- D4：SIGINT/SIGTERM 处理器——首次仅置停止标志（async-signal-safe），
 * 让主流程走正常 deactivate（驱动 destroy / 守护停机）后再退出，
 * 避免 Ctrl-C 把舵机留在最后指令位置、I2C 从设备滞留中间态。
 * 第二次升级为强杀，见函数体注释。 */
extern "C" void onStopSignal(int sig) {
	bot::stopRequested().store(true);
	/* 第二次即升级为强杀：本处理器是永久安装的，若只置标志，运维连按 Ctrl-C
	 * 也停不下来（agent 模式一次模型调用最长 600s 且不可中断），唯一出路只剩
	 * 外部的 SIGKILL——而那会跳过 deactivate，把执行器留在带电状态。
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

/* agent 配置组装 + key 注入进程环境 + 告警打印；失败已打印，返回 false。
 * 密钥只在进程环境里流转：驱动的参数表与 BOT4CJ_DEBUG 的参数转储都不该
 * 出现它，真正读它的是 Lua 驱动自己。
 * 注入前后各做一件事把「环境来源的密钥」这条路收口：setenv **之前**抓取 exec 时
 * 环境块里敏感变量的原始串，注入**之后**抹零——/proc/<pid>/environ 对同 UID 进程
 * 可读，只清洗子进程继承的那份（buildCleanEnvp）挡不住它读常驻宿主的这份。 */
bool prepareAgentConfig(const CliOptions& options, agent::AgentModeConfig& agentCfg) {
	std::string cfgErr;
	if (!agent::buildAgentConfig(options, agentCfg, cfgErr)) {
		std::cerr << "错误: " << cfgErr << std::endl;
		return false;
	}
	std::vector<char*> envSecrets = agent::collectEnvSecrets();
	if (!agentCfg.resolvedApiKey.empty()) {
		::setenv("BOT4CJ_LLM_KEY", agentCfg.resolvedApiKey.c_str(), 1);
	}
	agent::scrubEnvSecrets(envSecrets);
	if (!agentCfg.configPath.empty()) {
		std::cerr << "配置文件: " << agentCfg.configPath << std::endl;
	}
	if (!agentCfg.configWarning.empty()) {
		std::cerr << agentCfg.configWarning << std::endl;
	}
	/* 密钥来源对 agent 的 shell 可见（命令行/配置文件/密钥文件）且
	 * run_command 启用时如实告警：同用户子进程没有权限壁垒。 */
	std::string keyWarn = agent::shellKeyExposureWarning(agentCfg);
	if (!keyWarn.empty()) { std::cerr << keyWarn << std::endl; }
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

/* agent 流水线：上电 → 驱动查找/传输适配 → 工具循环。异常上抛由 main
 * 的 catch 统一折成退出码 1。 */
int runAgentPipeline(RobotStruct& robot, const agent::AgentModeConfig& agentCfg) {
	/* agent 模式：上电后硬件常驻，控制权交给智能体工具循环；
	 * 只有会话结束（--task 跑完 / REPL 退出）才 deactivate。
	 * 传输要在 active() 之后才能绑定——驱动变量是那时才装配的。 */
	robot.active();

	MatrixVars_Hardware* driver = robot.registry().find(agentCfg.llmVar);
	if (driver == nullptr) {
		throw std::runtime_error("结构脚本里没有 LLM 驱动变量 '"
			+ agentCfg.llmVar + "'（驱动表 " + agentCfg.driverTable
			+ "）；检查 --llm-var/--llm-driver，或用 --help 看内置驱动名");
	}
	agent::AgentDriverConfig dcfg = agent::makeDriverConfig(agentCfg);
	std::unique_ptr<agent::ChatTransport> transport =
		agent::makeDriverTransport(driver, dcfg);

	return agent::runAgentMode(agentCfg, robot, *transport, std::cin, std::cout, std::cerr);
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

} // namespace bot

int main(int argc, char** argv) {
	using namespace bot;

	auto args = parseArgs(argc, argv);
	const CliOptions& options = args.opts;

	/* ---- 拉起帮助手册 ---- */
	if (options.help) {
		printHelp();
		return 0;
	}

	if (int vrc = validateOptions(args); vrc != 0) { return vrc; }

	/* ---- 结构脚本 + 动作输入源分支 ---- */
	if (options.robot) {
		std::string rpath = *options.robot;

		/* agent 配置先组装并校验（含配置文件读取与优先级合并）：
		 * 结构装载与上电都在其后，配置错误不碰真机 */
		agent::AgentModeConfig agentCfg;
		if (options.agent && !prepareAgentConfig(options, agentCfg)) { return 2; }

		std::unique_ptr<RobotStruct> robot = loadStructure(rpath);
		if (!robot) { return 1; }

		/* @Include 基准目录：--input 文件通道取脚本所在目录（相对路径直觉，
		 * 且与「include 名不得越出基准树」的词法规则自洽）；stdin/TCP 无脚本
		 * 位置，保持缺省 CWD。agent 模式的基准由 run_action_script 工具设为
		 * --scripts-dir（与该工具的相对路径语义一致），不在此处设置。 */
		if (options.input) {
			std::string parent = std::filesystem::path(*options.input).parent_path().string();
			robot->setActionIncludeBaseDir(parent.empty() ? "." : parent);
		}

		/* agent 模式：结构脚本没声明驱动变量就按 --llm-driver 注入一颗。
		 * 必须在 active() 之前——变量是在 active() 里 createHardVars 装配的。
		 * 显式声明优先：脚本作者写了同名变量就照用，不重复注入。 */
		if (options.agent && !robot->declaresVariable(agentCfg.llmVar)) {
			try {
				robot->injectLuaDriver(agentCfg.driverTable, agentCfg.llmVar);
			} catch (const std::exception& e) {
				std::cerr << "错误: 无法注入 LLM 驱动变量: " << e.what() << std::endl;
				return 2;
			}
		}
		/* 摄像头同理（未声明则注入 modules/Camera.lua）。摄像头是可选外设：
		 * 驱动 build 打不开设备也只记原因、不阻断上电，抓拍时才如实报错。 */
		if (options.agent && agentCfg.allowCamera
			&& !robot->declaresVariable(agentCfg.cameraVar)) {
			try {
				robot->injectLuaDriver("Camera", agentCfg.cameraVar);
			} catch (const std::exception& e) {
				std::cerr << "错误: 无法注入摄像头驱动变量: " << e.what() << std::endl;
				return 2;
			}
		}

		/* D4：安装 SIGINT/SIGTERM 处理器（仅置标志），Ctrl-C 走正常清理链 */
		struct sigaction sa{};
		sa.sa_handler = onStopSignal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;	/* 不用 SA_RESTART：让 accept 等阻塞点被 EINTR 打断以便检查标志 */
		::sigaction(SIGINT, &sa, nullptr);
		::sigaction(SIGTERM, &sa, nullptr);

		int rc = 0;
		try {
			rc = options.agent ? runAgentPipeline(*robot, agentCfg)
					   : runActionPipeline(*robot, options);
		} catch (const std::exception& e) {
			std::cerr << "Aborted: " << e.what() << std::endl;
			rc = 1;
		} catch (...) {
			/* 本机 LuaJIT 用 C++ 异常实现 luaD_throw（导入 _Unwind_RaiseException
			 * 而无 setjmp/longjmp），从没有 Lua CFRAME 的 C++ 帧抛出的 Lua 错误
			 * 不是 std::exception——漏了它就直接 std::terminate，下面的
			 * deactivate 被跳过，执行器停在最后指令位置。 */
			std::cerr << "Aborted: non-standard exception (Lua error or foreign throw)"
				<< std::endl;
			rc = 1;
		}

		/* 无论有没有成功激活，都进行清理（对应 Cangjie finally） */
		robot->deactivate();
		/* N-7：弃管守护的 lua_State 在 exit 路径不可回收——退出前让泄漏可见 */
		if (int abandoned = abandonedDaemonCount(); abandoned > 0) {
			std::cerr << "Warning: " << abandoned << " daemon thread(s) abandoned at shutdown"
				<< " (join timeout); their Lua states are released by process exit only"
				<< std::endl;
		}
		return rc;
	}

	/* 没有输入机器人结构脚本，打印帮助文档 */
	printHelp();
	return 0;
}
