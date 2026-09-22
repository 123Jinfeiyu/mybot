#include "llmconfig.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "jsonparse.hpp"
#include "util.hpp"

namespace bot {

namespace {

constexpr std::size_t kMaxConfigBytes = 64u << 10;
constexpr std::size_t kMaxKeyFileBytes = 64u << 10;

bool fileReadable(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	return static_cast<bool>(f);
}

/* ~ 展开：只处理 "~" 与 "~/" 两种形态（不做 ~user，那要查 passwd，收益不值当） */
std::string expandHome(const std::string& path) {
	if (path.empty() || path[0] != '~') { return path; }
	if (path.size() > 1 && path[1] != '/') { return path; }
	auto home = getEnvVar("HOME");
	if (!home || home->empty()) { return path; }
	return *home + path.substr(1);
}

/* 阶段 4.1：本地 readWholeFile 已收敛到 util::readWholeFileLimited——
 * 旧实现读满 cap 即返回、不查剩余，把超限文件静默截断后伪装成
 * 「不是合法 JSON」（密钥文件则伪装成「密钥错误」）。 */

/* 字段类型不符时的统一报错（带字段名与期望类型，排障时不用猜） */
std::string typeError(const std::string& path, const std::string& key, const char* want) {
	return "配置文件 " + path + " 的字段 '" + key + "' 需要" + want;
}

bool wantString(const std::string& path, const Json& root, const char* key,
		std::optional<std::string>& out, std::string& err) {
	auto v = root.get(key);
	if (!v) { return true; }
	if (!v->isStr()) {
		err = typeError(path, key, "字符串");
		return false;
	}
	/* 空串按「未配置」处理：示例配置里留空占位是常见写法，若当成有效值会让
	 * api_key_file:"" 变成「打开空路径」这种与意图脱节的报错，也会把
	 * endpoint:"" 错误地压成空接入点。 */
	if (!v->getValue().empty()) { out = v->getValue(); }
	return true;
}

bool wantNumber(const std::string& path, const Json& root, const char* key,
		std::optional<double>& out, std::string& err) {
	auto v = root.get(key);
	if (!v) { return true; }
	if (!v->isInt() && !v->isFloat()) {
		err = typeError(path, key, "数字");
		return false;
	}
	out = v->isInt() ? static_cast<double>(v->getInt()) : v->getFloat();
	return true;
}

bool wantInt(const std::string& path, const Json& root, const char* key,
		long lo, long hi, std::optional<int>& out, std::string& err) {
	std::optional<double> num;
	if (!wantNumber(path, root, key, num, err)) { return false; }
	if (!num) { return true; }
	double d = *num;
	/* P1-5 防线（2026-09-19 评估）：先判有限与值域，再谈取整。配置里写
	 * "timeout": 1e300（或 inf/nan）时 static_cast<long long>(d) 是
	 * float-cast-overflow UB，范围检查根本来不及执行。 */
	if (!std::isfinite(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
		std::ostringstream os;
		os << d;
		err = rangeErrorText("配置文件 " + path + " 的字段 '" + key + "'", lo, hi, os.str());
		return false;
	}
	if (std::floor(d) != d) {
		err = typeError(path, key, "整数");
		return false;
	}
	out = static_cast<int>(d);
	return true;
}

/* 无符号长整数字段（B1：字节预算），0 合法 =「不限」，上限 llm_limits::…Limit。
 * 与 wantInt 同一条有限/值域/取整防线。 */
bool wantBudgetLong(const std::string& path, const Json& root, const char* key,
		long hi, std::optional<long>& out, std::string& err) {
	auto v = root.get(key);
	if (!v) { return true; }
	if (!v->isInt() && !v->isFloat()) {
		err = typeError(path, key, "数字");
		return false;
	}
	double d = v->isInt() ? static_cast<double>(v->getInt()) : v->getFloat();
	if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(hi)) {
		std::ostringstream os;
		os << d;
		err = rangeErrorText("配置文件 " + path + " 的字段 '" + key + "'", 0, hi, os.str());
		return false;
	}
	if (std::floor(d) != d) {
		err = typeError(path, key, "整数");
		return false;
	}
	out = static_cast<long>(d);
	return true;
}

bool wantBool(const std::string& path, const Json& root, const char* key,
		std::optional<bool>& out, std::string& err) {
	auto v = root.get(key);
	if (!v) { return true; }
	if (!v->isBool()) {
		err = typeError(path, key, "布尔值");
		return false;
	}
	out = v->getBool();
	return true;
}

bool wantStringArray(const std::string& path, const Json& root, const char* key,
		std::optional<std::vector<std::string>>& out, std::string& err) {
	auto v = root.get(key);
	if (!v) { return true; }
	if (!v->isArr()) {
		err = typeError(path, key, "字符串数组");
		return false;
	}
	std::vector<std::string> items;
	for (const Json& item : v->arr) {
		if (!item.isStr()) {
			err = "配置文件 " + path + " 的字段 '" + key + "' 的元素需要字符串";
			return false;
		}
		if (!item.getValue().empty()) { items.push_back(item.getValue()); }
	}
	/* 空数组按「未配置」处理：与 wantString 的空串口径一致，占位写法不生效 */
	if (!items.empty()) { out = std::move(items); }
	return true;
}

/* ---- 读取 + 解析 + 顶层形态 + 未知字段（F2-8 拆分之一）----
 * 未知字段拒绝：拼错一个键（如 "apikey"）而静默走缺省，会让人以为配置生效了。
 * 与「未知 --key 退出 2」同一口径。 */
bool loadConfigRoot(const std::string& path, Json& root, std::string& err) {
	std::string text;
	if (!readWholeFileLimited(path, kMaxConfigBytes, text, err, "配置文件")) { return false; }
	if (text.empty()) {
		err = "配置文件为空: " + path;
		return false;
	}
	try {
		root = parseJson(text);
	} catch (const std::exception& e) {
		err = "配置文件 " + path + " 不是合法 JSON: " + e.what();
		return false;
	}
	if (!root.isObj()) {
		err = "配置文件 " + path + " 的顶层必须是 JSON 对象";
		return false;
	}
	static const std::set<std::string> kKnown = {
		"driver", "var", "endpoint", "model", "api_key", "api_key_file",
		"timeout", "max_tokens", "temperature", "top_p",
		"reasoning_effort", "thinking", "max_turns", "scripts_dir", "allow_shell",
		"prompt_files", "camera_var", "camera_device", "camera_src", "allow_camera",
		"audit_log", "agent_policy",
		"max_history_bytes", "max_task_tool_result_bytes", "max_task_image_bytes",
		"max_calls_per_round", "max_retries", "retry_backoff_ms",
		"run_command_timeout_sec", "run_command_max_timeout_sec",
		"action_script_timeout_sec",
	};
	for (const auto& kv : root.items()) {
		if (kKnown.count(kv.first) == 0) {
			err = "配置文件 " + path + " 含未知字段 '" + kv.first + "'（拼写错误？）";
			return false;
		}
	}
	return true;
}

/* 字符串族字段（含 thinking 的三态：字符串 enabled/disabled 或布尔） */
bool parseConfigStrings(const std::string& path, const Json& root, LlmFileConfig& out,
		std::string& err) {
	if (!wantString(path, root, "driver", out.driver, err)) { return false; }
	if (!wantString(path, root, "var", out.var, err)) { return false; }
	if (!wantString(path, root, "endpoint", out.endpoint, err)) { return false; }
	if (!wantString(path, root, "model", out.model, err)) { return false; }
	if (!wantString(path, root, "api_key", out.apiKey, err)) { return false; }
	if (!wantString(path, root, "api_key_file", out.apiKeyFile, err)) { return false; }
	if (!wantString(path, root, "reasoning_effort", out.reasoningEffort, err)) { return false; }
	if (!wantString(path, root, "scripts_dir", out.scriptsDir, err)) { return false; }
	if (!wantString(path, root, "camera_var", out.cameraVar, err)) { return false; }
	if (!wantString(path, root, "camera_device", out.cameraDevice, err)) { return false; }
	if (!wantString(path, root, "camera_src", out.cameraSrc, err)) { return false; }
	if (!wantString(path, root, "audit_log", out.auditLog, err)) { return false; }
	if (!wantString(path, root, "agent_policy", out.agentPolicy, err)) { return false; }

	/* thinking 允许 "enabled"/"disabled" 字符串，也允许 true/false 布尔 */
	if (auto v = root.get("thinking")) {
		if (v->isStr()) {
			out.thinking = v->getValue();
		} else if (v->isBool()) {
			out.thinking = v->getBool() ? "enabled" : "disabled";
		} else {
			err = typeError(path, "thinking", "字符串（enabled/disabled）或布尔值");
			return false;
		}
	}
	return true;
}

/* 数值族字段：temperature/top_p 为浮点（各自区间），timeout/max_tokens/max_turns
 * 为整数（区间取自 llm_limits 单点——timeout 上限与驱动侧 llm_http.clamp_timeout
 * 一致：宿主接受什么，驱动就照发什么，不做「先接受再悄悄改写」） */
bool parseConfigNumbers(const std::string& path, const Json& root, LlmFileConfig& out,
		std::string& err) {
	std::optional<double> temperature, topP;
	if (!wantNumber(path, root, "temperature", temperature, err)) { return false; }
	if (!wantNumber(path, root, "top_p", topP, err)) { return false; }
	if (temperature && (*temperature < 0.0 || *temperature > 2.0)) {
		err = "配置文件 " + path + " 的字段 'temperature' 需要在 [0, 2] 范围内";
		return false;
	}
	if (topP && (*topP <= 0.0 || *topP > 1.0)) {
		err = "配置文件 " + path + " 的字段 'top_p' 需要在 (0, 1] 范围内";
		return false;
	}
	out.temperature = temperature;
	out.topP = topP;

	std::optional<int> timeoutSec, maxTokens, maxTurns;
	if (!wantInt(path, root, "timeout", llm_limits::kTimeoutMinSec, llm_limits::kTimeoutMaxSec,
			timeoutSec, err)) { return false; }
	if (!wantInt(path, root, "max_tokens", llm_limits::kMaxTokensMin, llm_limits::kMaxTokensMax,
			maxTokens, err)) { return false; }
	if (!wantInt(path, root, "max_turns", llm_limits::kMaxTurnsMin, llm_limits::kMaxTurnsMax,
			maxTurns, err)) { return false; }
	if (timeoutSec) { out.timeoutSec = *timeoutSec; }
	if (maxTokens) { out.maxTokens = *maxTokens; }
	if (maxTurns) { out.maxTurns = *maxTurns; }

	/* 上下文字节预算（B1）：0 = 不限，上限 llm_limits 单点 */
	std::optional<long> maxHistory, maxTool, maxImage;
	if (!wantBudgetLong(path, root, "max_history_bytes",
			llm_limits::kMaxHistoryBytesLimit, maxHistory, err)) { return false; }
	if (!wantBudgetLong(path, root, "max_task_tool_result_bytes",
			llm_limits::kMaxTaskToolResultBytesLimit, maxTool, err)) { return false; }
	if (!wantBudgetLong(path, root, "max_task_image_bytes",
			llm_limits::kMaxTaskImageBytesLimit, maxImage, err)) { return false; }
	out.maxHistoryBytes = maxHistory;
	out.maxTaskToolResultBytes = maxTool;
	out.maxTaskImageBytes = maxImage;

	/* 运行期数值（B4）：单点区间见 llm_limits；跨字段关系（run_command 上限 ≥
	 * 缺省）不在这里判——配置文件可能只给一个字段，那样要拿**生效值**（另一个
	 * 是缺省）比，判定落在 agentmode 的合流处。 */
	std::optional<int> maxCalls, maxRetries, backoffMs, rcTimeout, rcMaxTimeout;
	std::optional<int> scriptTimeout;
	if (!wantInt(path, root, "max_calls_per_round",
			llm_limits::kMaxCallsPerRoundMin, llm_limits::kMaxCallsPerRoundMax,
			maxCalls, err)) { return false; }
	if (!wantInt(path, root, "max_retries",
			llm_limits::kMaxRetriesMin, llm_limits::kMaxRetriesMax,
			maxRetries, err)) { return false; }
	if (!wantInt(path, root, "retry_backoff_ms",
			llm_limits::kRetryBackoffMsMin, llm_limits::kRetryBackoffMsMax,
			backoffMs, err)) { return false; }
	if (!wantInt(path, root, "run_command_timeout_sec",
			llm_limits::kRunCommandTimeoutMinSec, llm_limits::kRunCommandTimeoutMaxSec,
			rcTimeout, err)) { return false; }
	if (!wantInt(path, root, "run_command_max_timeout_sec",
			llm_limits::kRunCommandMaxTimeoutMinSec, llm_limits::kRunCommandMaxTimeoutMaxSec,
			rcMaxTimeout, err)) { return false; }
	/* PL-2：动作脚本执行预算。0 合法 = 显式关闭（与 max_retries=0 同款语义：
	 * 「未配置」与「配成 0」必须区分，故走 optional 而不是 `if (*opt)`）。 */
	if (!wantInt(path, root, "action_script_timeout_sec",
			llm_limits::kActionScriptTimeoutMinSec, llm_limits::kActionScriptTimeoutMaxSec,
			scriptTimeout, err)) { return false; }
	out.maxCallsPerRound = maxCalls;
	out.maxRetries = maxRetries;
	out.retryBackoffMs = backoffMs;
	out.runCommandTimeoutSec = rcTimeout;
	out.runCommandMaxTimeoutSec = rcMaxTimeout;
	out.actionScriptTimeoutSec = scriptTimeout;
	return true;
}

/* 布尔与数组族字段 */
bool parseConfigBools(const std::string& path, const Json& root, LlmFileConfig& out,
		std::string& err) {
	if (!wantBool(path, root, "allow_shell", out.allowShell, err)) { return false; }
	if (!wantStringArray(path, root, "prompt_files", out.promptFiles, err)) { return false; }
	if (!wantBool(path, root, "allow_camera", out.allowCamera, err)) { return false; }
	return true;
}

} // namespace

bool loadLlmConfigFile(const std::string& path, LlmFileConfig& out, std::string& err) {
	Json root;
	if (!loadConfigRoot(path, root, err)) { return false; }
	/* 阶段顺序 = 报错优先级：字符串族 → 浮点 → 整数 → 布尔/数组。
	 * 配置文件有多个字段非法时，报的是这条链上第一个命中的字段。 */
	if (!parseConfigStrings(path, root, out, err)) { return false; }
	if (!parseConfigNumbers(path, root, out, err)) { return false; }
	if (!parseConfigBools(path, root, out, err)) { return false; }
	return true;
}

std::string findDefaultLlmConfig() {
	static const char* kCandidates[] = {"./mybot.llm.json", "./mybot.llm.conf.json"};
	for (const char* c : kCandidates) {
		if (fileReadable(c)) { return c; }
	}
	auto home = getEnvVar("HOME");
	if (home && !home->empty()) {
		std::string p = *home + "/.config/mybot/llm.json";
		if (fileReadable(p)) { return p; }
	}
	return "";
}

bool readApiKeyFile(const std::string& path, std::string& out, std::string& err) {
	std::string real = expandHome(path);
	std::string text;
	if (!readWholeFileLimited(real, kMaxKeyFileBytes, text, err, "密钥文件")) { return false; }
	out = trim(text);	/* 末尾换行混进 Authorization 头会变成 401 */
	if (out.empty()) {
		err = "密钥文件为空: " + real;
		return false;
	}
	return true;
}

std::string secretFilePermissionWarning(const std::string& path) {
	struct stat st {};
	std::string real = expandHome(path);
	if (::stat(real.c_str(), &st) != 0) { return ""; }
	mode_t bad = S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	if ((st.st_mode & bad) == 0) { return ""; }
	return "提示: " + real + " 对同组/其他用户可读（建议 chmod 600），它保存着 API key";
}

/* 密钥字符集判据（2026-09-21 审计 AG-14）。见头文件；这里补两点实现理由：
 *   * 判据是「全部控制字符」而不是只挑 CR/LF——libcurl 对 slist 条目整条交出，
 *     任何控制字符都没有出现在合法 Bearer token 里的理由；宽判据也更抗未来
 *     libcurl 行为变化（今天拒 CR/LF，明天可能连同其他字符一起拒）。
 *   * err 里带**位置与字节值**：多行密钥文件的成因通常是「顺手 cat 了别的东西
 *     进来」，操作者需要一眼看到是第几个字节坏了。 */
bool validateApiKeyChars(const std::string& key, std::string& err) {
	for (std::size_t i = 0; i < key.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(key[i]);
		if (c < 0x20 || c == 0x7f) {
			char buf[8];
			std::snprintf(buf, sizeof buf, "0x%02x", c);
			err = "含控制字符 " + std::string(buf) + "（第 " + std::to_string(i + 1)
				+ " 个字节）：密钥必须是**单行**文本，换行会被 libcurl 原样拼进 "
				  "Authorization 头，等于把后面的内容变成额外请求头；"
				  "请检查密钥文件是否混入了多行内容或 CRLF 行尾";
			return false;
		}
	}
	return true;
}

} // namespace bot
