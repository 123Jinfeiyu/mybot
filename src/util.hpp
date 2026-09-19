#pragma once
/* util.hpp —— 全局基础工具（等价 bot4cj 的 Cangjie 基础层）
 * Reader：字节流抽象（等价 InputStream + StringReader，read() 返 -1 即 EOF）
 * 环境变量/BOT4CJ_DEBUG/惰性日志 logD
 */

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
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

/* 递归深度 RAII 计数（N-15，收敛此前 hardware/robot 各自的局部 Guard 结构体）：
 * 构造 ++depth，析构 --depth（异常安全）。上限检查与报错文案由调用方决定，
 * 本类只保证「计数与回退不脱节」。求值环（@Vars 互引、$ 引用列回环）在
 * 结构上是无限递归，只能靠计数兜底。 */
class RecursionGuard {
	int& depth_;
public:
	explicit RecursionGuard(int& depth) : depth_(depth) { ++depth_; }
	~RecursionGuard() { --depth_; }
	RecursionGuard(const RecursionGuard&) = delete;
	RecursionGuard& operator=(const RecursionGuard&) = delete;
};

/* 深度上限防线：把「超限检查 + 计数」合并为一个 RAII。RecursionGuard（N-15）
 * 只收敛了计数一半——上限检查仍由调用方手写，检查与计数分离地各写一份，
 * 次序/比较符漂移即 off-by-one。构造时超限即抛 onExceed()，未超限则 ++，
 * 析构 --（异常安全）。
 * limit 传 nullptr = 只计数不设限：用于「自身不判环、但其计数是嵌套调用
 * 防线输入」的路径（MetaProvider 的 destroy 收集——它嵌套触发被引用方的
 * 非销毁收集，外层深度正是内层超限判定的输入）。
 * 上限值与报错文案语义各异（@Vars 表达式环、$ 引用环），仍由调用方给。 */
class DepthLimitGuard {
	int& depth_;
public:
	DepthLimitGuard(int& depth, const int* limit, const std::function<std::string()>& onExceed)
		: depth_(depth) {
		if (limit != nullptr && depth_ >= *limit) { throw IllegalArgumentException(onExceed()); }
		++depth_;
	}
	~DepthLimitGuard() { --depth_; }
	DepthLimitGuard(const DepthLimitGuard&) = delete;
	DepthLimitGuard& operator=(const DepthLimitGuard&) = delete;
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

/* N-19：Lua 驱动模块根目录——BOT4CJ_MODULE_PATH（':' 分段），缺省 ./modules。
 * 此前宿主三处各自读取（luaend 白名单 / daemonvar chunk / robot 驱动脚本），
 * 其中 luaend 还是手工 getenv、空串语义与另两处不一致，收敛单点。 */
inline std::string modulePaths() {
	auto v = getEnvVar("BOT4CJ_MODULE_PATH");
	return (v && !v->empty()) ? *v : "./modules";
}

/* ':' 分段切分模块路径：去空段、剥尾斜杠（审计 #7 同源：fail-closed 白名单
 * 里尾斜杠分段会静默失效），单独 "/" 段保留原样。 */
inline std::vector<std::string> splitModulePaths(const std::string& paths) {
	std::vector<std::string> out;
	std::size_t b = 0;
	while (b <= paths.size()) {
		std::size_t e = paths.find(':', b);
		std::string p = (e == std::string::npos) ? paths.substr(b) : paths.substr(b, e - b);
		if (e == std::string::npos) { b = paths.size() + 1; } else { b = e + 1; }
		while (p.size() > 1 && p.back() == '/') { p.pop_back(); }
		if (!p.empty()) { out.push_back(std::move(p)); }
	}
	return out;
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

/* 整数选项越界报错的公共尾段。主体描述（「选项 --x」/「配置文件 f 的字段 'k'」）
 * 由调用方给，范围区间与实际值的呈现在此单点——CLI（agentmode parseIntOption）
 * 与配置文件（llmconfig wantInt）两条解析路径此前各自拼一份，措辞漂移会让
 * 按文案匹配的守门用例两头维护。 */
inline std::string rangeErrorText(const std::string& subject, long long lo, long long hi,
		const std::string& actual) {
	return subject + " 需要在 [" + std::to_string(lo) + ", " + std::to_string(hi)
		+ "] 范围内，实际是 " + actual;
}

namespace cli {

/* 单个 argv 选项 token 的解析结果。hasValue=false 表示裸 flag——值语义
 * 由调用方裁决（布尔 flag 置位 / 缺值报错），本层不掺和。 */
struct OptionToken {
	std::string key;
	std::string value;
	bool hasValue = false;
};

/* --key=value / --key value / 裸 --key 的切分单点（main.cpp 与 mtmerge.cpp
 * 此前各写一份）。调用方保证 argv[k] 以 "--" 开头：
 * - '=' 内联值优先，key 不含 '='；
 * - 无 '=' 时看 argv[k+1]：strictLookahead=true（mybot CLI，审计中-3）要求
 *   下一参数不以 "--" 开头才算值——裸 --tcp 不得把后续 flag 吞成端口；
 *   false（mtmerge takeVal 原语义）只要存在即消费。消费与否同步推进 k。 */
inline OptionToken parseKeyValue(int& k, int argc, char** argv, bool strictLookahead) {
	OptionToken tok;
	std::string a = argv[k];
	tok.key = a.substr(2);
	auto eq = tok.key.find('=');
	if (eq != std::string::npos) {
		tok.value = tok.key.substr(eq + 1);
		tok.key = tok.key.substr(0, eq);
		tok.hasValue = true;
	} else if (k + 1 < argc
			&& (!strictLookahead || std::string(argv[k + 1]).rfind("--", 0) != 0)) {
		tok.value = argv[++k];
		tok.hasValue = true;
	}
	return tok;
}

} // namespace cli

/* D4：信号停止标志——SIGINT/SIGTERM 处理器置位，主流程与阻塞点（TCP accept）
 * 检查后放弃阻塞，让 deactivate 清理链（destroy / 守护停机）正常走完再退出。
 * C-L1：命名空间级常量初始化（无 __cxa_guard 守卫），信号处理器内访问
 * async-signal-safe——函数局部 static 的首次初始化守卫若被同线程信号打断，
 * 守卫重入可致 abort。保留访问器以稳定调用点。 */
inline std::atomic<bool> g_stopRequested{false};
inline std::atomic<bool>& stopRequested() {
	return g_stopRequested;
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

/* ---- 安全标识符判定（安全判据必须单点）----
 * 注入的驱动表名/变量名会被拼进合成 SQL，且表名还被当成
 * modules/<表名>.lua 去解析——放行引号/分号等价于让命令行参数改写
 * 结构脚本。此前 agentmode 的 CLI 预检与 RobotStruct::injectLuaDriver
 * 的注入防线各自手写同一判据（一度逐字相同）：判据漂移（只放宽一处）
 * 就是防线不一致，故收敛到本单点。 */
inline bool isSafeIdent(const std::string& s) {
	if (s.empty()) { return false; }
	unsigned char first = static_cast<unsigned char>(s[0]);
	if (!(std::isalpha(first) || s[0] == '_')) { return false; }
	for (char c : s) {
		unsigned char u = static_cast<unsigned char>(c);
		if (!(std::isalnum(u) || c == '_')) { return false; }
	}
	return true;
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

/* 数值字面量判定：strtod 全串消费 + 有限值（拒绝空值/nan/inf）。与动作矩阵
 * 单元格的数值语义一致（parseMtDouble）。供「仅校验、不转换」的两处复用：
 * 结构脚本 Number 列、@Vars 的 value（值仍按文本驻留）。 */
inline bool isNumericLiteral(const std::string& s) {
	std::string v = trim(s);
	if (v.empty()) { return false; }
	char* end = nullptr;
	double d = std::strtod(v.c_str(), &end);
	if (end == v.c_str() || *end != '\0') { return false; }
	return std::isfinite(d);
}

/* 数值 → 文本：%.15g（整数不带小数点、去掉多余零），供 @Vars 表达式结果
 * 回写文本。程序不调用 setlocale，小数点恒为 '.'。 */
inline std::string formatNumber(double v) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.15g", v);
	return std::string(buf);
}

} // namespace bot