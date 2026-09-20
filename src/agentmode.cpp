#include "agentmode.hpp"

#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "agentloop.hpp"
#include "llmconfig.hpp"
#include "promptpack.hpp"
#include "agenttools.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

/* 读取脚本头部 cap 字节（提示词预算，**截断是刻意的**）。
 * 但 I/O 错误不得与「正常截断」混为一谈：旧实现忽略 ferror，磁盘错误会把
 * 半截脚本当成完整摘录喂给模型（阶段 4.1 已把「整文件读取」统一为超限响亮
 * 拒绝；这里是同一族的静默面，因预算语义不同而保留截断，只把错误显式化）。 */
std::string readHead(const std::string& path, std::size_t cap) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { return ""; }
	std::string data;
	data.resize(cap);
	f.read(&data[0], static_cast<std::streamsize>(cap));
	std::streamsize n = f.gcount();
	if (n < 0) { n = 0; }
	data.resize(static_cast<std::size_t>(n));
	if (f.bad()) {
		std::cerr << "警告：读取结构脚本头部时发生 I/O 错误，提示词中的脚本摘录可能不完整: "
			<< path << std::endl;
	}
	return data;
}

/* 解析 CLI 里的整数字符串选项；越界/非数字给出带选项名的人话报错 */
bool parseIntOption(const std::string& raw, const char* key, int lo, int hi,
		int& out, std::string& err) {
	auto [state, value] = parseLongStrict(trim(raw));
	if (state != IntParse::OK) {
		err = std::string("选项 --") + key + " 需要一个整数，实际是 '" + raw + "'";
		return false;
	}
	if (value < lo || value > hi) {
		err = rangeErrorText(std::string("选项 --") + key, lo, hi, std::to_string(value));
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

/* 驱动表名/变量名的安全判据在 util::isSafeIdent 单点（与
 * RobotStruct::injectLuaDriver 的注入防线同一份）；在此提前拒绝，
 * 坏名字走不到结构脚本与硬件装配。 */

} // namespace

std::string resolveLlmKeyFromEnv() {
	for (const char* name : {"BOT4CJ_LLM_KEY", "ROBOT_AGENT_API_KEY", "DEEPSEEK_API_KEY"}) {
		auto v = getEnvVar(name);
		if (v && !v->empty()) { return *v; }
	}
	return "";
}

namespace {

/* 显式 "none" = 本次不读配置文件。
 * 为什么需要：缺省查找会看 CWD 里的 ./mybot.llm.json，于是「同一份配置换台机器就
 * 变了行为」——CI/守门尤其需要确定性（否则开发者个人配置会悄悄改变测试结果，
 * 实测踩过）。命令行与环境变量都支持该值。 */
bool isConfigDisabled(const std::string& s) {
	std::string t = trim(s);
	for (char& c : t) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return t == "none";
}

/* 选配置文件：命令行 > 环境变量 BOT4CJ_LLM_CONFIG > 缺省查找。找不到即返回空串
 * （配置文件是可选的）；显式给了路径却读不到由 loadLlmConfigFile 报错——不能
 * 静默回落到缺省，否则用户会以为配置生效了。 */
std::string pickConfigPath(const CliOptions& opts) {
	if (!opts.llmConfig.empty()) {
		return isConfigDisabled(opts.llmConfig) ? std::string() : opts.llmConfig;
	}
	if (auto env = getEnvVar("BOT4CJ_LLM_CONFIG"); env && !env->empty()) {
		return isConfigDisabled(*env) ? std::string() : *env;
	}
	return findDefaultLlmConfig();
}

} // namespace

AgentDriverConfig makeDriverConfig(const AgentModeConfig& cfg) {
	AgentDriverConfig d;
	d.model = cfg.model;
	d.endpoint = cfg.endpoint;
	d.maxTokens = cfg.maxTokens;
	d.temperature = cfg.temperature;
	d.topP = cfg.topP;
	d.timeoutSec = cfg.timeoutSec;
	d.reasoningEffort = cfg.reasoningEffort;
	d.thinking = cfg.thinking;
	return d;
}

namespace {

/* 分层选取（命令行 > 环境变量 > 配置文件 > 空）：envName=null 跳过环境层 */
std::string pickString(const std::string& cli, const char* envName,
		const std::optional<std::string>& fromFile) {
	if (!cli.empty()) { return cli; }
	if (envName != nullptr) {
		if (auto v = getEnvVar(envName); v && !v->empty()) { return *v; }
	}
	return fromFile.value_or("");
}

/* ---- 3) 协议驱动与变量名（阶段 3 拆分）---- */
bool pickDriverAndVar(const CliOptions& opts, const LlmFileConfig& file,
		AgentModeConfig& out, std::string& err) {
	out.driverTable = pickString(opts.llmDriver, nullptr, file.driver);
	if (out.driverTable.empty()) { out.driverTable = "LlmOpenAI"; }
	out.llmVar = pickString(opts.llmVar, nullptr, file.var);
	if (out.llmVar.empty()) { out.llmVar = "llm0"; }
	if (!isSafeIdent(out.driverTable)) {
		err = "协议驱动表名必须是安全标识符（字母/数字/下划线），实际是 '"
			+ out.driverTable + "'（来自 --llm-driver 或配置文件的 driver 字段）";
		return false;
	}
	if (!isSafeIdent(out.llmVar)) {
		err = "驱动变量名必须是安全标识符（字母/数字/下划线），实际是 '"
			+ out.llmVar + "'（来自 --llm-var 或配置文件的 var 字段）";
		return false;
	}
	return true;
}

/* ---- 4) API key：命令行 > 环境变量链 > 配置文件 api_key > 配置文件 api_key_file ---- */
bool resolveApiKey(const CliOptions& opts, const LlmFileConfig& file,
		const std::string& cfgPath, AgentModeConfig& out, std::string& err) {
	out.resolvedApiKey = resolveLlmKeyFromEnv();
	if (!out.resolvedApiKey.empty()) { out.apiKeySource = ApiKeySource::Env; }
	if (!opts.llmKey.empty()) {
		out.resolvedApiKey = opts.llmKey;
		out.apiKeySource = ApiKeySource::Cli;
	}
	if (out.resolvedApiKey.empty() && file.apiKey) {
		out.resolvedApiKey = *file.apiKey;
		out.apiKeySource = ApiKeySource::ConfigInline;
	}
	if (out.resolvedApiKey.empty() && file.apiKeyFile) {
		std::string keyErr;
		if (!readApiKeyFile(*file.apiKeyFile, out.resolvedApiKey, keyErr)) {
			err = "读取配置文件的 api_key_file 失败: " + keyErr;
			return false;
		}
		out.apiKeySource = ApiKeySource::ConfigFile;
		out.configWarning = secretFilePermissionWarning(*file.apiKeyFile);
	}
	if (out.resolvedApiKey.empty()) {
		err = "缺少 API key：可用 --llm-key、配置文件的 api_key / api_key_file 字段，"
			"或环境变量 BOT4CJ_LLM_KEY（也兼容 ROBOT_AGENT_API_KEY / DEEPSEEK_API_KEY）";
		return false;
	}
	/* 配置文件里内联密钥时也提醒权限（明文密钥常在随手 chmod 644 的文件里） */
	if (out.configWarning.empty() && !cfgPath.empty() && file.apiKey) {
		out.configWarning = secretFilePermissionWarning(cfgPath);
	}
	return true;
}

/* ---- 5) 采样/超时/轮次：命令行 > 配置文件 > 内建缺省 ---- */
bool mergeSamplingLayer(const CliOptions& opts, const LlmFileConfig& file,
		AgentModeConfig& out, std::string& err) {
	if (file.maxTokens) { out.maxTokens = *file.maxTokens; }
	if (file.temperature) { out.temperature = *file.temperature; }
	if (file.topP) { out.topP = *file.topP; }
	if (file.timeoutSec) { out.timeoutSec = *file.timeoutSec; }
	if (file.maxTurns) { out.maxTurns = *file.maxTurns; }

	if (!opts.llmMaxTurns.empty()
		&& !parseIntOption(opts.llmMaxTurns, "llm-max-turns", 1, 100, out.maxTurns, err)) {
		return false;
	}
	if (!opts.llmMaxTokens.empty()
		&& !parseIntOption(opts.llmMaxTokens, "llm-max-tokens", 1, 200000, out.maxTokens, err)) {
		return false;
	}
	if (!opts.llmTimeout.empty()) {
		int timeoutSec = 0;
		/* 上限 600 与 llm_http.clamp_timeout 的下限/上限一致（见该处注释） */
		if (!parseIntOption(opts.llmTimeout, "llm-timeout", 1, 600, timeoutSec, err)) {
			return false;
		}
		out.timeoutSec = timeoutSec;
	}
	return true;
}

/* ---- 6) 摄像头（capture_image） ---- */
bool pickCamera(const CliOptions& opts, const LlmFileConfig& file,
		AgentModeConfig& out, std::string& err) {
	out.allowCamera = opts.noCamera ? false : file.allowCamera.value_or(true);
	out.cameraVar = pickString(opts.cameraVar, nullptr, file.cameraVar);
	if (out.cameraVar.empty()) { out.cameraVar = "cam0"; }
	if (!isSafeIdent(out.cameraVar)) {
		err = "摄像头变量名必须是安全标识符（字母/数字/下划线），实际是 '"
			+ out.cameraVar + "'（来自 --camera-var 或配置文件的 camera_var 字段）";
		return false;
	}
	out.cameraDevice = pickString(opts.cameraDevice, nullptr, file.cameraDevice);
	out.cameraSrc = pickString(opts.cameraSrc, nullptr, file.cameraSrc);
	return true;
}

} // namespace

bool buildAgentConfig(const CliOptions& opts, AgentModeConfig& out, std::string& err) {
	/* ---- 1) 配置文件（最低优先级的一层，命令行/环境变量随后覆盖） ---- */
	LlmFileConfig file;
	std::string cfgPath = pickConfigPath(opts);
	if (!cfgPath.empty()) {
		if (!loadLlmConfigFile(cfgPath, file, err)) { return false; }
		out.configPath = cfgPath;
	}

	/* ---- 2) endpoint/model：命令行 > 环境变量 > 配置文件 > 驱动内置 DEFAULTS ---- */
	out.endpoint = pickString(opts.llmEndpoint, "BOT4CJ_LLM_ENDPOINT", file.endpoint);
	out.model = pickString(opts.llmModel, "BOT4CJ_LLM_MODEL", file.model);

	/* ---- 3) 协议驱动与变量名 ---- */
	if (!pickDriverAndVar(opts, file, out, err)) { return false; }

	/* ---- 4) API key ---- */
	if (!resolveApiKey(opts, file, cfgPath, out, err)) { return false; }

	/* ---- 5) 采样/超时/轮次 ---- */
	if (!mergeSamplingLayer(opts, file, out, err)) { return false; }

	/* ---- 6) 摄像头 ---- */
	if (!pickCamera(opts, file, out, err)) { return false; }

	/* ---- 7) agent 行为 ---- */
	/* 驱动特有字段：原样透传给驱动（如 DeepSeek 的 thinking / reasoning_effort） */
	out.reasoningEffort = file.reasoningEffort.value_or("");
	out.thinking = file.thinking.value_or("");

	out.scriptsDir = opts.scriptsDir.empty() ? "scripts" : opts.scriptsDir;
	if (opts.scriptsDir.empty() && file.scriptsDir) { out.scriptsDir = *file.scriptsDir; }
	out.allowShell = opts.agentNoShell ? false : file.allowShell.value_or(true);
	out.structurePath = opts.robot ? *opts.robot : "";
	out.task = opts.agentTask;

	/* ---- 8) 提示词包：命令行给过就整体覆盖配置文件列表（与 pickString 的
	 * 「命令行 > 配置文件」同口径）。在这里就加载成成品文本——提示词路径
	 * 写错属于配置错误，必须在结构装载与上电之前失败，而不是等系统提示词
	 * 拼装时才炸（main 的约定「配置错误不碰真机」）。 */
	out.promptFiles = !opts.promptFiles.empty() ? opts.promptFiles
						: file.promptFiles.value_or(std::vector<std::string>{});
		/* 不写 value_or({})：花括号实参要求模板参数 _Up 可推导，libstdc++ 15
		 * 给 _Up 加了默认值才兜得住，g++12/14 推导失败直接报错（CI 基线
		 * debian:bookworm 即 g++12）。 */
	if (!out.promptFiles.empty()
		&& !loadPromptPack(out.promptFiles, out.promptPack, err)) {
		return false;
	}
	return true;
}

std::string shellKeyExposureWarning(const AgentModeConfig& cfg) {
	if (!cfg.allowShell) { return ""; }
	const char* how = nullptr;
	switch (cfg.apiKeySource) {
	case ApiKeySource::Cli:
		how = "--llm-key 命令行参数（同用户进程可从 /proc/<pid>/cmdline 读到）";
		break;
	case ApiKeySource::ConfigInline:
		how = "配置文件的 api_key 字段（配置文件本身可读）";
		break;
	case ApiKeySource::ConfigFile:
		how = "配置文件 api_key_file 指向的密钥文件（该文件可读）";
		break;
	case ApiKeySource::Env:		/* 会被 run_command 的环境清洗剔除 */
	case ApiKeySource::None:
		return "";
	}
	return std::string("警告: run_command 已启用，而 API key 来自 ") + how
		+ "。run_command 的子进程与宿主同属一个用户，可直接读出该密钥"
		"（chmod 600 这类文件权限位对此无效）。"
		"如需避免：改用环境变量 BOT4CJ_LLM_KEY 提供密钥"
		"（该变量名含 key，会被 run_command 的环境清洗剔除），"
		"或用 --agent-no-shell 关闭 run_command。";
}

std::string buildAgentSystemPrompt(const AgentModeConfig& cfg, RobotStruct& robot) {
	std::string p;
	p += "你是机器人控制智能体（agent），通过工具控制一台真实机器人。\n";
	p += "机器人结构脚本：" + cfg.structurePath
		+ "。硬件已加载并上电常驻（动作脚本执行完不会断电），"
		"因此你可以在一次任务里连续执行多个动作脚本。\n";
	p += "模型接入由 Lua 协议驱动 `" + cfg.driverTable + "`（变量 `" + cfg.llmVar
		+ "`）提供；接入点、密钥与模型来自配置文件或命令行。\n\n";

	p += "## 机器人结构（接线图）\n";
	std::string sql = readHead(cfg.structurePath, 16u << 10);
	p += sql.empty() ? std::string("(结构脚本不可读)\n") : sql + "\n";

	p += "## 硬件变量\n";
	p += describeHardware(robot) + "\n";

	p += "## 可用动作脚本（目录 " + cfg.scriptsDir + "）\n";
	p += describeActionScripts(cfg.scriptsDir) + "\n";

	p += "## 工作方式\n";
	p += "- 需要机器人行动时用 run_action_script 执行动作脚本，脚本名必须来自上面的清单。\n";
	p += "- 不清楚有哪些脚本或硬件时，先用 list_action_scripts / list_hardware 查询。\n";
	p += "- 一次只执行一个动作脚本，看清返回结果后再决定下一步。\n";
	p += "- 脚本报错时先读清原因（脚本名错、硬件未接、参数越界），不要盲目重复同一条。\n";
	if (cfg.allowShell) {
		p += "- run_command 适合查看系统状态、日志与设备节点；驱动硬件请用 run_action_script。\n";
	}
	p += "- 用中文回答，结论简洁并说明依据。\n";

	/* 提示词包追加在内置段落之后（robot_agent 同款语义）：内置部分只讲机器人
	 * 本体，「这个机器人是谁、该怎么行事」由包定义；buildAgentConfig 已把
	 * 加载失败挡在上电之前，这里不可能拿到坏包。 */
	if (!cfg.promptPack.empty()) { p += "\n\n" + cfg.promptPack; }
	return p;
}

int runAgentMode(const AgentModeConfig& cfg, RobotStruct& robot, ChatTransport& transport,
		std::istream& in, std::ostream& out, std::ostream& log) {
	AgentToolContext tctx;
	tctx.robot = &robot;
	tctx.scriptsDir = cfg.scriptsDir;
	tctx.structurePath = cfg.structurePath;
	tctx.allowShell = cfg.allowShell;
	if (cfg.allowCamera) {
		tctx.camera = robot.registry().find(cfg.cameraVar);
		tctx.cameraDevice = cfg.cameraDevice;
		tctx.cameraSrc = cfg.cameraSrc;
		if (tctx.camera == nullptr && cfg.verbose) {
			log << "提示: 结构脚本里没有摄像头驱动变量 '" << cfg.cameraVar
				<< "'，本次不提供 capture_image 工具\n";
		}
	}

	AgentToolRegistry tools = makeAgentTools(tctx);

	AgentOptions aopts;
	aopts.maxTurns = cfg.maxTurns;
	aopts.verbose = cfg.verbose;
	aopts.log = &log;
	aopts.systemPrompt = buildAgentSystemPrompt(cfg, robot);

	AgentSession session(transport, tools, std::move(aopts));

	/* ---- 一次性任务：--task "..." ---- */
	if (!cfg.task.empty()) {
		try {
			out << session.runTurn(cfg.task) << "\n";
			return 0;
		} catch (const std::exception& e) {
			log << "错误: " << e.what() << "\n";
			return 1;
		}
	}

	/* ---- 交互 REPL：硬件全程常驻，可连续下达任务 ---- */
	out << "agent 模式：机器人已上电常驻（结构脚本 " << cfg.structurePath << "）。\n"
		<< "直接输入任务；/hardware 看硬件清单，/scripts 看动作脚本，/exit 退出。\n";
	std::string line;
	while (!stopRequested()) {
		out << "mybot> " << std::flush;
		if (!std::getline(in, line)) { break; }
		std::string task = trim(line);
		if (task.empty()) { continue; }
		if (task == "/exit" || task == "/quit") { break; }
		if (task == "/hardware") {
			out << describeHardware(robot) << "\n";
			continue;
		}
		if (task == "/scripts") {
			out << describeActionScripts(cfg.scriptsDir) << "\n";
			continue;
		}
		try {
			out << session.runTurn(task) << "\n";
		} catch (const std::exception& e) {
			/* REPL：单轮失败不结束会话（历史已回滚，可继续下一条任务） */
			log << "错误: " << e.what() << "\n";
		}
	}
	out << "\n";
	return 0;
}

} // namespace agent
} // namespace bot
