#include "agentmode.hpp"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "agentloop.hpp"
#include "llmconfig.hpp"
#include "agenttools.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

std::string readHead(const std::string& path, std::size_t cap) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { return ""; }
	std::string data;
	data.resize(cap);
	f.read(&data[0], static_cast<std::streamsize>(cap));
	std::streamsize n = f.gcount();
	if (n < 0) { n = 0; }
	data.resize(static_cast<std::size_t>(n));
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
		err = std::string("选项 --") + key + " 需要在 [" + std::to_string(lo) + ", "
			+ std::to_string(hi) + "] 范围内，实际是 " + std::to_string(value);
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

/* 驱动表名/变量名要拼进合成 SQL，且表名会被当成 modules/<表名>.lua 解析——
 * 与 RobotStruct::injectLlmDriver 同一判据，在此提前拒绝（不碰结构脚本与硬件）。 */
bool isSafeIdent(const std::string& s) {
	if (s.empty()) { return false; }
	unsigned char first = static_cast<unsigned char>(s[0]);
	if (!(std::isalpha(first) || s[0] == '_')) { return false; }
	for (char c : s) {
		unsigned char u = static_cast<unsigned char>(c);
		if (!(std::isalnum(u) || c == '_')) { return false; }
	}
	return true;
}

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

bool buildAgentConfig(const CliOptions& opts, AgentModeConfig& out, std::string& err) {
	/* ---- 1) 配置文件（最低优先级的一层，命令行/环境变量随后覆盖） ---- */
	LlmFileConfig file;
	std::string cfgPath = pickConfigPath(opts);
	if (!cfgPath.empty()) {
		if (!loadLlmConfigFile(cfgPath, file, err)) { return false; }
		out.configPath = cfgPath;
	}

	/* ---- 2) endpoint/model：命令行 > 环境变量 > 配置文件 > 驱动内置 DEFAULTS ---- */
	auto pickString = [](const std::string& cli, const char* envName,
			const std::optional<std::string>& fromFile) -> std::string {
		if (!cli.empty()) { return cli; }
		if (envName != nullptr) {
			if (auto v = getEnvVar(envName); v && !v->empty()) { return *v; }
		}
		return fromFile.value_or("");
	};
	out.endpoint = pickString(opts.llmEndpoint, "BOT4CJ_LLM_ENDPOINT", file.endpoint);
	out.model = pickString(opts.llmModel, "BOT4CJ_LLM_MODEL", file.model);

	/* ---- 3) 协议驱动与变量名 ---- */
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

	/* ---- 4) API key：命令行 > 环境变量链 > 配置文件 api_key > 配置文件 api_key_file ---- */
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

	/* ---- 5) 采样/超时/轮次：命令行 > 配置文件 > 内建缺省 ---- */
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

	/* ---- 6) 摄像头（capture_image） ---- */
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

	/* ---- 7) agent 行为 ---- */
	/* 驱动特有字段：原样透传给驱动（如 DeepSeek 的 thinking / reasoning_effort） */
	out.reasoningEffort = file.reasoningEffort.value_or("");
	out.thinking = file.thinking.value_or("");

	out.scriptsDir = opts.scriptsDir.empty() ? "scripts" : opts.scriptsDir;
	if (opts.scriptsDir.empty() && file.scriptsDir) { out.scriptsDir = *file.scriptsDir; }
	out.allowShell = opts.agentNoShell ? false : file.allowShell.value_or(true);
	out.structurePath = opts.robot ? *opts.robot : "";
	out.task = opts.agentTask;
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
