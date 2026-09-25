/* cliargs.cpp —— 命令行解析 / 校验 / 帮助文本（自 main.cpp 纯搬移）
 *
 * 纯搬移声明：报错文案、退出码、打印顺序逐字不变；唯一形态变化是 print_help
 * 改为先合成字符串（helpText）再一次性输出 stdout——字节完全相同，但使
 * "--help 输出" 成为可被 C++ 单测锚定的对象（此前零守门）。
 */
#include "cliargs.hpp"

#include <algorithm>
#include <iostream>

#include "util.hpp"

namespace bot {

namespace {

/* 选项区间文本单点渲染（B2-4）：--help、校验共用同一组界，避免「帮助里写
 * 一个数、校验用另一个数」这类漂移。（llm_limits 的引用随 LLM 选项面迁
 * mybot-agent 一并移除；tcp_limits/daemon_limits 仍在此使用。） */
std::string rangeText(int lo, int hi) {
	return "范围 " + std::to_string(lo) + "-" + std::to_string(hi);
}

/* 重构计划 2.6：带值选项表——「agentOnly 标记 + 缺值登记 + 写入字段」
 * 此前是 19 段逐字重复的 if 链，新增选项靠复制粘贴；现在增项 = 表中加
 * 一行。布尔 flag 与「--tcp 的 flag+可选端口」形态特殊，留在表外显式处理。 */
struct OptionSpec {
	const char* name;
	void (*set)(CliOptions&, const std::string&);
	bool agentOnly;
};

constexpr OptionSpec kValueOptions[] = {
	{"robot", [](CliOptions& o, const std::string& v) { o.robot = v; }, false},
	{"tcp-host", [](CliOptions& o, const std::string& v) { o.tcpHost = v; }, false},
	{"tcp-token", [](CliOptions& o, const std::string& v) { o.tcpToken = v; }, false},
	{"tcp-idle", [](CliOptions& o, const std::string& v) { o.tcpIdle = v; }, false},
	{"tcp-max-auth-attempts", [](CliOptions& o, const std::string& v) { o.tcpMaxAuthAttempts = v; }, false},
	{"tcp-total-timeout", [](CliOptions& o, const std::string& v) { o.tcpTotalTimeout = v; }, false},
	{"daemon-grace-ms", [](CliOptions& o, const std::string& v) { o.daemonGraceMs = v; }, false},
	{"input", [](CliOptions& o, const std::string& v) { o.input = v; }, false},
	/* --mcp-socket：MCP 后端的 AF_UNIX 路径。非 agentOnly（它属 --mcp-backend
	 * 独立模式），backend-only 的组合校验在 validateOptions 里单点做。 */
	{"mcp-socket", [](CliOptions& o, const std::string& v) { o.mcpSocket = v; }, false},
	{"mcp-call-budget", [](CliOptions& o, const std::string& v) { o.mcpCallBudget = v; }, false},
	{"mcp-wall-clock", [](CliOptions& o, const std::string& v) { o.mcpWallClock = v; }, false},
	{"mcp-idle-timeout", [](CliOptions& o, const std::string& v) { o.mcpIdleTimeout = v; }, false},
	{"on-disconnect", [](CliOptions& o, const std::string& v) { o.onDisconnect = v; }, false},
	{"camera-var", [](CliOptions& o, const std::string& v) { o.cameraVar = v; }, true},
	{"camera-device", [](CliOptions& o, const std::string& v) { o.cameraDevice = v; }, true},
	{"camera-src", [](CliOptions& o, const std::string& v) { o.cameraSrc = v; }, true},
	{"audit-log", [](CliOptions& o, const std::string& v) { o.auditLog = v; }, true},
	{"agent-policy", [](CliOptions& o, const std::string& v) { o.agentPolicy = v; }, true},
	{"session-file", [](CliOptions& o, const std::string& v) { o.sessionFile = v; }, true},
	{"camera-src-dir", [](CliOptions& o, const std::string& v) { o.cameraSrcDir = v; }, true},
	{"task", [](CliOptions& o, const std::string& v) { o.agentTask = v; }, true},
	/* 可重复：每项一个可读变量名（发现 2 的 A 步，命令行独占） */
	{"read-var", [](CliOptions& o, const std::string& v) { o.readVars.push_back(v); }, true},
	{"scripts-dir", [](CliOptions& o, const std::string& v) { o.scriptsDir = v; }, true},
	{"prompt-file", [](CliOptions& o, const std::string& v) { o.promptFiles.push_back(v); }, true},
	{"llm-endpoint", [](CliOptions& o, const std::string& v) { o.llmEndpoint = v; }, true},
	{"llm-model", [](CliOptions& o, const std::string& v) { o.llmModel = v; }, true},
	{"llm-config", [](CliOptions& o, const std::string& v) { o.llmConfig = v; }, true},
	{"llm-driver", [](CliOptions& o, const std::string& v) { o.llmDriver = v; }, true},
	{"llm-var", [](CliOptions& o, const std::string& v) { o.llmVar = v; }, true},
	{"llm-max-turns", [](CliOptions& o, const std::string& v) { o.llmMaxTurns = v; }, true},
	{"llm-max-tokens", [](CliOptions& o, const std::string& v) { o.llmMaxTokens = v; }, true},
	{"llm-timeout", [](CliOptions& o, const std::string& v) { o.llmTimeout = v; }, true},
	{"llm-task-timeout", [](CliOptions& o, const std::string& v) { o.llmTaskTimeout = v; }, true},
};

} // namespace

ParsedArgs parseArgs(int argc, char** argv) {
	ParsedArgs out;
	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a.rfind("--", 0) != 0) { continue; }	/* 忽略位置参数 */
		/* 审计中-3：区分「布尔型 flag」与「带值 key」——裸写（无 '=' 且
		 * 下一参数以 -- 开头或不存在）时不再回落 val="1"。旧实现 `--tcp`
		 * 单独使用会静默得到端口 1，随后 bind 失败的报错与用户意图
		 * （漏写端口号）相去甚远。切分规则在 cli::parseKeyValue 单点。 */
		cli::OptionToken tok = cli::parseKeyValue(k, argc, argv, /*strictLookahead=*/true);
		const std::string& key = tok.key;
		const std::string& val = tok.value;
		bool noValue = !tok.hasValue;
		/* ---- 布尔 flag 与特殊形态：表外显式处理 ---- */
		if (key == "help") {
			out.opts.help = true;
			/* 2026-09-22: 支持 --help <topic> 分段帮助 */
			if (!noValue) { out.opts.helpTopic = val; }
		}
		else if (key == "tcp") {
			out.opts.tcp = true;
			/* 裸 --tcp：tcp 置位但端口留空，由 openActionSource 报「缺端口号」 */
			if (!noValue) { out.opts.tcpPort = val; }
		}
		else if (key == "agent") { out.opts.agent = true; out.opts.agentOptionsUsed = true; }
		else if (key == "agent-shell") {
			out.opts.agentShell = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "agent-hot-sql") {
			out.opts.agentHotSql = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "no-camera") {
			out.opts.noCamera = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "resume") {
			out.opts.resume = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "check-config") {
			out.opts.checkConfig = true;
			out.opts.agentOptionsUsed = true;
		}
		else if (key == "mcp-backend") {
			/* 独立模式：不置 agentOptionsUsed（它不是 agent 模式，只是复用 agent
			 * 的工具上下文选项）。组合校验见 validateOptions。 */
			out.opts.mcpBackend = true;
		}
		else {
			/* ---- 带值选项：表驱动 ---- */
			const OptionSpec* spec = nullptr;
			for (const auto& item : kValueOptions) {
				if (key == item.name) { spec = &item; break; }
			}
			if (spec == nullptr) { out.unknown.push_back(key); }
			else {
				/* agentOnly 先于缺值检查置位——裸写 --camera-var 也算
				 * 「用过 agent 选项」，与既有顺序一致 */
				if (spec->agentOnly) { out.opts.agentOptionsUsed = true; }
				if (noValue) { out.missingValue.push_back(key); }
				else { spec->set(out.opts, val); }
			}
		}
	}
	return out;
}

/* PL-4（2026-09-22）：判定从 main 抽到这里，理由见 cliargs.hpp 的声明注释。
 * 顺序是契约：**先 help 后校验**——同时给 `--help` 与非法键时，用户要的是帮助，
 * 不该被一串错误清单挡住（未知键仍被 parseArgs 记录，只是这条路不消费它）。 */
Preflight preflightCli(ParsedArgs& args) {
	if (args.opts.help) { return Preflight::Help; }
	return validateOptions(args) == 0 ? Preflight::Proceed : Preflight::ConfigError;
}

/* F-20/N-4/agent 选项组合校验：违规即打印并返回退出码 2，通过返回 0 */
int validateOptions(ParsedArgs& args) {
	const CliOptions& options = args.opts;

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

	/* ---- --agent 已移除（agent 循环迁 Rust）：在此快速失败 ----
	 * 旧路径会先做 LLM 驱动注入、取消哨兵装配、agent-over-TCP 甚至 accept 一条
	 * 连接，才在 main 的「已移除」桩上报错。F-20 同族口径：显式拒绝并退出 2，
	 * 文案给出替代用法（新拓扑：mybot --mcp-backend 提供工具后端，
	 * mybot-agent 跑模型循环，两者经 --mcp-socket 互联）。 */
	if (options.agent) {
		std::cerr << "参数错误：--agent 已移除（agent 循环已迁 Rust）。替代用法：\n"
			"  1) 启动工具后端：./build/mybot --robot <结构脚本> --mcp-backend "
			"--mcp-socket /tmp/mybot.sock\n"
			"  2) 启动 agent：./agent/target/release/mybot-agent --socket /tmp/mybot.sock\n"
			"（构建 agent：bash scripts/build_agent.sh；详见 docs/agent.md）"
			<< std::endl;
		return 2;
	}

	/* ---- 模型循环类选项：属于 Rust agent（mybot-agent），本程序一律拒绝 ----
	 * 这些是 mybot-agent 自己的选项面（它另有 --help）；本程序（C++ 宿主）不跑
	 * 模型循环。保留解析表条目是为了给出指路的报错，而不是「未知参数」。 */
	const char* loopOnly = nullptr;
	if (!options.agentTask.empty()) { loopOnly = "--task"; }
	else if (options.resume) { loopOnly = "--resume"; }
	else if (!options.sessionFile.empty()) { loopOnly = "--session-file"; }
	else if (!options.promptFiles.empty()) { loopOnly = "--prompt-file"; }
	else if (!options.llmEndpoint.empty()) { loopOnly = "--llm-endpoint"; }
	else if (!options.llmModel.empty()) { loopOnly = "--llm-model"; }
	else if (!options.llmConfig.empty()) { loopOnly = "--llm-config"; }
	else if (!options.llmDriver.empty()) { loopOnly = "--llm-driver"; }
	else if (!options.llmVar.empty()) { loopOnly = "--llm-var"; }
	else if (!options.llmMaxTurns.empty()) { loopOnly = "--llm-max-turns"; }
	else if (!options.llmMaxTokens.empty()) { loopOnly = "--llm-max-tokens"; }
	else if (!options.llmTimeout.empty()) { loopOnly = "--llm-timeout"; }
	else if (!options.llmTaskTimeout.empty()) { loopOnly = "--llm-task-timeout"; }
	if (loopOnly != nullptr) {
		std::cerr << "参数错误：" << loopOnly << " 属于 Rust agent（mybot-agent，"
			"经 --socket 连接本程序的工具后端）；C++ 宿主侧请用 --mcp-backend"
			<< std::endl;
		return 2;
	}

	/* ---- 工具门控类选项：只在 --mcp-backend 模式下有效 ----
	 * （先于结构脚本装载与任何硬件上电；F-20 同族口径：显式拒绝退出 2，
	 * 不静默忽略。） */
	if (!options.mcpBackend && options.agentOptionsUsed) {
		/* PL-5（2026-09-22）：选项名单与 docs 的选项表保持一致——用户敲了
		 * --no-camera 却被报「选项无效」，而文案里没有他刚敲的那一个，
		 * 看起来像撞上了另一个错误。 */
		std::cerr << "参数错误：--agent-shell "
			"/ --agent-hot-sql / --agent-policy / --read-var / --scripts-dir "
			"/ --camera-var / --camera-device / --camera-src / --camera-src-dir "
			"/ --no-camera / --audit-log / --check-config 只在 --mcp-backend 模式下有效；"
			"用 --help 查看说明"
			<< std::endl;
		return 2;
	}

	/* ---- MCP 后端模式（--mcp-backend）组合校验（方案 F / 拓扑 F3）----
	 * 与 F-20 同口径：组合错显式拒绝退出 2，不静默忽略。后端复用 agent 的工具
	 * 上下文选项（能力门控），但**不跑 loop、不发模型调用**，故 loop/LLM 专属
	 * 选项一律拒绝（silently ignored 会误导使用者）。 */
	if (!options.mcpBackend && !options.mcpSocket.empty()) {
		std::cerr << "参数错误：--mcp-socket 只在 --mcp-backend 模式下有效" << std::endl;
		return 2;
	}
	if (!options.mcpBackend
		&& (!options.mcpCallBudget.empty() || !options.mcpWallClock.empty()
			|| !options.mcpIdleTimeout.empty())) {
		std::cerr << "参数错误：--mcp-call-budget / --mcp-wall-clock / --mcp-idle-timeout"
			" 只在 --mcp-backend 模式下有效" << std::endl;
		return 2;
	}
	if (!options.mcpBackend && !options.onDisconnect.empty()) {
		std::cerr << "参数错误：--on-disconnect 只在 --mcp-backend 模式下有效"
			"（它是后端连接结束后的安全动作）" << std::endl;
		return 2;
	}
	if (options.mcpBackend) {
		if (options.tcp) {
			std::cerr << "参数错误：--mcp-backend 与 --tcp 互斥"
				"（后端经 AF_UNIX socket 服务，不监听 TCP 端口）" << std::endl;
			return 2;
		}
		if (options.input) {
			std::cerr << "参数错误：--mcp-backend 与 --input 互斥"
				"（后端不接受一次性动作输入源）" << std::endl;
			return 2;
		}
		if (!options.robot) {
			std::cerr << "参数错误：--mcp-backend 需要同时提供 --robot <结构脚本>" << std::endl;
			return 2;
		}
		if (options.mcpSocket.empty() && !options.checkConfig) {
			std::cerr << "参数错误：--mcp-backend 需要 --mcp-socket <AF_UNIX 路径>"
				"（mybot-agent 经此路径连接后端）" << std::endl;
			return 2;
		}
		/* 每连接预算数值校验（空 = 不限；配置错误不碰真机，故在此单点拒）。
		 * 上限只是防呆（跑飞护栏本就不该设成天文数字）。 */
		if (!options.mcpCallBudget.empty()) {
			auto [st, v] = parseLongStrict(options.mcpCallBudget);
			if (st != IntParse::OK || v < 0 || v > 1000000) {
				std::cerr << rangeErrorText("选项 --mcp-call-budget", 0, 1000000,
					options.mcpCallBudget) << std::endl;
				return 2;
			}
		}
		if (!options.mcpWallClock.empty()) {
			auto [st, v] = parseLongStrict(options.mcpWallClock);
			if (st != IntParse::OK || v < 0 || v > 86400) {
				std::cerr << rangeErrorText("选项 --mcp-wall-clock", 0, 86400,
					options.mcpWallClock) << std::endl;
				return 2;
			}
		}
		/* 空闲超时（A-2）：区间与墙钟同族（最长一天——空闲期不该比墙钟更宽松）。
		 * 0 = 不限（缺省），与既有预算选项同口径。 */
		if (!options.mcpIdleTimeout.empty()) {
			auto [st, v] = parseLongStrict(options.mcpIdleTimeout);
			if (st != IntParse::OK || v < 0 || v > 86400) {
				std::cerr << rangeErrorText("选项 --mcp-idle-timeout", 0, 86400,
					options.mcpIdleTimeout) << std::endl;
				return 2;
			}
		}
	}
	return 0;
}

/* TCP 选项块（全文帮助与 --help tcp 共用单点，2026-09-25 D3）：两处此前逐行
 * 手抄同一份，区间常量（rangeText）改一处漏一处只会让 --help 与
 * docs/reference.md 的黄金比对红一条而不知因——组装成函数后文本只有一份。 */
std::string tcpOptionsHelp() {
	std::string s;
	s += "  --tcp <port>       TCP 模式：监听端口接受一次成功连接，读取对端下发的 .mt\n";
	s += "                     （EOF 或 --tcp-idle 超时按输入结束处理，非流式长驻）\n";
	s += "  --tcp-host <addr>  TCP 监听地址，缺省 127.0.0.1；跨机监听（如 0.0.0.0）\n";
	s += "                     必须同时提供 --tcp-token，否则拒绝启动\n";
	s += "  --tcp-token <tok>  TCP 口令：跨机监听必填；对端须先发一行 \"<tok>\\n\" 再发脚本；\n";
	s += "                     校验失败关连接回等，连续 5 次才退出；也可用环境变量 BOT4CJ_TCP_TOKEN\n";
	s += "  --tcp-idle <sec>   TCP 空闲超时，缺省 30s（超时按输入结束处理）；\n";
	s += "                     另有连接总时长预算（600s，随该值 x20 放大）防慢滴挂住进程\n";
	s += "  --tcp-max-auth-attempts <N>  口令连续失败上限（缺省 5，"
		+ rangeText(tcp_limits::kAuthMaxFailsMin, tcp_limits::kAuthMaxFailsMax) + "）\n";
	s += "  --tcp-total-timeout <sec>    连接总时长预算下限（缺省 600，"
		+ rangeText(tcp_limits::kTotalTimeoutMin, tcp_limits::kTotalTimeoutMax)
		+ "）；生效值取该下限与 idle x20 的较大者\n";
	return s;
}

std::string helpText() {
	std::string s;
	s += "这是一个机器人动作脚本解析器。\n";
	s += "使用--robot选项指定结构脚本，然后使用管道输入动作脚本，示例：\n";
	s += "  cat scripts/std.mt | ./build/mybot --robot=./robots/spider1.sql\n";
	s += "\n";
	s += "动作输入源（优先级从高到低）：\n";
	s += tcpOptionsHelp();
	s += "  --input <path>     从文件读取动作脚本\n";
	s += "  （缺省）           从 stdin 管道读取\n";
	s += "  --daemon-grace-ms <ms>  守护线程弃管后的二段停机宽限上限（缺省 30000，"
		+ rangeText(daemon_limits::kGraceMsMin, daemon_limits::kGraceMsMax)
		+ "）；仅结构脚本含 @Daemon 时生效，与 --tcp/--input 等输入模式无关\n";
	s += "\n";
	s += "agent 体系（模型循环已迁 Rust：agent/target/release/mybot-agent）：\n";
	s += "本程序在 agent 体系里的职责是**工具后端**（--mcp-backend）：机器人结构脚本\n";
	s += "加载后硬件常驻内存，工具调用经 AF_UNIX socket 服务；LLM 循环（REPL/--task/\n";
	s += "预算/历史裁剪）由 mybot-agent 承担，它直连 provider、经 --socket 调用本程序。\n";
	s += "模型侧参数（--task/--llm-*/--prompt-file/--session-file/--resume）是\n";
	s += "mybot-agent 自己的选项——本程序会显式拒绝并指引；见 mybot-agent --help\n";
	s += "与 docs/agent.md。\n";
	s += "  --mcp-backend       把 agent 工具层经本地 AF_UNIX socket 暴露给 mybot-agent。\n";
	s += "                      独立模式：不跑模型循环、不发起任何模型调用——每次工具\n";
	s += "                      调用都走既有的「策略确认 → 执行 → 审计」漏斗。与\n";
	s += "                      --tcp/--input 互斥；需要 --robot 与 --mcp-socket\n";
	s += "  --mcp-socket <路径>  AF_UNIX socket 路径（mybot-agent 的 --socket 连这里）。\n";
	s += "                      父目录建议 0700、socket 文件 0600；对端经内核\n";
	s += "                      SO_PEERCRED 校验 uid/gid（不可伪造）\n";
	s += "  --mcp-call-budget <N>   每连接工具调用上限（跑飞护栏；0/空 = 不限，缺省不限）。\n";
	s += "                      后端不假设调用方是谁，故自行再强制一层预算\n";
	s += "  --mcp-wall-clock <秒>   每连接墙钟预算（0/空 = 不限，缺省不限；上限 86400）。\n";
	s += "                      注：单次物理调用的 script/command 超时始终在工具层生效\n";
	s += "  --mcp-idle-timeout <秒> 每连接空闲（无帧）超时（0/空 = 不限，缺省不限；上限 86400）。\n";
	s += "                      让「连上后一声不吭」的哑连接按时让出唯一的 accept 槽\n";
	s += "  --on-disconnect <脚本>  连接结束（对端关闭/崩溃/超时/协议违规，任何原因）后，\n";
	s += "                      后端自行执行的安全动作脚本（scripts-dir 之下的相对路径，\n";
	s += "                      如 zero.mt）。缺省不执行——此时硬件停在最后一个姿态带电\n";
	s += "                      常驻。脚本在上电前校验（越界/不存在/为空即退出 2）；\n";
	s += "                      不经策略确认（操作者配置的安全动作不该等外部闸门）；\n";
	s += "                      执行失败只记日志/审计，后端继续服务。SIGINT/SIGTERM 停机\n";
	s += "                      不触发（由断电清理链处理）\n";
	s += "  --scripts-dir <目录>  动作脚本根目录（缺省 scripts）；智能体只能执行其下的 .mt\n";
	s += "  --agent-shell         允许智能体执行 shell 命令（注册 run_command 工具）；\n";
	s += "                       **缺省不提供**该工具（任意 shell 执行是最大的一张牌，\n";
	s += "                       要用就显式开启；理由与缓解见 docs/agent.md 安全一节）。\n";
	s += "                       --mcp-backend 下开它**强制**要 --agent-policy 且其\n";
	s += "                       require_confirm 包含 run_command（无人值守暴露危险能力\n";
	s += "                       必须有外部确认闸门）\n";
	s += "  --agent-hot-sql       允许智能体用结构 SQL 改硬件（注册 apply_sql 工具）：\n";
	s += "                       CREATE TABLE / INSERT INTO 会**立即装配并上电**（开 fd、\n";
	s += "                       进断电清理链），失败整批回滚；DELETE 当场断电摘除、\n";
	s += "                       **不可逆**。**缺省不提供**，配置文件的 allow_hot_sql\n";
	s += "                       只能关不能开（只读的 query_sql 不受影响）；--mcp-backend\n";
	s += "                       下开它同样强制策略确认（apply_sql）\n";
	s += "  --read-var <变量名>    声明一个**可读**硬件变量（可重复；注册 read_variables 与\n";
	s += "                       wait_variable 工具，让智能体主动读器件当前值、或轮询等\n";
	s += "                       某个读数满足条件）。**缺省不提供**这两个工具：\n";
	s += "                       只有列进来的变量可读——执行器（Servo/Relay 等）也是 Lua\n";
	s += "                       驱动，靠种类分不出传感器与执行器，故由操作者显式声明；\n";
	s += "                       配置文件写 read_vars 会被拒绝加载（与 --agent-shell 同判据）\n";
	s += "  --agent-policy <路径> 策略钩子（严格 JSON）：声明哪些工具的每次调用要先过\n";
	s += "                       外部确认程序（stdin 收工具名与遮蔽后的参数 JSON，\n";
	s += "                       退出 0 = 放行）；超时按 on_timeout（缺省 deny）处理；\n";
	s += "                       缺省不启用。也可用配置文件 agent_policy 字段\n";
	s += "  --camera-var <变量名> 承载摄像头驱动的变量名（缺省 cam0）；结构脚本已声明则直接用\n";
	s += "  --camera-device <路径> 摄像头设备（缺省 /dev/video0；需支持 MJPEG）\n";
	s += "  --camera-src <图片>   模拟图片来源：不接摄像头也能跑通「拍照→进上下文」。\n";
	s += "                       命令行给的 src 以其**所在目录**为白名单根；配置文件里的\n";
	s += "                       camera_src 不推导根（防止一份落地配置把任意文件读进上下文），\n";
	s += "                       需配 --camera-src-dir 或 BOT4CJ_CAMERA_SRC_DIR，否则驱动默认拒绝\n";
	s += "  --camera-src-dir <目录> capture_image 模拟图片的白名单根目录（**只能命令行给**：\n";
	s += "                       它放宽读权限，配置文件写 camera_src_dir 会被拒绝加载）\n";
	s += "  --no-camera           不向智能体提供 capture_image 工具\n";
	s += "  --check-config        只校验配置并打印摘要后退出 0（与 --mcp-backend 联用）：\n";
	s += "                       不装载结构脚本、不上电、不发起任何网络请求（配置文件\n";
	s += "                       拼错时无需真起后端）。会**打开但不写入**审计日志文件。\n";
	s += "                       agent 侧的配置校验用 mybot-agent --check-config\n";
	s += "  --audit-log <路径>    审计轨迹（JSON Lines 追加写）：每次工具调用一条记录\n";
	s += "                       （时间戳/轮次/工具/参数/成败/耗时）；图片只记体积与指纹，\n";
	s += "                       命令原文里的 KEY=VALUE 敏感值先遮蔽；\"-\" 表示写 stderr；\n";
	s += "                       缺省不记录。也可用配置文件 audit_log 字段\n";
	s += "\n";
	s += "示例：\n";
	s += "  # 终端 1：工具后端（硬件上电常驻）\n";
	s += "  ./build/mybot --robot=robots/relay-test.sql --mcp-backend --mcp-socket /tmp/mybot.sock\n";
	s += "  # 终端 2：agent（REPL / --task；密钥经环境变量给）\n";
	s += "  ./agent/target/release/mybot-agent --socket /tmp/mybot.sock\n";
	s += "\n";
	s += "安全约束：动作脚本只能引入新的参数键，不得覆盖结构脚本的元数据列\n";
	s += "（fd / address / max / min / offset 等）与触发器引用名（trigger 列）；\n";
	s += "结构脚本与动作脚本单次输入上限均为 8 MiB。\n";
	s += "\n";
	s += "分段帮助（2026-09-22 新增）：\n";
	s += "  --help tcp     - 只显示 TCP 模式相关选项\n";
	s += "  --help agent   - 只显示 agent 体系（--mcp-backend + mybot-agent）相关选项\n";
	s += "  --help llm     - LLM/模型配置已迁 mybot-agent（指路）\n";
	s += "  --help camera  - 只显示摄像头相关选项\n";
	s += "  --help mcp     - 只显示 MCP 后端模式（--mcp-backend）相关选项\n";
	return s;
}

/* 分段帮助文本（2026-09-22 可用性改进）：支持快速查看特定主题 */
std::string helpText(const std::string& topic) {
	if (topic.empty()) { return helpText(); }
	std::string t = toLowerAscii(trim(topic));

	std::string s;
	if (t == "tcp") {
		s += "TCP 模式（动作输入源）：\n\n";
		s += tcpOptionsHelp();
		s += "\n示例：\n";
		s += "  ./build/mybot --robot=robots/spider1.sql --tcp 8080\n";
		s += "  ./build/mybot --robot=robots/spider1.sql --tcp 8080 --tcp-host 0.0.0.0 --tcp-token mysecret\n";
	} else if (t == "agent") {
		s += "agent 体系（模型循环已迁 Rust mybot-agent）：\n\n";
		s += "拓扑：mybot --mcp-backend 提供工具后端（硬件上电常驻，经 AF_UNIX\n";
		s += "socket 服务工具调用）；mybot-agent 跑模型循环（REPL/--task/预算/历史\n";
		s += "裁剪），直连 provider，经 --socket 调用后端。构建 agent：\n";
		s += "bash scripts/build_agent.sh；其选项见 mybot-agent --help 与 docs/agent.md。\n\n";
		s += "本程序的 agent 面选项（都在 --mcp-backend 下有效）：\n";
		s += "  --agent-shell         显式开启 run_command（任意 shell 执行）；后端下开它\n";
		s += "                       强制要 --agent-policy 且 require_confirm 含 run_command\n";
		s += "  --agent-hot-sql       显式开启 apply_sql（结构 SQL 带电生效）；后端下开它\n";
		s += "                       强制要策略确认（apply_sql）。配置文件不能开\n";
		s += "  --read-var <变量名>    声明可读硬件变量（可重复；注册 read_variables 与\n";
		s += "                       wait_variable 工具）。\n";
		s += "                       缺省不提供；配置文件不能给\n";
		s += "  --agent-policy <路径> 策略钩子：命中 require_confirm 的工具调用先过外部\n";
		s += "                       确认程序（退出 0 = 放行；超时按 on_timeout，缺省 deny）\n";
		s += "  --scripts-dir <目录>  动作脚本根目录（run_action_script 的沙箱边界）\n";
		s += "  --camera-* / --no-camera  摄像头（见 --help camera）\n";
		s += "  --audit-log <路径>    审计轨迹（JSON Lines；\"-\"=stderr；缺省不记录）\n";
		s += "  --check-config        只校验配置并打印摘要后退出 0（离线，不碰硬件）\n\n";
		s += "示例：\n";
		s += "  ./build/mybot --robot=robots/relay-test.sql --mcp-backend \\\n";
		s += "      --mcp-socket /tmp/mybot.sock --agent-shell --agent-policy policy.json\n";
		s += "  ./agent/target/release/mybot-agent --socket /tmp/mybot.sock\n";
	} else if (t == "llm") {
		s += "LLM/模型配置（已迁 Rust mybot-agent）：\n\n";
		s += "模型接入点、密钥、采样与循环参数（--llm-endpoint / --llm-model /\n";
		s += "--llm-config / --llm-timeout 等、--task、--prompt-file、--session-file、\n";
		s += "--resume）都是 mybot-agent 的选项——本程序会显式拒绝并指引。\n\n";
		s += "  mybot-agent --help\n";
		s += "  ./agent/target/release/mybot-agent --socket /tmp/mybot.sock --llm-model gpt-4\n\n";
		s += "环境变量 BOT4CJ_LLM_KEY（兼容 ROBOT_AGENT_API_KEY / DEEPSEEK_API_KEY）\n";
		s += "由 mybot-agent 进程读取（优先级：命令行 > 环境变量 > 配置文件 > 内置缺省）。\n";
		s += "详见 docs/agent.md。\n";
	} else if (t == "camera") {
		s += "摄像头配置选项（--mcp-backend 下有效）：\n\n";
		s += "  --camera-var <变量名> 承载摄像头驱动的变量名（缺省 cam0）\n";
		s += "  --camera-device <路径> 摄像头设备（缺省 /dev/video0；需支持 MJPEG）\n";
		s += "  --camera-src <图片>   模拟图片来源：不接摄像头也能跑通「拍照→进上下文」\n";
		s += "  --camera-src-dir <目录> 模拟图片白名单根目录（只能命令行给）\n";
		s += "  --no-camera           不向智能体提供 capture_image 工具\n";
		s += "  --check-config        只校验配置并打印摘要后退出 0（不装结构、不上电、不联网）\n";
		s += "  --audit-log <路径>    审计轨迹（JSON Lines 追加写；\"-\"=stderr；缺省不记录）\n";
		s += "                        也可用配置文件 audit_log 字段\n\n";
		s += "示例：\n";
		s += "  ./build/mybot --robot=robots/spider1.sql --mcp-backend --mcp-socket /tmp/s.sock \\\n";
		s += "      --camera-device /dev/video1\n";
	} else if (t == "mcp") {
		s += "MCP 后端模式（--mcp-backend）：\n\n";
		s += "  --mcp-backend       把 agent 工具层经本地 AF_UNIX socket 暴露给 Rust agent\n";
		s += "                      （mybot-agent，经 --socket 连接；它对上跑 REPL/--task，\n";
		s += "                      对下经帧协议调用这里的工具）。独立模式：不跑模型循环、\n";
		s += "                      不发起任何模型调用——每次工具调用都走既有的\n";
		s += "                      「策略确认 → 执行 → 审计」漏斗（DispatchGate::run）。\n";
		s += "                      与 --tcp / --input 互斥；需要 --robot 与 --mcp-socket。\n";
		s += "  --mcp-socket <路径>  AF_UNIX socket 路径（必填）。父目录建议 0700、socket 文件\n";
		s += "                      0600；对端经内核 SO_PEERCRED 校验 uid/gid（不可伪造）。\n";
		s += "  --mcp-call-budget <N>   每连接工具调用上限（跑飞护栏；0/空 = 不限，缺省不限）。\n";
		s += "                      后端不假设调用方是 agent 循环，故自行再强制一层预算。\n";
		s += "  --mcp-wall-clock <秒>   每连接墙钟预算（0/空 = 不限，缺省不限；上限 86400）。\n";
		s += "                      注：单次物理调用的 script/command 超时始终在工具层生效。\n";
		s += "  --mcp-idle-timeout <秒> 每连接空闲（无帧）超时（0/空 = 不限，缺省不限；上限 86400）。\n";
		s += "                      让「连上后一声不吭」的哑连接按时让出唯一的 accept 槽；\n";
		s += "                      墙钟预算同样在空闲期收口（到点断连，对端可重连）。\n";
		s += "  --on-disconnect <脚本>  连接结束（任何原因）后由后端自行执行的安全动作脚本\n";
		s += "                      （scripts-dir 之下的相对路径，如 zero.mt）。缺省不执行，\n";
		s += "                      硬件停在最后一个姿态。上电前校验；不经策略确认；失败\n";
		s += "                      只记日志/审计（kind=session, status=disconnect_action）。\n";
		s += "                      SIGINT/SIGTERM 停机不触发（走断电清理链）。\n\n";
		s += "安全：后端开 --agent-shell / --agent-hot-sql 时**强制**要 --agent-policy，且其\n";
		s += "      require_confirm 必须分别包含 run_command / apply_sql（无人值守暴露危险能力\n";
		s += "      必须有外部确认闸门）；纯只读/查询/动作脚本后端不受此约束。\n\n";
		s += "复用 agent 的工具上下文/能力门控选项（决定对外宣告哪些工具）：\n";
		s += "  --agent-shell / --agent-hot-sql / --read-var / --camera-* / --no-camera /\n";
		s += "  --scripts-dir / --agent-policy / --audit-log\n";
		s += "（--task / --llm-* / --prompt-file / --session-file / --resume 属于 mybot-agent）\n\n";
		s += "示例：\n";
		s += "  ./build/mybot --robot=robots/relay-test.sql --mcp-backend \\\n";
		s += "      --mcp-socket /run/user/1000/bot4cj/mcp.sock --audit-log -\n";
	} else {
		s += "未知主题: " + topic + "\n\n";
		s += "支持的主题：tcp, agent, llm, camera, mcp\n";
		s += "使用 --help 查看完整帮助\n";
	}
	return s;
}

void printHelp() {
	std::cout << helpText();
}

void printHelp(const std::string& topic) {
	std::cout << helpText(topic);
}

} // namespace bot
