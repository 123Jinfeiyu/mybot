#include "agentmode.hpp"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "llmconfig.hpp"
#include "promptpack.hpp"
#include "agenttools.hpp"
#include "hardware.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace bot {
namespace agent {

namespace {

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
	return toLowerAscii(trim(s)) == "none";
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

/* PL-1（2026-09-22）：把**已经校验通过的**配置渲染成人类可读摘要。
 * 为什么是纯渲染而不是「再校验一遍」：校验是 buildAgentConfig 的单点职责，
 * 它失败时调用方已经 return 2。这里再造一套判据只会带来两处漂移——
 * 正是本项目反复强调要避免的。因此本函数只读 cfg、无副作用、可离线单测。
 * 密钥（resolvedApiKey）**绝不出现**在摘要里，只报来源与是否就绪。 */
std::string summarizeAgentConfig(const AgentModeConfig& cfg) {
	auto orNone = [](const std::string& s, const char* fallback) {
		return s.empty() ? std::string(fallback) : s;
	};
	/* 只报来源、不报值。映射与 validateApiKeyChars 的报错文案同口径。 */
	auto keySourceName = [](ApiKeySource src) -> const char* {
		switch (src) {
		case ApiKeySource::Env: return "环境变量";
		case ApiKeySource::ConfigInline: return "配置文件 api_key 字段";
		case ApiKeySource::ConfigFile: return "配置文件 api_key_file";
		case ApiKeySource::None: break;
		}
		return "未解析到";
	};

	std::string s;
	s += "配置校验通过：\n";
	if (!cfg.configPath.empty()) { s += "  配置文件    : " + cfg.configPath + "\n"; }
	else { s += "  配置文件    : (未使用)\n"; }
	s += "  协议驱动    : " + cfg.driverTable + "（变量 " + cfg.llmVar + "）\n";
	s += "  模型        : " + orNone(cfg.model, "(未配置，用驱动缺省)") + "\n";
	/* 接入点空 = 运行期必然失败（B2 起不再有内置缺省）。校验模式的价值一半在这里：
	 * 不联网也能把这句话摆到操作者面前。 */
	s += "  接入点      : " + orNone(cfg.endpoint, "(未配置，首次调用将失败)") + "\n";
	s += "  采样        : max_tokens=" + std::to_string(cfg.maxTokens)
		+ " temperature=" + std::to_string(cfg.temperature)
		+ " top_p=" + std::to_string(cfg.topP)
		+ " timeout=" + std::to_string(cfg.timeoutSec) + "s\n";
	s += "  轮次上限    : max_turns=" + std::to_string(cfg.maxTurns)
		+ " max_calls_per_round=" + std::to_string(cfg.maxCallsPerRound) + "\n";
	s += "  API key     : " + std::string(keySourceName(cfg.apiKeySource))
		+ "（值不显示）\n";
	s += "  结构脚本    : " + cfg.structurePath + "\n";
	s += "  动作脚本目录: " + cfg.scriptsDir + "\n";
	s += "  工具超时    : run_command=" + std::to_string(cfg.commandTimeoutSec)
		+ "s，上限=" + std::to_string(cfg.maxCommandTimeoutSec)
		+ "s，action_script=" + std::to_string(cfg.scriptTimeoutSec) + "s\n";
	s += "  提示词包    : " + std::to_string(cfg.promptFiles.size()) + " 项，"
		+ std::to_string(cfg.promptPack.size()) + " 字节"
		+ (cfg.structureHead ? "；结构脚本头 " + std::to_string(cfg.structureHead->size()) + " 字节" : "")
		+ (cfg.scriptsList ? "；脚本清单 " + std::to_string(cfg.scriptsList->size()) + " 字节" : "")
		+ "\n";
	s += "  历史/任务预算: 历史 " + std::to_string(cfg.maxHistoryBytes)
		+ " 字节，单任务文本 " + std::to_string(cfg.maxTaskToolResultBytes)
		+ " 字节，单任务图片 " + std::to_string(cfg.maxTaskImageBytes) + " 字节\n";
	s += "  摄像头      : " + std::string(cfg.allowCamera
			? (cfg.cameraSrc.empty() ? "启用（变量 " + cfg.cameraVar + "，设备 "
					+ orNone(cfg.cameraDevice, "/dev/video0（驱动缺省）") + "）"
				: "启用（模拟来源 " + cfg.cameraSrc + "）")
			: "不提供 capture_image 工具") + "\n";
	s += "  run_command : " + std::string(cfg.allowShell ? "已开启（--agent-shell）" : "未提供（缺省关）") + "\n";
	s += "  apply_sql   : " + std::string(cfg.allowHotSql ? "已开启（--agent-hot-sql）" : "未提供（缺省关）") + "\n";
	/* 回读：只报**声明了什么**（能不能解析要到上电后才知道，判据不在摘要这一层；
	 * 会话开始时的提示会点名被跳过的名字）。 */
	s += "  read/wait_variable: " + (cfg.readVars.empty()
			? std::string("未提供（缺省关；用 --read-var <变量名> 指定可读变量）")
			: "已声明 " + std::to_string(cfg.readVars.size()) + " 项") + "\n";
	s += "  审计轨迹    : " + orNone(cfg.auditLogPath, "(未开启)") + "\n";
	s += "  策略钩子    : " + orNone(cfg.policyPath, "(未启用)") + "\n";
	s += "  断连安全动作: " + orNone(cfg.onDisconnectScript, "(未配置；断连后硬件停在最后姿态)") + "\n";
	if (!cfg.configWarning.empty()) { s += "  配置告警    : " + cfg.configWarning + "\n"; }
	s += "  任务模式    : " + std::string(cfg.task.empty() ? "交互 REPL" : "一次性 --task") + "\n";
	return s;
}


/* 结构脚本展示名（去目录、去 .sql 后缀）：REPL 提示符、会话文件的 robot 指纹
 * 字段与 MCP 后端 context.get 的 robotName 共用同一份（批次 1）——各写一遍
 * 迟早分叉。声明在 agentmode.hpp（mcpbackend.cpp 也用）。 */
std::string structureDisplayName(const std::string& path) {
	std::string name = path;
	if (std::size_t slash = name.rfind('/'); slash != std::string::npos) {
		name = name.substr(slash + 1);
	}
	if (name.size() > 4 && name.substr(name.size() - 4) == ".sql") {
		name = name.substr(0, name.size() - 4);
	}
	return name;
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

/* ---- 4) API key：环境变量链 > 配置文件 api_key > 配置文件 api_key_file ----
 * 2026-09-20 评估：--llm-key 已移除（命令行密钥会出现在 /proc/<pid>/cmdline，
 * 同用户进程可直接读到，且与「密钥不进 argv」的自证口径自相矛盾）。 */
bool resolveApiKey(const LlmFileConfig& file,
		const std::string& cfgPath, AgentModeConfig& out, std::string& err) {
	out.resolvedApiKey = resolveLlmKeyFromEnv();
	if (!out.resolvedApiKey.empty()) { out.apiKeySource = ApiKeySource::Env; }
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
		err = "缺少 API key：请设环境变量 BOT4CJ_LLM_KEY"
			"（也兼容 ROBOT_AGENT_API_KEY / DEEPSEEK_API_KEY），"
			"或在配置文件的 api_key / api_key_file 字段提供"
			"（命令行不提供密钥选项：argv 对同用户进程可读）";
		return false;
	}
	/* 密钥含控制字符 → 拒绝（2026-09-21 审计 AG-14，已用真实二进制复现）。
	 * 密钥会被驱动拼进 `Authorization: Bearer <key>` 交给 curl_slist_append，
	 * 而 libcurl 对 slist 里的 CR/LF **不做拒绝**：实测一个多行密钥文件会把
	 * 换行后的内容变成**额外的请求头**发给网关。三条来源都要过这一关——
	 * api_key_file 只做首尾 trim（内嵌换行不在其覆盖范围），环境变量路径
	 * 则完全不 trim，所以校验放在三条来源汇合之后的这里，而不是某一来源内部。
	 * 这既是安全收口（请求头注入），也是可用性收口：「多行密钥文件」是极常见
	 * 的误配置，在这里点名比在网关那里看一个含混的 401/400 好得多。 */
	if (std::string keyErr; !validateApiKeyChars(out.resolvedApiKey, keyErr)) {
		err = "API key 非法（来源: "
			+ std::string(out.apiKeySource == ApiKeySource::Env ? "环境变量"
				: out.apiKeySource == ApiKeySource::ConfigInline ? "配置文件 api_key 字段"
										: "配置文件 api_key_file")
			+ "）: " + keyErr;
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
	if (file.maxTaskSeconds) { out.maxTaskSeconds = *file.maxTaskSeconds; }

	/* 运行期数值（B4）：只有配置文件这一层（不加 CLI 选项，理由见 llm_limits）。
	 * 0 是合法值（max_retries / retry_backoff_ms），故用 has_value 语义判空。 */
	if (file.maxCallsPerRound) { out.maxCallsPerRound = *file.maxCallsPerRound; }
	if (file.maxRetries) { out.maxRetries = *file.maxRetries; }
	if (file.retryBackoffMs) { out.retryBackoffMs = *file.retryBackoffMs; }

	if (!opts.llmMaxTurns.empty()
		&& !parseIntOption(opts.llmMaxTurns, "llm-max-turns",
			llm_limits::kMaxTurnsMin, llm_limits::kMaxTurnsMax, out.maxTurns, err)) {
		return false;
	}
	if (!opts.llmMaxTokens.empty()
		&& !parseIntOption(opts.llmMaxTokens, "llm-max-tokens",
			llm_limits::kMaxTokensMin, llm_limits::kMaxTokensMax, out.maxTokens, err)) {
		return false;
	}
	if (!opts.llmTimeout.empty()) {
		int timeoutSec = 0;
		/* 上限与 llm_http.clamp_timeout 的钳制区间同源（llm_limits 单点） */
		if (!parseIntOption(opts.llmTimeout, "llm-timeout",
				llm_limits::kTimeoutMinSec, llm_limits::kTimeoutMaxSec, timeoutSec, err)) {
			return false;
		}
		out.timeoutSec = timeoutSec;
	}
	if (!opts.llmTaskTimeout.empty()) {
		int taskSec = 0;
		/* 与 --llm-timeout 同一口径：区间取自 llm_limits 单点，0 是合法的
		 * "不限"（A4 的逃生口），故下界是 kTaskTimeoutMinSec 而不是 1。 */
		if (!parseIntOption(opts.llmTaskTimeout, "llm-task-timeout",
				llm_limits::kTaskTimeoutMinSec, llm_limits::kTaskTimeoutMaxSec,
				taskSec, err)) {
			return false;
		}
		out.maxTaskSeconds = taskSec;
	}
	return true;
}

bool mergeToolBudgets(const LlmFileConfig& file, AgentModeConfig& out, std::string& err) {
	if (file.runCommandTimeoutSec) { out.commandTimeoutSec = *file.runCommandTimeoutSec; }
	if (file.runCommandMaxTimeoutSec) {
		out.maxCommandTimeoutSec = *file.runCommandMaxTimeoutSec;
	}
	if (file.actionScriptTimeoutSec) { out.scriptTimeoutSec = *file.actionScriptTimeoutSec; }
	// 只配置上限时，也必须与合并后的缺省超时比较。
	if (out.maxCommandTimeoutSec < out.commandTimeoutSec) {
		err = "run_command 超时上限低于缺省超时（run_command_max_timeout_sec="
			+ std::to_string(out.maxCommandTimeoutSec) + "s < run_command_timeout_sec="
			+ std::to_string(out.commandTimeoutSec) + "s）：请把上限调到不小于缺省，"
			"或一并调小缺省超时";
		return false;
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
	/* H1：来源决定「能不能用 src 自己推导白名单根」（判据见 agenttools.hpp 的
	 * AgentToolContext）。pickString 里命令行优先，故命令行非空即等于采用了它。 */
	out.cameraSrcFromCli = !opts.cameraSrc.empty();
	/* H1：白名单根目录**只从命令行**取；配置文件写 camera_src_dir 由
	 * buildAgentConfig 拒绝加载（与 allow_shell 同判据，见那里）。 */
	out.cameraSrcDir = opts.cameraSrcDir;
	return true;
}

/* ---- 各编号步骤的具名实现（拆分自 buildAgentConfig：原函数 242 行把 12 个
 * 编号步骤全部内联，读哪个判据都要先定位到步骤号再往下翻；拆出后每个函数
 * 一个职责，签名/判据/文案不变——编排顺序仍是下面的 buildAgentConfig）。 */

/* 0) 会话恢复的命令行组合校验（批次 1，2026-09-23）：只依赖 opts 本身，放在
 * 最前（配置文件查找之前）：不通过就立刻失败——配置错误不碰真机，也不碰
 * 文件系统里任何别的东西。恢复是**输入通道**（磁盘内容读进模型上下文），
 * 故路径必须来自命令行：配置文件来源的 session_file 只用于写入（判据与
 * H2 给提示词路径加根约束同源——「这个配置文件存在」不构成授权一次读取）。 */
bool checkResumeCombo(const CliOptions& opts, AgentModeConfig& out, std::string& err) {
	out.sessionFileFromCli = !opts.sessionFile.empty();
	if (opts.resume && !out.sessionFileFromCli) {
		err = "--resume 需要与 --session-file <路径> 联用，且路径必须来自命令行："
			"恢复会把会话文件的内容读进模型上下文（输入通道），配置文件里的 "
			"session_file 只用于写入，不能授权一次读取";
		return false;
	}
	if (opts.resume) {
		std::error_code ec;
		if (!fs::exists(opts.sessionFile, ec) || ec) {
			err = "--session-file '" + opts.sessionFile + "' 不存在，无会话可恢复"
				"（恢复只作用于已存在的会话文件；开新会话请去掉 --resume）";
			return false;
		}
	}
	return true;
}

/* 2-5) LLM 层（endpoint/model、协议驱动、API key、采样/超时/轮次）。
 * MCP 后端（--mcp-backend）**不发起任何模型调用**：它只把工具层经 socket
 * 暴露给外部 MCP 客户端，模型在客户端那侧。故这几步整体跳过——尤其第 4 步
 * 「缺少 API key 即失败」不该拦住一个根本不调模型的后端。 */
bool mergeLlmLayer(const CliOptions& opts, const LlmFileConfig& file,
		const std::string& cfgPath, AgentModeConfig& out, std::string& err) {
	(void)cfgPath;
	if (opts.mcpBackend) { return true; }
	/* 2) endpoint/model：命令行 > 环境变量 > 配置文件 > 驱动内置 DEFAULTS */
	out.endpoint = pickString(opts.llmEndpoint, "BOT4CJ_LLM_ENDPOINT", file.endpoint);
	out.model = pickString(opts.llmModel, "BOT4CJ_LLM_MODEL", file.model);
	/* 3) 协议驱动与变量名 */
	if (!pickDriverAndVar(opts, file, out, err)) { return false; }
	/* 4) API key */
	if (!resolveApiKey(file, cfgPath, out, err)) { return false; }
	/* 5) 采样/超时/轮次 */
	if (!mergeSamplingLayer(opts, file, out, err)) { return false; }
	return true;
}

/* 「配置文件只能收紧、不能放宽」的统一报错口径：放宽能力的一侧（哪个字段、
 * 为什么危险、该敲哪个命令行开关）一次说全，四个字段共用同一形态。 */
std::string relaxingFieldError(const std::string& cfgPath, const std::string& field,
		const std::string& why, const std::string& cliSwitch) {
	return "配置文件 " + (cfgPath.empty() ? std::string("(缺省查找)") : cfgPath)
		+ " 设置了 " + field + "：" + why
		+ "，只能由命令行 " + cliSwitch + " 提供，配置文件不能给"
		+ "（请删除该字段或置回缺省；缺省查找会读 CWD 的 ./mybot.llm.json，"
		  "「这个文件存在」不构成授权）";
}

/* 6/7/7b) 摄像头与能力门控。H1：camera_src_dir 决定 capture_image 的模拟图片
 * 白名单根，等价于**放宽读权限**，故与 allow_shell 同判据——只能命令行给。
 * 缺省查找会读 CWD 的 ./mybot.llm.json，于是一个未经审查的落地文件就能同时
 * 改 endpoint（外发）+ camera_src（读哪个文件）+ 白名单根（能不能读）+
 * 注册 run_command（模型回复直接变成主机 shell），把「任意文件 → base64 →
 * 模型上下文 → 攻击者网关」串成一条链；而 allow_camera 缺省为真、cam0 自动
 * 注入，这条链**不需要 --agent-shell**——所以四个字段一律「配置文件只能关」。 */
bool applyGatingLayer(const CliOptions& opts, const LlmFileConfig& file,
		const std::string& cfgPath, AgentModeConfig& out, std::string& err) {
	/* 6) 摄像头（变量名/设备/来源的常规三层合并已在 pickCamera 内完成） */
	if (!pickCamera(opts, file, out, err)) { return false; }
	if (file.cameraSrcDir) {
		err = relaxingFieldError(cfgPath, "camera_src_dir",
			"它决定 capture_image 能读哪个目录（放宽读权限）", "--camera-src-dir");
		return false;
	}

	/* 7) agent 行为：驱动特有字段原样透传（如 DeepSeek 的 thinking /
	 * reasoning_effort）；scripts-dir 命令行优先；两个危险开关缺省**不提供**
	 * 且只能命令行开（安全缺省：评估 C12 翻转 + 审计 4.1 收紧；
	 * 热插表 2026-09-23 与 run_command 同判据——apply_sql 会长出新硬件并
	 * **立即上电**，「模型回复直接改变带电的硬件结构」）。 */
	out.reasoningEffort = file.reasoningEffort.value_or("");
	out.thinking = file.thinking.value_or("");

	out.scriptsDir = opts.scriptsDir.empty() ? "scripts" : opts.scriptsDir;
	if (opts.scriptsDir.empty() && file.scriptsDir) { out.scriptsDir = *file.scriptsDir; }
	out.allowShell = opts.agentShell;
	if (!opts.agentShell && file.allowShell.value_or(false)) {
		err = relaxingFieldError(cfgPath, "allow_shell",
			"run_command 等于把主机 shell 交给模型", "--agent-shell");
		return false;
	}
	out.allowHotSql = opts.agentHotSql;
	if (!opts.agentHotSql && file.allowHotSql.value_or(false)) {
		err = relaxingFieldError(cfgPath, "allow_hot_sql",
			"apply_sql 会按模型给的 SQL 立即装配并给新硬件上电（开 fd、进断电清理链）",
			"--agent-hot-sql");
		return false;
	}
	out.structurePath = opts.robot ? *opts.robot : "";
	out.task = opts.agentTask;

	/* 7b) 回读清单（发现 2 的 A 步，2026-09-23）：「能读哪些变量」是由操作者
	 * 授予的能力（执行器与传感器靠种类分不开，Servo/Relay 也是 lua 驱动，
	 * 清单必须由人列），同上四个字段：只能命令行给。 */
	out.readVars = opts.readVars;
	if (file.readVars) {
		err = relaxingFieldError(cfgPath, "read_vars",
			"它决定模型能主动读哪些硬件变量（等同于给模型一条读通道）", "--read-var");
		return false;
	}
	return true;
}

/* 8a) 上下文字节预算（B1）：配置文件覆盖编译期缺省；0 = 不限。
 * 严格解析（类型/区间/未知字段）已在 llmconfig 完成，这里只做搬运。 */
void applyContextBudgets(const LlmFileConfig& file, AgentModeConfig& out) {
	if (file.maxHistoryBytes) { out.maxHistoryBytes = *file.maxHistoryBytes; }
	if (file.maxTaskToolResultBytes) { out.maxTaskToolResultBytes = *file.maxTaskToolResultBytes; }
	if (file.maxTaskImageBytes) { out.maxTaskImageBytes = *file.maxTaskImageBytes; }
}

/* 8) 提示词包：命令行给过就整体覆盖配置文件列表（与 pickString 的
 * 「命令行 > 配置文件」同口径）。在这里就加载成成品文本——提示词路径
 * 写错属于配置错误，必须在结构装载与上电之前失败，而不是等系统提示词
 * 拼装时才炸（main 的约定「配置错误不碰真机」）。
 * H2（2026-09-23）：**配置文件来源**的提示词路径必须落在允许的根之内
 * （当前工作目录 ∪ BOT4CJ_PROMPT_DIR 列的目录）。命令行 --prompt-file 是
 * 操作者本人的授意，不受这条约束——与 H1 的 camera_src 同一判据：
 * 配置文件不能成为一个「读任意文件」的原语（判据与理由单点在
 * promptpack.hpp 的 promptPathsWithinRoots 注释）。 */
bool loadPromptLayer(const CliOptions& opts, const LlmFileConfig& file,
		AgentModeConfig& out, std::string& err) {
	/* MCP 后端不构建系统提示词，跳过提示词包装载（下面的 H2 根约束与
	 * loadPromptPack 因 out.promptFiles 保持空而自然 no-op）。 */
	if (!opts.mcpBackend) {
		out.promptFiles = !opts.promptFiles.empty() ? opts.promptFiles
							: file.promptFiles.value_or(std::vector<std::string>{});
	}
		/* 不写 value_or({})：花括号实参要求模板参数 _Up 可推导，libstdc++ 15
		 * 给 _Up 加了默认值才兜得住，g++12/14 推导失败直接报错（CI 基线
		 * debian:bookworm 即 g++12）。 */
	if (opts.promptFiles.empty() && !out.promptFiles.empty()) {
		std::vector<std::string> roots = promptRootsFromEnv();
		std::error_code ec;
		if (const fs::path cwd = fs::current_path(ec); !ec) { roots.push_back(cwd.string()); }
		if (!promptPathsWithinRoots(out.promptFiles, roots, err)) { return false; }
	}
	if (!out.promptFiles.empty()
		&& !loadPromptPack(out.promptFiles, out.promptPack, err)) {
		return false;
	}
	return true;
}

/* 9) 提示词体积预检：**不依赖硬件**的三段（2026-09-21 审计 AG-8）。
 * 硬件变量清单那一段要到上电后才算得出来（变量是 active() 时装配的），
 * 但另外三段（结构脚本头、动作脚本清单、提示词包）在配置组装期就都是已知量：
 *   ① 三段中任何一段自己就已顶穿上限时，**上电之前**就失败；
 *   ② 顺带在配置期读一遍结构脚本头与脚本清单——它们进上下文前要 UTF-8
 *      消毒（AG-1），早读早暴露；
 *   ③ 报错口径与 Rust 侧 build_system_prompt 的总量检查一致（同一常量、
 *      同一文案），不会出现「配置期放过、上电后拒绝」的分叉。
 * 读一次存两份（C6）：这两段成品供 MCP 后端的 context.get 复用（单一读法）。
 * MCP 后端不拼系统提示词，整段跳过（structureHead/scriptsList 缓存也不用）。 */
bool precheckPromptSize(const CliOptions& opts, AgentModeConfig& out, std::string& err) {
	if (opts.mcpBackend) { return true; }
	out.structureHead = structureScriptHead(out.structurePath, kStructureHeadBytes);
	out.scriptsList = describeActionScripts(out.scriptsDir);
	const std::size_t hardwareIndependent = out.structureHead->size()
		+ out.scriptsList->size()
		+ out.promptPack.size();
	if (hardwareIndependent > kMaxSystemPromptBytes) {
		err = "系统提示词（不依赖硬件的三段：结构脚本头 / 动作脚本清单 / 提示词包）"
			"已超过 " + std::to_string(kMaxSystemPromptBytes) + " 字节上限（实测 "
			+ std::to_string(hardwareIndependent)
			+ " 字节）；无需上电即可判定，请先缩减 --prompt-file 或 --scripts-dir "
			  "下的脚本数量";
		return false;
	}
	return true;
}

/* 10) 审计轨迹：在这里就打开——路径打不开属于配置错误，必须在结构装载与
 * 上电之前失败（「配置错误不碰真机」），而不是等到第一次工具调用才炸。 */
bool openAuditLog(const CliOptions& opts, const LlmFileConfig& file,
		AgentModeConfig& out, std::string& err) {
	out.auditLogPath = pickString(opts.auditLog, nullptr, file.auditLog);
	if (!out.auditLogPath.empty()) {
		out.audit = std::make_shared<AuditLog>();
		if (std::string auditErr; !out.audit->open(out.auditLogPath, auditErr)) {
			err = auditErr;
			return false;
		}
	}
	return true;
}

/* 11) 策略钩子：在这里就加载——策略文件写错属于配置错误，必须在上电之前
 * 失败（同一口径）。注意它**不是**安全边界：确认程序与宿主同权限，真正的
 * 隔离仍是专用用户 / 容器（docs/agent.md 安全一节的既有口径不变）。 */
bool loadPolicyHook(const CliOptions& opts, const LlmFileConfig& file,
		AgentModeConfig& out, std::string& err) {
	out.policyPath = pickString(opts.agentPolicy, nullptr, file.agentPolicy);
	if (!out.policyPath.empty()) {
		if (std::string policyErr; !(out.policy = AgentPolicy::load(out.policyPath, policyErr))) {
			err = policyErr;
			return false;
		}
	}
	return true;
}

/* 11b) MCP 后端 + 危险能力 ⇒ 强制外部确认（评估 §3.2，用户定「折中」）。
 * --mcp-backend 把工具暴露给**任意外部 MCP 客户端**、且硬件全程带电常驻，
 * 信任面比坐在终端前的 agent REPL 大得多。故当后端开了危险能力时，**强制**
 * 要有策略钩子、且该钩子确实对相应工具要求外部确认（require_confirm，超时
 * fail-closed）：
 *   --agent-shell   （run_command）→ 策略必须 require_confirm "run_command"
 *   --agent-hot-sql （apply_sql）  → 策略必须 require_confirm "apply_sql"
 * 折中：纯只读 / 查询 / 动作脚本后端（未开上述危险能力）不受此约束。
 * 注意 agentpolicy 仍**不是**隔离边界（同 uid 进程可绕过，见 docs/agent.md
 * 与 mcpbackend.hpp 的残留风险说明）——它是「每次调用先过人/审批」的确认闸门。 */
bool enforceBackendConfirmPolicy(const CliOptions& opts, const AgentModeConfig& out,
		std::string& err) {
	if (!opts.mcpBackend) { return true; }
	if (out.allowShell
		&& (out.policy == nullptr || !out.policy->requiresConfirm("run_command"))) {
		err = "--mcp-backend 开了 --agent-shell（run_command）：后端对外暴露且硬件带电，"
			"必须用 --agent-policy 提供策略钩子，且其 require_confirm 必须包含 \"run_command\""
			"（无人值守暴露任意 shell 必须有外部确认闸门）";
		return false;
	}
	if (out.allowHotSql
		&& (out.policy == nullptr || !out.policy->requiresConfirm("apply_sql"))) {
		err = "--mcp-backend 开了 --agent-hot-sql（apply_sql）：后端对外暴露且硬件带电，"
			"必须用 --agent-policy 提供策略钩子，且其 require_confirm 必须包含 \"apply_sql\""
			"（热插表会立即给新硬件上电，必须有外部确认闸门）";
		return false;
	}
	return true;
}

/* 11c) 断连安全动作（--on-disconnect）：上电前预检脚本能装载——路径越界 /
 * 不存在 / 为空都是配置错误，必须在这里失败。否则要等到**第一次断连**、也就是
 * 最需要它的那一刻才发现脚本名拼错，硬件照旧停在最后一个姿态。判据与
 * run_action_script 同源（loadActionScriptFile），不另写一份。 */
bool precheckDisconnectScript(const CliOptions& opts, AgentModeConfig& out, std::string& err) {
	out.onDisconnectScript = opts.onDisconnect;
	if (out.onDisconnectScript.empty()) { return true; }
	try {
		(void)loadActionScriptFile(out.scriptsDir, out.onDisconnectScript);
	} catch (const std::exception& e) {
		err = "--on-disconnect 脚本不可用 [" + out.onDisconnectScript + "]: " + e.what()
			+ "（须是 --scripts-dir 之下的相对路径，且非空）";
		return false;
	}
	return true;
}

} // namespace

bool buildAgentConfig(const CliOptions& opts, AgentModeConfig& out, std::string& err) {
	/* 编排：12 个编号步骤按序落位，每步的判据/文案在上方对应的具名函数里。
	 * 任何一步失败都发生在结构装载与上电之前（「配置错误不碰真机」）。 */
	if (!checkResumeCombo(opts, out, err)) { return false; }		/* 0) 会话恢复组合 */

	/* 1) 配置文件（最低优先级的一层，命令行/环境变量随后覆盖） */
	LlmFileConfig file;
	std::string cfgPath = pickConfigPath(opts);
	if (!cfgPath.empty()) {
		if (!loadLlmConfigFile(cfgPath, file, err)) { return false; }
		out.configPath = cfgPath;
	}

	if (!mergeLlmLayer(opts, file, cfgPath, out, err)) { return false; }	/* 2-5) LLM 层 */
	if (!mergeToolBudgets(file, out, err)) { return false; }
	if (!applyGatingLayer(opts, file, cfgPath, out, err)) { return false; }	/* 6/7/7b) 门控 */
	applyContextBudgets(file, out);						/* 8a) 上下文预算 */
	if (!loadPromptLayer(opts, file, out, err)) { return false; }		/* 8) 提示词包 */
	if (!precheckPromptSize(opts, out, err)) { return false; }		/* 9) 体积预检 */
	if (!openAuditLog(opts, file, out, err)) { return false; }		/* 10) 审计 */
	if (!loadPolicyHook(opts, file, out, err)) { return false; }		/* 11) 策略钩子 */
	if (!enforceBackendConfirmPolicy(opts, out, err)) { return false; }	/* 11b) 后端强制确认 */
	if (!precheckDisconnectScript(opts, out, err)) { return false; }	/* 11c) 断连安全动作 */

	/* 12) 会话文件（--session-file / 配置文件 session_file；批次 1）：
	 * 写入是纯输出，判据同 audit_log：可进配置文件。恢复的组合校验已在
	 * 第 0 步完成（--resume 只认命令行路径 + 文件必须存在）。打开与指纹
	 * 核对在会话启动处：指纹要到系统提示词定稿（上电后）才算得出。 */
	out.sessionFile = out.sessionFileFromCli ? opts.sessionFile
		: (file.sessionFile ? *file.sessionFile : "");
	out.resumeSession = opts.resume;
	return true;
}

std::string shellKeyExposureWarning(const AgentModeConfig& cfg) {
	if (!cfg.allowShell) { return ""; }
	const char* how = nullptr;
	switch (cfg.apiKeySource) {
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
	return std::string("警告: run_command 已启用（--agent-shell；自 f06b712 起配置文件"
		"的 allow_shell 只能关不能开），而 API key 来自 ") + how
		+ "。run_command 的子进程与宿主同属一个用户，可直接读出该密钥"
		"（chmod 600 这类文件权限位对此无效）。"
		"如需避免：改用环境变量 BOT4CJ_LLM_KEY 提供密钥"
		"（该变量名含 key，会被 run_command 的环境清洗剔除），"
		"或去掉 --agent-shell（缺省即关闭）。";
}

/* 组装工具上下文（AgentToolContext）：agent REPL 与 MCP 后端（--mcp-backend）
 * 共用这一个函数——「对外提供哪些工具、能读哪些变量、摄像头怎么解析」的判据
 * 只在这里发生一次，两条路径不会漂移（后端暴露的工具集与 agent 模式逐字节一致）。
 * 只读配置 + robot，不发起模型调用、不上电（调用方负责 active() 在先）。 */
AgentToolContext makeToolContext(const AgentModeConfig& cfg, RobotStruct& robot,
		std::ostream& log) {
	AgentToolContext tctx;
	tctx.robot = &robot;
	tctx.scriptsDir = cfg.scriptsDir;
	tctx.structurePath = cfg.structurePath;
	tctx.allowShell = cfg.allowShell;
	tctx.allowHotSql = cfg.allowHotSql;
	tctx.commandTimeoutSec = cfg.commandTimeoutSec;
	tctx.maxCommandTimeoutSec = cfg.maxCommandTimeoutSec;
	tctx.scriptTimeoutSec = cfg.scriptTimeoutSec;	/* PL-2：动作脚本执行预算 */
	tctx.readVars = cfg.readVars;			/* 发现 2 的 A 步：声明原文（注册时解析） */
	/* 声明的可读变量里被跳过的那些（未注册 / 种类不是 lua·daemon）如实告知：
	 * 静默丢能力正是本项目反复要避免的形态——`--read-var` 敲错一个字，
	 * 操作者必须能从启动输出里看出来，而不是等到模型说"我读不到"。 */
	if (!cfg.readVars.empty()) {
		std::vector<std::string> skipped;
		const std::vector<std::string> resolved = resolvableReadVars(cfg.readVars, robot, &skipped);
		if (resolved.empty()) {
			log << "提示: --read-var 声明的变量一个都不可读（未注册，或种类不是 lua/daemon），"
				"本次不提供 read_variables / wait_variable 工具\n";
		}
		if (!skipped.empty()) {
			log << "提示: 以下 --read-var 声明被跳过（未注册，或种类不是 lua/daemon）: ";
			for (std::size_t i = 0; i < skipped.size(); ++i) {
				log << (i > 0 ? ", " : "") << skipped[i];
			}
			log << "\n";
		}
	}
	if (cfg.allowCamera) {
		tctx.camera = robot.registry().find(cfg.cameraVar);
		tctx.cameraDevice = cfg.cameraDevice;
		tctx.cameraSrc = cfg.cameraSrc;
		/* H1：白名单的两个输入分别传递——来源标记决定"能不能用 src 推导根"，
		 * cameraSrcRoot 是操作者显式给的根（只能来自命令行，见 buildAgentConfig）。 */
		tctx.cameraSrcFromCli = cfg.cameraSrcFromCli;
		tctx.cameraSrcRoot = cfg.cameraSrcDir;
		if (tctx.camera == nullptr) {
			if (cfg.verbose) {
				log << "提示: 结构脚本里没有摄像头驱动变量 '" << cfg.cameraVar
					<< "'，本次不提供 capture_image 工具\n";
			}
		} else if (std::string_view(tctx.camera->kindName()) != "lua") {
			/* 必须是 Lua 驱动：playQuiet 对别的种类就退化成 play()（hardware.hpp），
			 * 那样模型调一次「拍照」就会向别名/数值变量/舵机真下发 {path,src}。
			 * 与 agentdriver 对 LLM 驱动的种类校验同一条判据。 */
			if (cfg.verbose) {
				log << "提示: 硬件变量 '" << cfg.cameraVar << "' 不是 Lua 驱动（实际是 "
					<< tctx.camera->kindName() << "），本次不提供 capture_image 工具\n";
			}
			tctx.camera = nullptr;
		}
	}
	return tctx;
}

} // namespace agent
} // namespace bot
