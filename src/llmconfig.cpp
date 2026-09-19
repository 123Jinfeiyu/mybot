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

std::string readWholeFile(const std::string& path, std::size_t cap, std::string& err) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "无法打开文件 " + path;
		return "";
	}
	std::string data;
	data.resize(cap);
	f.read(&data[0], static_cast<std::streamsize>(cap));
	std::streamsize n = f.gcount();
	if (n < 0) { n = 0; }
	data.resize(static_cast<std::size_t>(n));
	return data;
}

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
		err = "配置文件 " + path + " 的字段 '" + key + "' 需要在 [" + std::to_string(lo)
			+ ", " + std::to_string(hi) + "] 范围内，实际是 " + os.str();
		return false;
	}
	if (std::floor(d) != d) {
		err = typeError(path, key, "整数");
		return false;
	}
	out = static_cast<int>(d);
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

} // namespace

bool loadLlmConfigFile(const std::string& path, LlmFileConfig& out, std::string& err) {
	std::string text = readWholeFile(path, kMaxConfigBytes, err);
	if (err.empty() && text.empty()) {
		err = "配置文件为空: " + path;
	}
	if (!err.empty()) { return false; }

	Json root;
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

	/* 未知字段拒绝：拼错一个键（如 "apikey"）而静默走缺省，会让人以为配置生效了。
	 * 与「未知 --key 退出 2」同一口径。 */
	static const std::set<std::string> kKnown = {
		"driver", "var", "endpoint", "model", "api_key", "api_key_file",
		"timeout", "max_tokens", "temperature", "top_p",
		"reasoning_effort", "thinking", "max_turns", "scripts_dir", "allow_shell",
		"camera_var", "camera_device", "camera_src", "allow_camera",
	};
	for (const auto& kv : root.items()) {
		if (kKnown.count(kv.first) == 0) {
			err = "配置文件 " + path + " 含未知字段 '" + kv.first + "'（拼写错误？）";
			return false;
		}
	}

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
	/* timeout 上限与驱动侧 llm_http.clamp_timeout 的钳制区间一致：宿主接受什么，
	 * 驱动就照发什么，不做「先接受再悄悄改写」 */
	if (!wantInt(path, root, "timeout", 1, 600, timeoutSec, err)) { return false; }
	if (!wantInt(path, root, "max_tokens", 1, 200000, maxTokens, err)) { return false; }
	if (!wantInt(path, root, "max_turns", 1, 100, maxTurns, err)) { return false; }
	if (timeoutSec) { out.timeoutSec = *timeoutSec; }
	if (maxTokens) { out.maxTokens = *maxTokens; }
	if (maxTurns) { out.maxTurns = *maxTurns; }

	if (!wantBool(path, root, "allow_shell", out.allowShell, err)) { return false; }
	if (!wantBool(path, root, "allow_camera", out.allowCamera, err)) { return false; }
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
	std::string text = readWholeFile(real, kMaxKeyFileBytes, err);
	if (!err.empty()) { return false; }
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

} // namespace bot
