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
	{"input", [](CliOptions& o, const std::string& v) { o.input = v; }, false},
	{"camera-var", [](CliOptions& o, const std::string& v) { o.cameraVar = v; }, true},
	{"camera-device", [](CliOptions& o, const std::string& v) { o.cameraDevice = v; }, true},
	{"camera-src", [](CliOptions& o, const std::string& v) { o.cameraSrc = v; }, true},
	{"task", [](CliOptions& o, const std::string& v) { o.agentTask = v; }, true},
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
		if (key == "help") { out.opts.help = true; }
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
		else if (key == "no-camera") {
			out.opts.noCamera = true;
			out.opts.agentOptionsUsed = true;
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

	/* ---- agent 模式选项校验（先于结构脚本装载与任何硬件上电）----
	 * F-20 同族口径：选项组合错必须显式拒绝并退出 2，不能静默忽略。
	 *   1) agent 专属选项出现在非 agent 模式下 → 用户意图与参数不一致；
	 *   2) --agent 与 --tcp/--input 互斥 → 两者都要求主导输入源。 */
	if (!options.agent && options.agentOptionsUsed) {
		std::cerr << "参数错误：--task / --llm-* / --agent-shell / --scripts-dir "
			"/ --prompt-file 只在 --agent 模式下有效；用 --help 查看 agent 模式说明"
			<< std::endl;
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
	return 0;
}

std::string helpText() {
	std::string s;
	s += "这是一个机器人动作脚本解析器。\n";
	s += "使用--robot选项指定结构脚本，然后使用管道输入动作脚本，示例：\n";
	s += "cat scripts/std.mt | ./build/mybot --robot=./robots/spider1.sql\n";
	s += "\n";
	s += "动作输入源（优先级从高到低）：\n";
	s += "  --tcp <port>       TCP 模式：监听端口接受一次成功连接，读取对端下发的 .mt\n";
	s += "                     （EOF 或 --tcp-idle 超时按输入结束处理，非流式长驻）\n";
	s += "  --tcp-host <addr>  TCP 监听地址，缺省 127.0.0.1；跨机监听（如 0.0.0.0）\n";
	s += "                     必须同时提供 --tcp-token，否则拒绝启动\n";
	s += "  --tcp-token <tok>  TCP 口令：跨机监听必填；对端须先发一行 \"<tok>\\n\" 再发脚本；\n";
	s += "                     校验失败关连接回等，连续 5 次才退出；也可用环境变量 BOT4CJ_TCP_TOKEN\n";
	s += "  --tcp-idle <sec>   TCP 空闲超时，缺省 30s（超时按输入结束处理）；\n";
	s += "                     另有连接总时长预算（600s，随该值 x20 放大）防慢滴挂住进程\n";
	s += "  --input <path>     从文件读取动作脚本\n";
	s += "  （缺省）           从 stdin 管道读取\n";
	s += "\n";
	s += "agent 模式（与上面的动作输入源互斥）：机器人结构脚本加载后硬件常驻内存，\n";
	s += "模型调用由 Lua 协议驱动承担（不链接任何 HTTP 库，运行期只需 libcurl）。\n";
	s += "  --agent            开启 agent 模式（缺省交互 REPL；硬件全程上电常驻）\n";
	s += "  --task <文本>      一次性任务，跑完即退出；缺省则进入交互 REPL\n";
	s += "  --llm-config <路径>  配置文件（JSON）：目标 URL、API key、模型、采样与 agent 参数\n";
	s += "                       缺省按 ./mybot.llm.json -> ./mybot.llm.conf.json ->\n";
	s += "                       $HOME/.config/mybot/llm.json 查找；传 none 表示本次不读配置文件；\n";
	s += "                       也可用环境变量 BOT4CJ_LLM_CONFIG 指定\n";
	s += "  --llm-driver <表名>  协议驱动表名 = modules/<表名>.lua\n";
	s += "                       内置：LlmOpenAI（缺省，OpenAI 兼容）、LlmDeepSeek\n";
	s += "  --llm-var <变量名>   承载驱动的变量名（缺省 llm0）；结构脚本已声明则直接用\n";
	s += "  --llm-endpoint <URL>  接入点（覆盖配置文件）\n";
	s += "  --llm-model <名称>    模型名（覆盖配置文件）\n";
	s += "  （API key）           只从环境变量 BOT4CJ_LLM_KEY（兼容 ROBOT_AGENT_API_KEY /\n";
	s += "                       DEEPSEEK_API_KEY）或配置文件的 api_key / api_key_file 取；\n";
	s += "                       不提供命令行传密钥的选项——argv 对同用户进程可读\n";
	s += "                       （/proc/<pid>/cmdline）\n";
	s += "  --llm-max-turns <N>   单次任务的最大模型轮数（缺省 8，范围 1-100）\n";
	s += "  --llm-max-tokens <N>  单次生成最大 token 数（缺省 4096）\n";
	s += "  --llm-timeout <秒>    单次模型调用超时（缺省 180）\n";
	s += "  --scripts-dir <目录>  动作脚本根目录（缺省 scripts）；智能体只能执行其下的 .mt\n";
	s += "  --prompt-file <路径>  提示词包：可重复传入（文件或目录），全部内容拼接后\n";
	s += "                       追加到系统提示词，用于注入机器人身份与行为约束；\n";
	s += "                       目录收集其中全部 .md 按文件名自然排序（编号即装载顺序），\n";
	s += "                       文件间以 \"## 文件名\" 分节；也可用配置文件 prompt_files 数组\n";
	s += "                       （示例：prompts/spider_robot/）\n";
	s += "  --agent-shell         允许智能体执行 shell 命令（注册 run_command 工具）；\n";
	s += "                       **缺省不提供**该工具（任意 shell 执行是最大的一张牌，\n";
	s += "                       要用就显式开启；理由与缓解见 docs/agent.md 安全一节）\n";
	s += "  --camera-var <变量名> 承载摄像头驱动的变量名（缺省 cam0）；结构脚本已声明则直接用\n";
	s += "  --camera-device <路径> 摄像头设备（缺省 /dev/video0；需支持 MJPEG）\n";
	s += "  --camera-src <图片>   模拟图片来源：不接摄像头也能跑通「拍照→进上下文」\n";
	s += "  --no-camera           不向智能体提供 capture_image 工具\n";
	s += "取值优先级：命令行 > 环境变量 > 配置文件 > 驱动内置缺省。\n";
	s += "REPL 内置命令：/hardware 硬件清单，/scripts 动作脚本清单，/exit 退出。\n";
	s += "\n";
	s += "示例：\n";
	s += "  ./build/mybot --robot=robots/relay-test.sql --agent --task \"让继电器动作一次\"\n";
	s += "  ./build/mybot --robot=robots/spider1.sql --agent --llm-config mybot.llm.json\n";
	s += "\n";
	s += "安全约束：动作脚本只能引入新的参数键，不得覆盖结构脚本的元数据列\n";
	s += "（fd / address / max / min / offset 等）与触发器引用名（trigger 列）；\n";
	s += "结构脚本与动作脚本单次输入上限均为 8 MiB。\n";
	return s;
}

void printHelp() {
	std::cout << helpText();
}

} // namespace bot
