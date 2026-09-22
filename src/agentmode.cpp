#include "agentmode.hpp"

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "agentloop.hpp"
#include "llmconfig.hpp"
#include "promptpack.hpp"
#include "agenttools.hpp"
#include "hardware.hpp"
#include "robot.hpp"
#include "util.hpp"

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
	s += "  审计轨迹    : " + orNone(cfg.auditLogPath, "(未开启)") + "\n";
	s += "  策略钩子    : " + orNone(cfg.policyPath, "(未启用)") + "\n";
	if (!cfg.configWarning.empty()) { s += "  配置告警    : " + cfg.configWarning + "\n"; }
	s += "  任务模式    : " + std::string(cfg.task.empty() ? "交互 REPL" : "一次性 --task") + "\n";
	return s;
}

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
	d.maxRetries = cfg.maxRetries;
	d.retryBackoffMs = cfg.retryBackoffMs;
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

	/* 运行期数值（B4）：只有配置文件这一层（不加 CLI 选项，理由见 llm_limits）。
	 * 0 是合法值（max_retries / retry_backoff_ms），故用 has_value 语义判空。 */
	if (file.maxCallsPerRound) { out.maxCallsPerRound = *file.maxCallsPerRound; }
	if (file.maxRetries) { out.maxRetries = *file.maxRetries; }
	if (file.retryBackoffMs) { out.retryBackoffMs = *file.retryBackoffMs; }
	if (file.runCommandTimeoutSec) { out.commandTimeoutSec = *file.runCommandTimeoutSec; }
	if (file.runCommandMaxTimeoutSec) {
		out.maxCommandTimeoutSec = *file.runCommandMaxTimeoutSec;
	}
	/* PL-2：动作脚本执行预算。0 合法 = 显式关闭预算（同 max_retries 的 has_value 语义）。 */
	if (file.actionScriptTimeoutSec) {
		out.scriptTimeoutSec = *file.actionScriptTimeoutSec;
	}
	/* 跨字段关系按**生效值**判（不是按文件里的字面值）：只配了上限而它低于缺省
	 * 超时时同样要挡——否则模型不传 timeout_seconds 时按缺省请求 10s 会越过操作者
	 * 声明的 5s 上限，「上限」名不副实。位于合流处 = 单一判定点。 */
	if (out.maxCommandTimeoutSec < out.commandTimeoutSec) {
		err = "run_command 超时上限低于缺省超时（run_command_max_timeout_sec="
			+ std::to_string(out.maxCommandTimeoutSec) + "s < run_command_timeout_sec="
			+ std::to_string(out.commandTimeoutSec) + "s）：请把上限调到不小于缺省，"
			"或一并调小缺省超时";
		return false;
	}

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
	if (!resolveApiKey(file, cfgPath, out, err)) { return false; }

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
	/* 安全缺省（评估 C12 翻转 + 审计 4.1 收紧）：run_command 缺省**不提供**，
	 * 且**只能由命令行 --agent-shell 开启**。配置文件里的 allow_shell=true 一律
	 * 拒绝加载并指名原因——缺省查找会读 CWD 的 ./mybot.llm.json
	 * （findDefaultLlmConfig），于是一个未经审查的落地文件就能同时改 endpoint
	 * （把含结构脚本、硬件清单、摄像头画面的上下文外发）并注册 run_command
	 * （模型回复直接变成主机 shell）。「这个文件存在」不构成授权，
	 * 故配置文件只能**关**不能开。 */
	out.allowShell = opts.agentShell;
	if (!opts.agentShell && file.allowShell.value_or(false)) {
		err = "配置文件 " + (cfgPath.empty() ? std::string("(缺省查找)") : cfgPath)
			+ " 设置了 allow_shell=true：run_command 等于把主机 shell 交给模型，"
			+ "只能由命令行 --agent-shell 显式开启，配置文件不能开"
			+ "（请把 allow_shell 置回 false 或删除该字段）";
		return false;
	}
	out.structurePath = opts.robot ? *opts.robot : "";
	out.task = opts.agentTask;

	/* ---- 8a) 上下文字节预算（B1）：配置文件覆盖编译期缺省；0 = 不限。
	 * 严格解析（类型/区间/未知字段）已在 llmconfig 完成，这里只做搬运。 ---- */
	if (file.maxHistoryBytes) { out.maxHistoryBytes = *file.maxHistoryBytes; }
	if (file.maxTaskToolResultBytes) { out.maxTaskToolResultBytes = *file.maxTaskToolResultBytes; }
	if (file.maxTaskImageBytes) { out.maxTaskImageBytes = *file.maxTaskImageBytes; }

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

	/* ---- 9) 提示词体积预检：**不依赖硬件**的三段（2026-09-21 审计 AG-8）----
	 * 总量上限原本只在 buildAgentSystemPrompt 里查，而那时 robot.active() 已经
	 * 跑过了——与「配置错误不碰真机」的口径不符。硬件变量清单那一段确实要到
	 * 上电后才算得出来（变量是 active() 时装配的），但另外三段不用：结构脚本头、
	 * 动作脚本清单、提示词包在配置组装期就都是已知量。
	 * 在这里先查一遍有三个好处：
	 *   ① 三段中任何一段自己就已顶穿上限时，**上电之前**就失败；
	 *   ② 顺带在配置期读一遍结构脚本头与脚本清单——它们进系统提示词前要 UTF-8
	 *      消毒（AG-1），早读早暴露，而不是等上电后拼提示词时才发现问题；
	 *   ③ 报错口径与 buildAgentSystemPrompt 完全一致（同一常量、同一文案），
	 *      不会出现「配置期放过、上电后拒绝」的分叉。
	 * 注意：这不替代 buildAgentSystemPrompt 的总量检查——那一段（含硬件清单）
	 * 仍是唯一的最终判据，这里只是把可前移的部分前移。 */
	{
		/* 读一次存两份（C6）：预检要的就是这两段成品，buildAgentSystemPrompt
		 * 直接复用，不再各读一遍（原先两处各读各的，文件在两次读之间变动会让
		 * 「预检口径」与「提示词口径」分叉）。 */
		out.structureHead = structureScriptHead(out.structurePath, 16u << 10);
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
	}

	/* ---- 10) 审计轨迹（--audit-log / 配置文件 audit_log；"-" = stderr）----
	 * 在这里就打开：路径打不开属于配置错误，必须在结构装载与上电之前失败
	 * （「配置错误不碰真机」），而不是等到第一次工具调用才炸。 */
	out.auditLogPath = pickString(opts.auditLog, nullptr, file.auditLog);
	if (!out.auditLogPath.empty()) {
		out.audit = std::make_shared<AuditLog>();
		if (std::string auditErr; !out.audit->open(out.auditLogPath, auditErr)) {
			err = auditErr;
			return false;
		}
	}

	/* ---- 11) 策略钩子（--agent-policy / 配置文件 agent_policy）----
	 * 在这里就加载：策略文件写错属于配置错误，必须在上电之前失败（同一口径）。
	 * 注意它**不是**安全边界：确认程序与宿主同权限，真正的隔离仍是专用
	 * 用户 / 容器（docs/agent.md 安全一节的既有口径不变）。 */
	out.policyPath = pickString(opts.agentPolicy, nullptr, file.agentPolicy);
	if (!out.policyPath.empty()) {
		if (std::string policyErr; !(out.policy = AgentPolicy::load(out.policyPath, policyErr))) {
			err = policyErr;
			return false;
		}
	}
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
	return std::string("警告: run_command 已启用（--agent-shell 或 allow_shell=true），"
		"而 API key 来自 ") + how
		+ "。run_command 的子进程与宿主同属一个用户，可直接读出该密钥"
		"（chmod 600 这类文件权限位对此无效）。"
		"如需避免：改用环境变量 BOT4CJ_LLM_KEY 提供密钥"
		"（该变量名含 key，会被 run_command 的环境清洗剔除），"
		"或去掉 --agent-shell / 把 allow_shell 置回 false（缺省即关闭）。";
}

int runAgentMode(const AgentModeConfig& cfg, RobotStruct& robot, ChatTransport& transport,
		std::istream& in, std::ostream& out, std::ostream& log) {
	AgentToolContext tctx;
	tctx.robot = &robot;
	tctx.scriptsDir = cfg.scriptsDir;
	tctx.structurePath = cfg.structurePath;
	tctx.allowShell = cfg.allowShell;
	tctx.commandTimeoutSec = cfg.commandTimeoutSec;
	tctx.maxCommandTimeoutSec = cfg.maxCommandTimeoutSec;
	tctx.scriptTimeoutSec = cfg.scriptTimeoutSec;	/* PL-2：动作脚本执行预算 */
	if (cfg.allowCamera) {
		tctx.camera = robot.registry().find(cfg.cameraVar);
		tctx.cameraDevice = cfg.cameraDevice;
		tctx.cameraSrc = cfg.cameraSrc;
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

	AgentToolRegistry tools = makeAgentTools(tctx);

	AgentOptions aopts;
	aopts.maxTurns = cfg.maxTurns;
	aopts.verbose = cfg.verbose;
	aopts.log = &log;
	aopts.maxHistoryBytes = cfg.maxHistoryBytes;
	aopts.maxTaskToolResultBytes = cfg.maxTaskToolResultBytes;
	aopts.maxTaskImageBytes = cfg.maxTaskImageBytes;
	aopts.maxCallsPerRound = cfg.maxCallsPerRound;
	aopts.systemPrompt = buildAgentSystemPrompt(cfg, robot);
	aopts.audit = cfg.audit;
	aopts.policy = cfg.policy;

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

	/* ---- 交互 REPL：硬件全程常驻，可连续下达任务 ----
	 * 循环本体在 runAgentRepl（C3 拆出，可离线单测）；这里只做装配。 */
	return runAgentRepl(session, cfg, robot, in, out, log);
}

int runAgentRepl(AgentSession& session, const AgentModeConfig& cfg,
		RobotStruct& robot, std::istream& in, std::ostream& out, std::ostream& log) {
	/* 状态指示提示符组件（2026-09-22 可用性改进）：提取结构脚本名（去路径和扩展名）、
	 * 硬件变量数量，让用户在提示符即可看到当前上下文，无需反复 /hardware 查询。 */
	std::string robotName = cfg.structurePath;
	if (std::size_t slash = robotName.rfind('/'); slash != std::string::npos) {
		robotName = robotName.substr(slash + 1);
	}
	if (robotName.size() > 4 && robotName.substr(robotName.size() - 4) == ".sql") {
		robotName = robotName.substr(0, robotName.size() - 4);
	}
	const std::size_t hwCount = robot.registry().byName().size();

	out << "agent 模式：机器人已上电常驻（结构脚本 " << cfg.structurePath << "）。\n"
		<< "直接输入任务；/hardware 看硬件清单，/scripts 看动作脚本，/clear 清空对话记忆，"
		"/help 看帮助，/exit 退出。\n";
	std::string line;
	while (!stopRequested()) {
		/* 状态提示符格式：mybot[robot_name|llm_var|N_hw]>
		 * 示例：mybot[spider1|llm0|14hw]> */
		out << "mybot[" << robotName << "|" << cfg.llmVar << "|" << hwCount << "hw]> " << std::flush;
		if (!std::getline(in, line)) { break; }
		/* 输入长度限制（2026-09-22 审计后添加）：防止恶意输入巨大字符串
		 * 导致内存耗尽。getline 本身无限制，只在坏分配时 bad_alloc。
		 * 超限响亮拒绝并提示，不静默截断（与配置文件同款口径）。 */
		if (line.size() > bot::agent::kMaxReplInputBytes) {
			log << "错误: 输入过长（" << line.size() << " 字节，上限 "
				<< bot::agent::kMaxReplInputBytes << " 字节）；请缩短输入或分多次提交\n";
			continue;
		}
		std::string task = trim(line);
		if (task.empty()) { continue; }
		if (task == "/exit" || task == "/quit") { break; }
		if (task == "/help") {
			out << "内置命令:\n"
				<< "  /hardware  - 显示硬件清单（只读，不触发任何动作）\n"
				<< "  /scripts   - 显示可用动作脚本清单（相对路径 + 首行注释）\n"
				<< "  /clear     - 清空对话记忆（保留硬件上电状态与系统提示词）\n"
				<< "  /help      - 显示此帮助\n"
				<< "  /exit      - 退出 agent 模式\n"
				<< "  /quit      - 同 /exit\n"
				<< "直接输入任务描述即可让智能体执行，例如：\n"
				<< "  让机器人站立\n"
				<< "  拍照并描述看到的内容\n";
			continue;
		}
		if (task == "/clear") {
			/* C1：上下文被一次错误结论污染后，此前唯一出路是重启进程——而重启
			 * 会丢掉硬件常驻状态（执行器停在最后指令位置，现场代价不小）。
			 * /clear 只裁会话历史，机器人保持上电。 */
			session.clearHistory();
			out << "已清空对话记忆（硬件保持上电，系统提示词不变）。\n";
			continue;
		}
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
