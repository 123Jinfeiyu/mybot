#include <algorithm>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "actionsource.hpp"
#include "agentdriver.hpp"
#include "agentmode.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace bot {

/* ---- D4：SIGINT/SIGTERM 处理器——仅置停止标志（async-signal-safe），
 * 让主流程走正常 deactivate（驱动 destroy / 守护停机）后再退出，
 * 避免 Ctrl-C 把舵机留在最后指令位置、I2C 从设备滞留中间态。 */
extern "C" void onStopSignal(int) {
	bot::stopRequested().store(true);
}

/* ---- 命令行参数解析：--key=value 与 --key value 双形态 ----
 * 未知键收进 unknown（F-20：由调用方统一拒绝，退出码 2）；
 * 带值键裸写收进 missingValue（N-4：旧实现回落哨兵值 "1"，`--tcp-host` 裸写
 * 会得到 host="1"、在 TCP 层报「地址无效」，与「漏写值」的用户意图脱节）。 */
struct ParsedArgs {
	bot::CliOptions opts;
	std::vector<std::string> unknown;
	std::vector<std::string> missingValue;
};

static ParsedArgs parseArgs(int argc, char** argv) {
	ParsedArgs out;
	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a.rfind("--", 0) != 0) { continue; }	/* 忽略位置参数 */
		std::string key = a.substr(2);
		std::string val;
		/* 审计中-3：区分「布尔型 flag」与「带值 key」——裸写（无 '=' 且
		 * 下一参数以 -- 开头或不存在）时不再回落 val="1"。旧实现 `--tcp`
		 * 单独使用会静默得到端口 1，随后 bind 失败的报错与用户意图
		 * （漏写端口号）相去甚远。 */
		bool noValue = true;
		auto eq = key.find('=');
		if (eq != std::string::npos) {
			val = key.substr(eq + 1);
			key = key.substr(0, eq);
			noValue = false;
		} else if (k + 1 < argc && std::string(argv[k + 1]).rfind("--", 0) != 0) {
			val = argv[++k];
			noValue = false;
		}
		if (key == "robot") {
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.robot = val; }
		}
		else if (key == "help") { out.opts.help = true; }
		else if (key == "tcp") {
			out.opts.tcp = true;
			/* 裸 --tcp：tcp 置位但端口留空，由 openActionSource 报「缺端口号」 */
			if (!noValue) { out.opts.tcpPort = val; }
		}
		else if (key == "tcp-host") {
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.tcpHost = val; }
		}
		else if (key == "tcp-token") {
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.tcpToken = val; }
		}
		else if (key == "tcp-idle") {
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.tcpIdle = val; }
		}
		else if (key == "input") {
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.input = val; }
		}
		/* ---- agent 模式（--agent 及其专属选项）----
		 * 裸写（漏值）一律进 missingValue，与既有 --tcp-host 等同口径 */
		else if (key == "agent") { out.opts.agent = true; out.opts.agentOptionsUsed = true; }
		else if (key == "agent-no-shell") {
			out.opts.agentNoShell = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "no-camera") {
			out.opts.noCamera = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "camera-var") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.cameraVar = val; }
		}
		else if (key == "camera-device") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.cameraDevice = val; }
		}
		else if (key == "camera-src") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.cameraSrc = val; }
		}
		else if (key == "task") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.agentTask = val; }
		}
		else if (key == "scripts-dir") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.scriptsDir = val; }
		}
		else if (key == "llm-endpoint") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmEndpoint = val; }
		}
		else if (key == "llm-model") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmModel = val; }
		}
		else if (key == "llm-key") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmKey = val; }
		}
		else if (key == "llm-config") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmConfig = val; }
		}
		else if (key == "llm-driver") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmDriver = val; }
		}
		else if (key == "llm-var") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmVar = val; }
		}
		else if (key == "llm-max-turns") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmMaxTurns = val; }
		}
		else if (key == "llm-max-tokens") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmMaxTokens = val; }
		}
		else if (key == "llm-timeout") {
			out.opts.agentOptionsUsed = true;
			if (noValue) { out.missingValue.push_back(key); }
			else { out.opts.llmTimeout = val; }
		}
		else { out.unknown.push_back(key); }
	}
	return out;
}

void print_help() {
	std::cout << "这是一个机器人动作脚本解析器。" << std::endl;
	std::cout << "使用--robot选项指定结构脚本，然后使用管道输入动作脚本，示例：" << std::endl;
	std::cout << "cat scripts/std.mt | ./build/mybot --robot=./robots/spider1.sql" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "动作输入源（优先级从高到低）：" << std::endl;
	std::cout << "  --tcp <port>       TCP 模式：监听端口接受一次成功连接，读取对端下发的 .mt" << std::endl;
	std::cout << "                     （EOF 或 --tcp-idle 超时按输入结束处理，非流式长驻）" << std::endl;
	std::cout << "  --tcp-host <addr>  TCP 监听地址，缺省 127.0.0.1；跨机监听（如 0.0.0.0）" << std::endl;
	std::cout << "                     必须同时提供 --tcp-token，否则拒绝启动" << std::endl;
	std::cout << "  --tcp-token <tok>  TCP 口令：跨机监听必填；对端须先发一行 \"<tok>\\n\" 再发脚本；" << std::endl;
	std::cout << "                     校验失败关连接回等，连续 5 次才退出；也可用环境变量 BOT4CJ_TCP_TOKEN" << std::endl;
	std::cout << "  --tcp-idle <sec>   TCP 空闲超时，缺省 30s（超时按输入结束处理）；" << std::endl;
	std::cout << "                     另有连接总时长预算（600s，随该值 x20 放大）防慢滴挂住进程" << std::endl;
	std::cout << "  --input <path>     从文件读取动作脚本" << std::endl;
	std::cout << "  （缺省）           从 stdin 管道读取" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "agent 模式（与上面的动作输入源互斥）：机器人结构脚本加载后硬件常驻内存，" << std::endl;
	std::cout << "模型调用由 Lua 协议驱动承担（不链接任何 HTTP 库，运行期只需 libcurl）。" << std::endl;
	std::cout << "  --agent            开启 agent 模式（缺省交互 REPL；硬件全程上电常驻）" << std::endl;
	std::cout << "  --task <文本>      一次性任务，跑完即退出；缺省则进入交互 REPL" << std::endl;
	std::cout << "  --llm-config <路径>  配置文件（JSON）：目标 URL、API key、模型、采样与 agent 参数" << std::endl;
	std::cout << "                       缺省按 ./mybot.llm.json -> ./mybot.llm.conf.json ->" << std::endl;
	std::cout << "                       $HOME/.config/mybot/llm.json 查找；传 none 表示本次不读配置文件；" << std::endl;
	std::cout << "                       也可用环境变量 BOT4CJ_LLM_CONFIG 指定" << std::endl;
	std::cout << "  --llm-driver <表名>  协议驱动表名 = modules/<表名>.lua" << std::endl;
	std::cout << "                       内置：LlmOpenAI（缺省，OpenAI 兼容）、LlmDeepSeek" << std::endl;
	std::cout << "  --llm-var <变量名>   承载驱动的变量名（缺省 llm0）；结构脚本已声明则直接用" << std::endl;
	std::cout << "  --llm-endpoint <URL>  接入点（覆盖配置文件）" << std::endl;
	std::cout << "  --llm-model <名称>    模型名（覆盖配置文件）" << std::endl;
	std::cout << "  --llm-key <KEY>       API key（覆盖配置文件）；会写入本进程环境变量" << std::endl;
	std::cout << "                       BOT4CJ_LLM_KEY。更推荐直接写进配置文件或设环境变量" << std::endl;
	std::cout << "                       （argv 对 /proc/<pid>/cmdline 可读）" << std::endl;
	std::cout << "  --llm-max-turns <N>   单次任务的最大模型轮数（缺省 8，范围 1-100）" << std::endl;
	std::cout << "  --llm-max-tokens <N>  单次生成最大 token 数（缺省 4096）" << std::endl;
	std::cout << "  --llm-timeout <秒>    单次模型调用超时（缺省 180）" << std::endl;
	std::cout << "  --scripts-dir <目录>  动作脚本根目录（缺省 scripts）；智能体只能执行其下的 .mt" << std::endl;
	std::cout << "  --agent-no-shell      不向智能体提供 run_command（去掉任意 shell 执行能力）" << std::endl;
	std::cout << "  --camera-var <变量名> 承载摄像头驱动的变量名（缺省 cam0）；结构脚本已声明则直接用" << std::endl;
	std::cout << "  --camera-device <路径> 摄像头设备（缺省 /dev/video0；需支持 MJPEG）" << std::endl;
	std::cout << "  --camera-src <图片>   模拟图片来源：不接摄像头也能跑通「拍照→进上下文」" << std::endl;
	std::cout << "  --no-camera           不向智能体提供 capture_image 工具" << std::endl;
	std::cout << "取值优先级：命令行 > 环境变量 > 配置文件 > 驱动内置缺省。" << std::endl;
	std::cout << "REPL 内置命令：/hardware 硬件清单，/scripts 动作脚本清单，/exit 退出。" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "示例：" << std::endl;
	std::cout << "  ./build/mybot --robot=robots/relay-test.sql --agent --task \"让继电器动作一次\"" << std::endl;
	std::cout << "  ./build/mybot --robot=robots/spider1.sql --agent --llm-config mybot.llm.json" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "安全约束：动作脚本只能引入新的参数键，不得覆盖结构脚本的元数据列" << std::endl;
	std::cout << "（fd / address / max / min / offset 等）与触发器引用名（trigger 列）；" << std::endl;
	std::cout << "结构脚本与动作脚本单次输入上限均为 8 MiB。" << std::endl;
}

} // namespace bot

int main(int argc, char** argv) {
	using namespace bot;

	auto args = parseArgs(argc, argv);
	const CliOptions& options = args.opts;

	/* ---- 拉起帮助手册 ---- */
	if (options.help) {
		print_help();
		return 0;
	}

	/* ---- F-20：未知/拼错的 --key 一律拒绝（退出码 2）----
	 * 旧行为是静默忽略：`--robott=foo` 打印帮助后 exit 0（CI 里的假成功），
	 * `--robot` 拼错还会让程序在「没给结构脚本」的路径上看似正常退出。 */
	if (!args.unknown.empty()) {
		std::sort(args.unknown.begin(), args.unknown.end());
		for (const auto& key : args.unknown) {
			std::cerr << "未知参数：--" << key << "；用 --help 查看支持的参数" << std::endl;
		}
		return 2;
	}

	/* N-4：带值键裸写（漏值）在解析层即拒绝，退出码与 unknown 一致 */
	if (!args.missingValue.empty()) {
		for (const auto& key : args.missingValue) {
			std::cerr << "选项 --" << key << " 缺少值；用法：--" << key << " <值>"
				<< "，用 --help 查看说明" << std::endl;
		}
		return 2;
	}

	/* ---- agent 模式选项校验（先于结构脚本装载与任何硬件上电）----
	 * F-20 同族口径：选项组合错必须显式拒绝并退出 2，不能静默忽略。
	 *   1) agent 专属选项出现在非 agent 模式下 → 用户意图与参数不一致；
	 *   2) --agent 与 --tcp/--input 互斥 → 两者都要求主导输入源。 */
	if (!options.agent && options.agentOptionsUsed) {
		std::cerr << "参数错误：--task / --llm-* / --agent-no-shell / --scripts-dir "
			"只在 --agent 模式下有效；用 --help 查看 agent 模式说明" << std::endl;
		return 2;
	}
	if (options.agent && (options.tcp || options.input)) {
		std::cerr << "参数错误：--agent 与 --tcp/--input 互斥"
			"（agent 模式以智能体工具循环为主流程，不接受一次性动作输入源）"
			<< std::endl;
		return 2;
	}
	/* agent 模式没有结构脚本就无从常驻，必须显式拒绝而不是打印帮助后退出 0 */
	if (options.agentOptionsUsed && !options.robot) {
		std::cerr << "参数错误：agent 模式需要同时提供 --robot <结构脚本>" << std::endl;
		return 2;
	}

	/* ---- 结构脚本 + 动作输入源分支 ---- */
	if (options.robot) {
		std::string rpath = *options.robot;

		/* agent 配置先组装并校验（含配置文件读取与优先级合并）：
		 * 结构装载与上电都在其后，配置错误不碰真机 */
		agent::AgentModeConfig agentCfg;
		if (options.agent) {
			std::string cfgErr;
			if (!agent::buildAgentConfig(options, agentCfg, cfgErr)) {
				std::cerr << "错误: " << cfgErr << std::endl;
				return 2;
			}
			/* 密钥只在进程环境里流转：驱动的参数表与 BOT4CJ_DEBUG 的参数转储
			 * 都不该出现它。真正读它的是 Lua 驱动自己。 */
			if (!agentCfg.resolvedApiKey.empty()) {
				::setenv("BOT4CJ_LLM_KEY", agentCfg.resolvedApiKey.c_str(), 1);
			}
			if (!agentCfg.configPath.empty()) {
				std::cerr << "配置文件: " << agentCfg.configPath << std::endl;
			}
			if (!agentCfg.configWarning.empty()) {
				std::cerr << agentCfg.configWarning << std::endl;
			}
		}

		/* 友好报错：结构脚本打不开 → 人话提示，不裸栈回溯 */
		std::unique_ptr<Reader> fstream;
		{
			auto f = std::make_unique<FileReader>(rpath);
			if (!f->good()) {
				std::cerr << "无法打开结构脚本 '" << rpath << "'" << std::endl;
				std::cerr << "请确认 --robot 指向存在的结构脚本，示例：cat scripts/std.mt | ./build/mybot --robot=robots/spider1.sql" << std::endl;
				return 1;
			}
			/* F-02：结构脚本与动作脚本共用同一字节上限——readSQL 先把全部
			 * token 驻留内存，不设限即「输入多大、进程就吃多大内存」。 */
			fstream = std::make_unique<LimitedReader>(std::move(f), kMaxScriptBytes, "structure script");
		}

		std::unique_ptr<RobotStruct> robot;
		try {
			robot = std::make_unique<RobotStruct>(*fstream);
		} catch (const std::exception& e) {
			std::cerr << "结构脚本解析失败 '" << rpath << "': " << e.what() << std::endl;
			std::cerr << "无硬件自检链路：cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql" << std::endl;
			return 1;
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
			if (options.agent) {
				/* agent 模式：上电后硬件常驻，控制权交给智能体工具循环；
				 * 只有会话结束（--task 跑完 / REPL 退出）才 deactivate。
				 * 传输要在 active() 之后才能绑定——驱动变量是那时才装配的。 */
				robot->active();

				MatrixVars_Hardware* driver = robot->registry().find(agentCfg.llmVar);
				if (driver == nullptr) {
					throw std::runtime_error("结构脚本里没有 LLM 驱动变量 '"
						+ agentCfg.llmVar + "'（驱动表 " + agentCfg.driverTable
						+ "）；检查 --llm-var/--llm-driver，或用 --help 看内置驱动名");
				}
				agent::AgentDriverConfig dcfg;
				dcfg.model = agentCfg.model;
				dcfg.endpoint = agentCfg.endpoint;
				dcfg.maxTokens = agentCfg.maxTokens;
				dcfg.temperature = agentCfg.temperature;
				dcfg.topP = agentCfg.topP;
				dcfg.timeoutSec = agentCfg.timeoutSec;
				dcfg.reasoningEffort = agentCfg.reasoningEffort;
				dcfg.thinking = agentCfg.thinking;
				std::unique_ptr<agent::ChatTransport> transport =
					agent::makeDriverTransport(driver, dcfg);

				rc = agent::runAgentMode(agentCfg, *robot, *transport,
					std::cin, std::cout, std::cerr);
			} else {
				/* 审计 H-2 防线：先取动作脚本、后上电。TCP 模式下 accept/口令/
				 * 读取阶段任何失败（无客户端连接、口令错、停止信号）都发生在
				 * 硬件 build 之前——不再出现「硬件已 build、带电空等客户端」；
				 * 三种输入源本就一次性读全，open 失败时 deactivate 也不会执行
				 * 任何 destroy 之外的驱动副作用。 */
				std::unique_ptr<Reader> input = openActionSource(options);
				if (!input) {
					std::cerr << "动作输入源打开失败，退出" << std::endl;
					rc = 1;
				} else {
					robot->active();

					robot->run(*input);
				}
			}
		} catch (const std::exception& e) {
			std::cerr << "Aborted: " << e.what() << std::endl;
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
	print_help();
	return 0;
}