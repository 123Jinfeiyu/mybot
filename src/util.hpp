#pragma once
/* util.hpp —— 全局基础工具（等价 bot4cj 的 Cangjie 基础层）
 * Reader：字节流抽象（等价 InputStream + StringReader，read() 返 -1 即 EOF）
 * 环境变量/BOT4CJ_DEBUG/惰性日志 logD
 */

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>

namespace bot {

/* 对应 Cangjie IllegalArgumentException */
class IllegalArgumentException : public std::runtime_error {
public:
	explicit IllegalArgumentException(const std::string& m) : std::runtime_error(m) {}
};

/* 字节流抽象：结构/动作脚本统一按字符消费 */
class Reader {
public:
	virtual ~Reader() = default;
	virtual int read() = 0;	/* -1 = EOF，否则 0..255 */
};

/* 内存驻留读取器（stdin 整体读入 / 测试） */
class MemoryReader : public Reader {
	std::string data_;
	std::size_t pos_ = 0;
public:
	explicit MemoryReader(std::string data) : data_(std::move(data)) {}
	int read() override {
		return pos_ < data_.size() ? static_cast<unsigned char>(data_[pos_++]) : -1;
	}
};

inline std::optional<std::string> getEnvVar(const std::string& name) {
	const char* v = std::getenv(name.c_str());
	return v == nullptr ? std::nullopt : std::optional<std::string>(std::string(v));
}

/* 严格整数解析的失败形态：调用方按场景给出人话报错（本项目错误信息惯例） */
enum class IntParse { OK, NOT_INT, OVERFLOW };

/* strtoll + ERANGE + 全串消费；负号等细节语义由调用方按场景裁决 */
inline std::pair<IntParse, long long> parseLongStrict(const std::string& s) {
	char* end = nullptr;
	errno = 0;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0') { return {IntParse::NOT_INT, 0}; }
	if (errno != 0) { return {IntParse::OVERFLOW, 0}; }
	return {IntParse::OK, v};
}

/* D4：信号停止标志——SIGINT/SIGTERM 处理器置位，主流程与阻塞点（TCP accept）
 * 检查后放弃阻塞，让 deactivate 清理链（destroy / 守护停机）正常走完再退出。 */
inline std::atomic<bool>& stopRequested() {
	static std::atomic<bool> flag{false};
	return flag;
}

/* ---- 恒时字符串比较（审计 M1）----
 * std::string 的 == / != 短路：首个不匹配字节即返回，比较耗时与「匹配到
 * 哪一位」相关，--tcp 0.0.0.0 暴露场景下可被计时侧信道逐字节猜令牌。
 * 异或累积全部字节：耗时只取决于长度，与匹配位置无关。
 * 长度不等提前返回：凭据行的长度本就暴露给网络观察者，不是秘密。 */
inline bool safeEquals(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) { return false; }
	unsigned char diff = 0;
	for (std::size_t i = 0; i < a.size(); ++i) {
		diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
	}
	return diff == 0;
}

/* BOT4CJ_DEBUG = 1 时开启 call/build/destroy 详细信息转储 */
class DebugSwitch {
public:
	static bool enabled() {
		static bool en = [] {
			auto v = getEnvVar("BOT4CJ_DEBUG");
			return v.has_value() && *v == "1";
		}();
		return en;
	}
};

/* 惰性调试日志：仅 debug 模式才求值消息体（对应 Cangjie logD(() -> String)） */
inline void logD(const std::function<std::string()>& msg) {
	if (DebugSwitch::enabled()) { std::cerr << msg() << std::endl; }
}

inline std::string trim(const std::string& s) {
	std::size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
	return s.substr(b, e - b);
}

} // namespace bot