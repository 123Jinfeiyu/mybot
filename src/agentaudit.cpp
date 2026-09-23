/* agentaudit.cpp —— 审计轨迹实现（见 agentaudit.hpp 的设计约束） */

#include "agentaudit.hpp"

#include <cstdint>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "agent_subprocess.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

/* FNV-1a 64 位参数（素数与偏移基是算法标准值） */
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kFnvBasis = 14695981039346656037ULL;

/* 遮蔽专用补充词：对「环境变量名清洗」无意义（没有叫这个的环境变量要挡），
 * 但命令文本里的凭据头（Authorization: Bearer xxx）恰好长这样 */
bool isMaskOnlyWord(const std::string& w) {
	return w == "authorization" || w == "bearer";
}

bool maskedWord(const std::string& w) {
	return isSensitiveEnvName(w) || isMaskOnlyWord(w);
}

/* 与 isSensitiveEnvName 同一整词切词法：名字按非字母数字分段，任一段命中即遮蔽
 * （api-key → {api, key} 命中；keystone → {keystone} 不命中） */
bool isMaskedName(const std::string& lowerName) {
	std::size_t i = 0;
	while (i < lowerName.size()) {
		if (!std::isalnum(static_cast<unsigned char>(lowerName[i]))) { ++i; continue; }
		std::size_t j = i;
		while (j < lowerName.size() && std::isalnum(static_cast<unsigned char>(lowerName[j]))) { ++j; }
		if (maskedWord(lowerName.substr(i, j - i))) { return true; }
		i = j;
	}
	return false;
}

std::string toLowerAscii(const std::string& s) {
	std::string out(s);
	for (char& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return out;
}

} // namespace

AuditLog::~AuditLog() {
	if (file_ != nullptr) { std::fclose(static_cast<FILE*>(file_)); }
}

bool AuditLog::open(const std::string& path, std::string& err) {
	if (opened_) {
		err = "审计日志已打开: " + path_;
		return false;
	}
	if (path.empty()) {
		err = "审计日志路径为空";
		return false;
	}
	/* 会话 id：时间 + pid 的 64 位 FNV 指纹取 8 位 hex——只用于把同一会话的
	 * 行对上号，不要求不可预测。 */
	{
		struct timespec ts{};
		clock_gettime(CLOCK_REALTIME, &ts);
		const std::uint64_t seed = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
			+ static_cast<std::uint64_t>(ts.tv_nsec)
			+ static_cast<std::uint64_t>(::getpid()) * 0x9E3779B97F4A7C15ULL;
		sessionId_ = fingerprint64(std::to_string(seed)).substr(0, 8);
	}
	if (path == "-") {
		toStderr_ = true;
	} else {
		FILE* f = std::fopen(path.c_str(), "a");
		if (f == nullptr) {
			err = "无法打开审计日志文件 '" + path + "'（errno=" + std::to_string(errno)
				+ "）；请检查目录是否存在、是否有写权限，或在配置错误阶段先修正路径";
			return false;
		}
		file_ = f;
	}
	path_ = path;
	opened_ = true;
	return true;
}

/* SAN-9：接入点进审计前去掉 query 与 userinfo。
 * 有些网关把凭据放在 URL 里（`https://user:tok@host/v1/...?key=...`），
 * 而审计是 append-only 的落盘文件——把整条 URL 抄进去等于把密钥写进日志。
 * 只保留 scheme://host/path 这部分（口径与 maskSecretsInText 同源：
 * "不该出门的东西不进日志"）。 */
std::string sanitizeEndpoint(const std::string& url) {
	std::string out = url;
	/* 去掉 query / fragment */
	if (std::size_t q = out.find_first_of("?#"); q != std::string::npos) { out.resize(q); }
	/* 去掉 userinfo（scheme:// 与第一个 '/' 之间的 '@' 之前那段） */
	const std::size_t scheme = out.find("://");
	if (scheme != std::string::npos) {
		const std::size_t hostStart = scheme + 3;
		const std::size_t slash = out.find('/', hostStart);
		const std::size_t end = (slash == std::string::npos) ? out.size() : slash;
		if (std::size_t at = out.rfind('@', end); at != std::string::npos && at >= hostStart) {
			out.erase(hostStart, at - hostStart + 1);
		}
	}
	return out;
}

void AuditLog::write(const AuditEntry& e) {
	if (!opened_) { return; }

	Json line = JsonObject();
	line.put("ts", JsonString(auditTimestamp()));
	line.put("session", JsonString(sessionId_));
	/* kind 显式写在最前（紧跟 session）：老消费方不看它，新消费方按它分派。
	 * 工具行也显式写 "tool"——不是"缺省即 tool"的隐式约定，省得后来者猜。 */
	line.put("kind", JsonString(e.kind.empty() ? "tool" : e.kind));

	if (e.kind == "session") {
		/* ---- 会话边界记录（SAN-9）：只记"这次会话是用什么配置跑的"，
		 * 供事后复盘时定位"这一轮到底连的是哪、用的哪个模型"。
		 * **绝不写密钥**：只写 api_key_source 枚举名（与 --check-config 的
		 * 摘要同口径：只报来源不报值）。 */
		line.put("status", JsonString(e.status.empty() ? "start" : e.status));
		if (!e.driver.empty()) { line.put("driver", JsonString(e.driver)); }
		if (!e.model.empty()) { line.put("model", JsonString(e.model)); }
		if (!e.endpoint.empty()) {
			line.put("endpoint", JsonString(sanitizeEndpoint(e.endpoint)));
		}
		if (!e.apiKeySource.empty()) {
			line.put("api_key_source", JsonString(e.apiKeySource));
		}
		if (!e.error.empty()) { line.put("error", JsonString(e.error)); }
		if (e.ms > 0) { line.put("ms", JsonInt(e.ms)); }
	} else if (e.kind == "model") {
		/* ---- 模型轮次记录（SAN-9）：为什么需要它——工具行只能回答
		 * "调了什么工具"，回答不了"模型说了什么、花了多久、这一轮有没有发出
		 * 请求"。一次在模型调用阶段失败的会话此前在日志里**一条都没有**。 */
		if (e.turn >= 0) { line.put("turn", JsonInt(e.turn)); }
		line.put("ok", JsonBool(e.ok));
		if (!e.status.empty()) { line.put("status", JsonString(e.status)); }
		if (!e.error.empty()) { line.put("error", JsonString(e.error)); }
		line.put("ms", JsonInt(e.ms));
		if (e.reqBytes > 0) { line.put("req_bytes", JsonInt(static_cast<std::int64_t>(e.reqBytes))); }
		if (e.respBytes > 0) { line.put("resp_bytes", JsonInt(static_cast<std::int64_t>(e.respBytes))); }
		if (e.attempts > 0) { line.put("attempts", JsonInt(e.attempts)); }
		/* token 用量：**只记 provider 给的数**，不做任何推算/计费估算 */
		if (e.promptTokens.has_value()) { line.put("prompt_tokens", JsonInt(*e.promptTokens)); }
		if (e.completionTokens.has_value()) {
			line.put("completion_tokens", JsonInt(*e.completionTokens));
		}
	} else {
		/* ---- 既有工具行：字段与顺序**一个不动**（兼容性硬约束）---- */
		line.put("turn", JsonInt(e.turn));
		line.put("tool", JsonString(e.tool));
		if (e.args.isObj()) {
			line.put("args", e.args);
		} else if (!e.argsRaw.empty()) {
			/* 参数解析失败/非对象：记原始文本（截断，显式标注） */
			std::string raw = e.argsRaw;
			if (raw.size() > kMaxAuditArgsRaw) {
				raw = raw.substr(0, kMaxAuditArgsRaw)
					+ "...(截断，原始 " + std::to_string(e.argsRaw.size()) + " 字节)";
			}
			line.put("args_raw", JsonString(sanitizeUtf8(raw)));
		}
		line.put("ok", JsonBool(e.ok));
		if (!e.error.empty()) { line.put("error", JsonString(e.error)); }
		line.put("ms", JsonInt(e.ms));
		line.put("bytes", JsonInt(static_cast<std::int64_t>(e.bytes)));
		if (e.imageBytes > 0) {
			/* 图片只记体积与指纹：base64 不落盘（体积 + 隐私，见头文件约束） */
			line.put("image_bytes", JsonInt(static_cast<std::int64_t>(e.imageBytes)));
			line.put("image_fp", JsonString(e.imageFp));
		}
		if (e.exitCode.has_value()) { line.put("exit_code", JsonInt(*e.exitCode)); }
		/* G2：工具自报的生效摘要（apply_sql 等）。加法字段，老行没有。 */
		if (!e.summary.empty()) { line.put("summary", JsonString(e.summary)); }
	}

	std::string text = line.toString();
	text.push_back('\n');
	if (toStderr_) {
		std::fputs(text.c_str(), stderr);
		std::fflush(stderr);
		return;
	}
	FILE* f = static_cast<FILE*>(file_);
	if (f == nullptr || std::fputs(text.c_str(), f) == EOF || std::fflush(f) != 0) {
		/* 落盘失败响亮告警一次：审计「看起来在记、其实没记」比没有更糟。
		 * 不抛异常——审计失败不该打断机器人正在执行的动作。 */
		if (!warnBroken_) {
			warnBroken_ = true;
			std::fputs("警告: 审计日志写入失败（磁盘满/权限？），本次会话的后续记录"
				"可能丢失；工具执行不受影响\n", stderr);
		}
	}
}

std::string maskSecretsInText(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	std::size_t i = 0;
	const std::size_t n = text.size();
	auto isSep = [&](char c) {
		return std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'';
	};
	while (i < n) {
		const char c = text[i];
		if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) {
			out += c;
			++i;
			continue;
		}
		std::size_t j = i;
		while (j < n && (std::isalnum(static_cast<unsigned char>(text[j]))
			|| text[j] == '_' || text[j] == '-')) { ++j; }
		const std::string name = toLowerAscii(text.substr(i, j - i));

		/* 形态一：敏感名=值（值到空白/引号为止，整体替换为 ***） */
		if (j < n && text[j] == '=' && isMaskedName(name)) {
			std::size_t k = j + 1;
			while (k < n && !isSep(text[k])) { ++k; }
			out += text.substr(i, j - i);
			out += "=***";
			i = k;
			continue;
		}
		/* 形态二：Bearer 令牌（`Bearer <tok>`；tok 到空白/引号为止）。
		 * 只认这个词本身：Authorization 后面跟 Basic/Token 等其他 scheme 时
		 * 由形态三兜住，这里不重复。 */
		if (name == "bearer") {
			out += text.substr(i, j - i);
			std::size_t m = j;
			while (m < n && std::isspace(static_cast<unsigned char>(text[m]))) {
				out += text[m];
				++m;
			}
			std::size_t p = m;
			while (p < n && !isSep(text[p])) { ++p; }
			out += "***";
			i = p;
			continue;
		}
		/* 形态三：敏感头 `Name: value`（如 Authorization: Basic ...）——
		 * 值若是 "bearer"，随后的令牌交给形态二继续遮（Authorization: Bearer
		 * sk-xxx 会先遮掉 "Bearer"，再由形态二遮掉令牌本身）。 */
		if (isMaskedName(name) && j < n && text[j] == ':') {
			out += text.substr(i, j - i);
			std::size_t m = j;
			while (m < n && (text[m] == ':' || std::isspace(static_cast<unsigned char>(text[m])))) {
				out += text[m];
				++m;
			}
			std::size_t p = m;
			while (p < n && !isSep(text[p])) { ++p; }
			const std::string scheme = toLowerAscii(text.substr(m, p - m));
			out += "***";
			if (scheme == "bearer") {
				std::size_t q = p;
				while (q < n && std::isspace(static_cast<unsigned char>(text[q]))) {
					out += text[q];
					++q;
				}
				std::size_t r = q;
				while (r < n && !isSep(text[r])) { ++r; }
				out += "***";
				i = r;
				continue;
			}
			i = p;
			continue;
		}
		out += text.substr(i, j - i);
		i = j;
	}
	return out;
}

std::optional<long long> extractExitCode(const std::string& text) {
	static const char kPrefix[] = "exit_code=";
	const std::size_t n = sizeof kPrefix - 1;
	if (text.compare(0, n, kPrefix) != 0) { return std::nullopt; }
	std::size_t i = n;
	bool neg = false;
	if (i < text.size() && text[i] == '-') { neg = true; ++i; }
	if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) {
		return std::nullopt;
	}
	long long v = 0;
	while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
		v = v * 10 + (text[i] - '0');
		++i;
	}
	/* 退出码只有 8 位回绕域，越界值说明这不是 run_command 的回执形态 */
	if (v > 255) { return std::nullopt; }
	return neg ? -v : v;
}

std::string fingerprint64(const std::string& s) {
	std::uint64_t h = kFnvBasis;
	for (char c : s) {
		h ^= static_cast<unsigned char>(c);
		h *= kFnvPrime;
	}
	char buf[20];
	std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
	return buf;
}

std::string auditTimestamp() {
	struct timespec ts{};
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tmv{};
	localtime_r(&ts.tv_sec, &tmv);
	const long off = tmv.tm_gmtoff;
	const char sign = off >= 0 ? '+' : '-';
	const long abso = off >= 0 ? off : -off;
	char buf[96];	/* gcc 的 -Wformat-truncation 按「最坏展开」估算（年可达 11 位、
			 * 时区偏移可达 14 位），48 字节会被误判截断；96 一步到位 */
	std::snprintf(buf, sizeof buf,
		"%04d-%02d-%02dT%02d:%02d:%02d.%03ld%c%02ld:%02ld",
		tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
		static_cast<long>(ts.tv_nsec / 1000000),
		sign, abso / 3600, (abso % 3600) / 60);
	return buf;
}

} // namespace agent
} // namespace bot
