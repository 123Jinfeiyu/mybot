#pragma once
/* util.hpp —— 全局基础工具（等价 bot4cj 的 Cangjie 基础层）
 * Reader：字节流抽象（等价 InputStream + StringReader，read() 返 -1 即 EOF）
 * 环境变量/BOT4CJ_DEBUG/惰性日志 logD
 */

#include <atomic>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <limits>
#include <unistd.h>

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

/* 多模块路径逐段探测（2026-09-20 重构阶段 2）：在 paths（':' 分段）下按序找
 * 第一个存在的 rel，返回规范化后的完整路径；全部不存在返回 nullopt。
 * 与 Lua 侧 modules/lib/mpath.lua 的 mpath.resolve 同语义，是宿主侧的单点
 *（此前 daemonvar 的 firstExistingModule 与 hw_factory 各自实现，Lua 侧更是
 * 59 个驱动各自用 ":match('[^:]+')" 只取首段——多路径承诺因此对驱动不成立）。 */
inline std::optional<std::string> findExistingModule(const std::string& paths,
		const std::string& rel) {
	for (const std::string& p : splitModulePaths(paths)) {
		std::string cand = p + "/" + rel;
		std::ifstream f(cand, std::ios::binary);
		if (f.good()) { return cand; }
	}
	return std::nullopt;
}

/* 严格整数解析的失败形态：调用方按场景给出人话报错（本项目错误信息惯例） */
enum class IntParse { OK, NOT_INT, OVERFLOW };

/* strtoll + ERANGE + 全串消费；负号等细节语义由调用方按场景裁决。
 * **内嵌 NUL 一律判非法**（2026-09-21 第七轮审计）：strtoll 走 c_str()，在第一个
 * NUL 处停下，而那个 NUL 正是 `*end`——于是"全串消费"判据被满足，尾随垃圾被
 * **静默丢弃**（`90\0junk` 报 OK/90）。NUL 进得来：.mt 词法不把 NUL 当分隔符，
 * .sql 引号态字面追加（见 sqlparse.cpp 与 daemonvar.cpp 的自述）。同族判据见
 * checkRelPath 的 RelPathCheck::NUL 与 luabridge.cpp 的 rejectEmbeddedNul。 */
inline std::pair<IntParse, long long> parseLongStrict(const std::string& s) {
	if (s.find('\0') != std::string::npos) { return {IntParse::NOT_INT, 0}; }
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

/* ---- CLI 超时/上限参数化区间（2026-09-22 F-1/F-2/F-3，原 TD-2 剩余部分）----
 * 三个此前硬编码的运行期数值改为可选 CLI 参数（同款「不传 = 现状缺省值」
 * 零变更契约）。与 llm_limits（llmconfig.hpp）同一惯例：区间常量单点，
 * --help / 校验共用，避免「帮助写一个数、校验用另一个数」的漂移。这三个
 * 值不属于 agent 配置面（daemon/TCP 是全局能力，不限 --agent），故不进
 * llmconfig 的 LlmFileConfig，走普通 CLI 选项。 */
namespace daemon_limits {
/* RobotStruct::deactivate 的二段屏障宽限上限（原写死 kMaxGraceMs=30000）。
 * 下限 0：允许「不给弃管守护额外宽限」（宁可提前进入 destroy 竞态窗口，
 * 也要缩短停机时间——特殊场景下操作者可自行承担）。上限 300000（5 分钟）：
 * 弃管本就是「join 超时已经用尽」的兜底，再长的等待收益不大且会拖慢停机。 */
inline constexpr int kGraceMsMin = 0;
inline constexpr int kGraceMsMax = 300000;
inline constexpr int kGraceMsDefault = 30000;
} // namespace daemon_limits

namespace tcp_limits {
/* --tcp-max-auth-attempts：口令连续失败上限（原写死 kTcpAuthMaxFails=5）。
 * 下限 1：0 会让第一次失败就退出，与「重试」语义矛盾，不如干脆不设口令；
 * 上限 1000：远超任何合理部署，但不放开「无限」——无限重试等于放弃这条
 * 防爆破的护栏，与 max_calls_per_round 等「能力只能收紧」的口径不同源
 * （这里没有安全下界要保护，纯粹是「离谱数值大概率是打错」的常识上限）。 */
inline constexpr int kAuthMaxFailsMin = 1;
inline constexpr int kAuthMaxFailsMax = 1000;
inline constexpr int kAuthMaxFailsDefault = 5;
/* --tcp-total-timeout：连接总时长预算下限（原写死 tcpTotalBudgetSec 里的
 * 600）。生效值语义不变：max(该下限, idleSec*20)。下限 1：允许收紧到
 * 「基本不给慢滴对端机会」；上限 86400（24 小时）与 --tcp-idle 的既有上限
 * 同数量级，超过它的部署诉求应该用文件/TCP 之外的通道，而不是把连接
 * 挂一整天。 */
inline constexpr int kTotalTimeoutMin = 1;
inline constexpr int kTotalTimeoutMax = 86400;
inline constexpr int kTotalTimeoutDefault = 600;
} // namespace tcp_limits

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

/* ---- 相对路径段判据（M2/B2-1 单点，2026-09-20）----
 * 「对相对路径逐 '/' 段判定」此前散落三处：agenttools::resolveActionScriptPath、
 * mtinclude::validateIncludePath、luasandbox 的 hasDotDotComponent——前两者整段
 * 逐字相似，第三份只判 '..'。判据是安全边界（越出目录遍历/脚本目录穿越），
 * 复制即漂移，收敛到此；各调用方保留自己的报错文案（grep 守门锚定不共享）。
 * 只判不抛：返回枚举，错误文案留给调用方。 */
enum class RelPathCheck {
	OK,          /* 无非法段 */
	ABSOLUTE,    /* 以 '/' 开头（拒绝绝对路径） */
	EMPTY_OR_DOT,/* 空段或 '.' 段（含 "a//b"、"a/./b"） */
	DOTDOT,      /* 含 '..' 段（目录穿越） */
	NUL,         /* 含 '\0' */
};
inline RelPathCheck checkRelPath(const std::string& s) {
	if (s.empty()) { return RelPathCheck::EMPTY_OR_DOT; }
	if (s[0] == '/') { return RelPathCheck::ABSOLUTE; }
	if (s.find('\0') != std::string::npos) { return RelPathCheck::NUL; }
	std::size_t b = 0;
	while (b <= s.size()) {
		std::size_t e = s.find('/', b);
		std::size_t len = (e == std::string::npos) ? s.size() - b : e - b;
		if (len == 0 || (len == 1 && s[b] == '.')) { return RelPathCheck::EMPTY_OR_DOT; }
		if (len == 2 && s[b] == '.' && s[b + 1] == '.') { return RelPathCheck::DOTDOT; }
		if (e == std::string::npos) { break; }
		b = e + 1;
	}
	return RelPathCheck::OK;
}
/* '..' 判定（按 '/'-段精确比较，不误伤含 '..' 子串的合法名，如 foo..bar.lua）。
 * luasandbox 专用：这里的判定**不看**绝对前缀——模块路径白名单本身可能以绝对
 * 路径打头，'..' 穿越才是要挡的，故独立全段扫描，不复用 checkRelPath 的
 * ABSOLUTE 短路（否则绝对路径下的 '..' 会漏网）。 */
inline bool hasDotDotComponent(const std::string& s) {
	std::size_t b = 0;
	while (b <= s.size()) {
		std::size_t e = s.find('/', b);
		std::size_t len = (e == std::string::npos) ? s.size() - b : e - b;
		if (len == 2 && s[b] == '.' && s[b + 1] == '.') { return true; }
		if (e == std::string::npos) { break; }
		b = e + 1;
	}
	return false;
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

/* ---- 有限数值解析（M2/B2-2 单点，2026-09-20）----
 * strtod 全串消费 + 拒绝非有限值：four 处此前是同一判据的四份拷贝——
 *   hardware.cpp::parseMtDouble（矩阵单元格）、hardware.cpp Vars 字面量分支、
 *   expr.cpp::parseNumber、jsonparse.cpp 的 Float 分支；
 * 外加 util.hpp::isNumericLiteral（trim 后同判据）。醒目二分：strtod 静默
 * 返 0 + end 全消费是「没吃到数」与「读到 nan/inf 却当成功」的含糊——
 * 判据漂移意味着同一语义下 4 处消息口径各表其是，收敛到此。
 * 返回三态：INVALID=没消费干净/空串；NONFINITE=消费了但 nan/inf。
 * **只回状态**——报错文案与消息取舍留在各调用方（grep 守门锚定原文）。 */
enum class ParseFiniteStatus { OK, INVALID, NONFINITE };
inline ParseFiniteStatus parseFiniteDouble(const std::string& s, double& out) {
	/* 内嵌 NUL 一律判 INVALID：理由与 parseLongStrict 同款——strtod 在第一个 NUL
	 * 处停下而 `*end` 恰为该 NUL，"全串消费"判据会被满足，尾随垃圾静默丢弃。 */
	if (s.find('\0') != std::string::npos) { return ParseFiniteStatus::INVALID; }
	char* end = nullptr;
	out = std::strtod(s.c_str(), &end);
	if (end == s.c_str() || *end != '\0') { return ParseFiniteStatus::INVALID; }
	return std::isfinite(out) ? ParseFiniteStatus::OK : ParseFiniteStatus::NONFINITE;
}

/* 数值字面量判定：strtod 全串消费 + 有限值（拒绝空值/nan/inf）。与动作矩阵
 * 单元格的数值语义一致（parseMtDouble）。供「仅校验、不转换」的两处复用：
 * 结构脚本 Number 列、@Vars 的 value（值仍按文本驻留）。 */
inline bool isNumericLiteral(const std::string& s) {
	std::string v = trim(s);
	double d = 0.0;
	return parseFiniteDouble(v, d) == ParseFiniteStatus::OK;
}

/* ---- double → 整数转换的值域判据（M2/B2-2 余部，2026-09-21）----
 * 同一判据此前两份手写（luabridge 的键/值两条路径、agenttools 的工具参数），
 * 浮点边界恰是最容易抄错的一类常量：
 *   - int64：上界必须取 2^63 而**不是** LLONG_MAX——后者的 double 表示向上
 *     舍入为 2^63，用 `<=` 比较会放行溢出值；故这里是半开区间 [−2^63, 2^63)。
 *   - int：两个端点都能被 double 精确表示，直接取实际端点、闭区间。
 * 判据本身（含有限性）收敛到两个谓词；越界后怎么办（走浮点分支还是报错）
 * 仍由调用方决定，文案不动。 */
inline bool doubleFitsInt64(double d) {
	constexpr double kLo = -9223372036854775808.0;	/* -2^63 */
	constexpr double kHi = 9223372036854775808.0;	/* 2^63（半开区间上界） */
	return std::isfinite(d) && d >= kLo && d < kHi;
}

inline bool doubleFitsInt(double d) {
	constexpr double kLo = static_cast<double>(std::numeric_limits<int>::min());
	constexpr double kHi = static_cast<double>(std::numeric_limits<int>::max());
	return std::isfinite(d) && d >= kLo && d <= kHi;
}

/* 数值 → 文本：%.15g（整数不带小数点、去掉多余零），供 @Vars 表达式结果
 * 回写文本。程序不调用 setlocale，小数点恒为 '.'。 */
inline std::string formatNumber(double v) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.15g", v);
	return std::string(buf);
}

/* ---- 报错回显：用户可控原文的转义 + 截断（单点）----
 * 2026-09-20 审计 C4：把用户可控字节原样拼进 stderr 有两个问题——
 *   ① 体积：一条报错可把「脚本 8 MiB 上限」量级的字节灌进日志（实测矩阵单元格
 *      20 万字节 → stderr 200KB；`--tcp` 远端脚本可达）；
 *   ② 控制字符直写操作者终端：ANSI 转义可改色、移光标、伪造输出。
 * 表达式报错（expr.cpp）此前已按 80 字符截断，但矩阵单元格报错
 * （hardware.cpp 的 parseMtDouble）漏了同一条防线 —— 同一类缺陷、同一份承诺、
 * 两条路径只有一条落地。收敛到本单点后两条路径共用同一判据与文案形态。
 * 不可打印字节（<0x20 或 0x7f）转成 \xNN 可见形式，再按**输入**长度截断并补
 * 省略标记：截断口径与旧的 `substr(0,80) + "..."` 逐字一致，只多出转义。 */
inline constexpr std::size_t kMaxEchoChars = 80;

inline std::string echoSource(const std::string& s) {
	const std::size_t n = s.size() < kMaxEchoChars ? s.size() : kMaxEchoChars;
	std::string out;
	out.reserve(n);
	static const char kHex[] = "0123456789abcdef";
	for (std::size_t i = 0; i < n; ++i) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x20 || c == 0x7f) {
			out += "\\x";
			out += kHex[c >> 4];
			out += kHex[c & 0x0f];
		} else {
			out += static_cast<char>(c);
		}
	}
	if (s.size() > kMaxEchoChars) { out += "..."; }
	return out;
}

/* ---- 日志标记转义（2026-09-22 安全改进；C7 自 agentloop 匿名命名空间挪出）----
 * 工具的返回文本会整块进 verbose 日志，而它可能含 `[FAIL]` / `[PASS]` 这类
 * 行首标记——下游按行解析日志的东西（测试汇总、CI 日志抓取）会把工具输出当成
 * 真的判定行读走。这里只转义**行首**的 `[`，其余位置的方括号原样保留（JSON
 * 数组、`a[0]` 之类的正常内容不该被改写）。 */
inline std::string escapeLogMarkers(const std::string& s) {
	std::string out;
	bool atLineStart = true;
	for (std::size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (atLineStart && c == '[') {
			out += "\\[";	/* 转义行首方括号 */
		} else {
			out += c;
		}
		atLineStart = (c == '\n');
	}
	return out;
}

/* ---- UTF-8 合法性判据 + 消毒（2026-09-21 审计 AG-1 单点）----
 * 为什么必须做：JSON 的字符串**只允许合法 UTF-8**，而请求体里的 `0x80-0xFF`
 * 由两端的编码器原样透传（C++ 侧 Json::toString 与 Lua 侧 modules/lib/json.lua
 * 的 encode_string 都只转义 <0x20、引号与反斜杠，这是「合法多字节 UTF-8 原样
 * 透传」所必需的）。于是**只要有任何一处把非法字节放进消息文本**，请求体就是
 * 一段非法 UTF-8，严格校验的网关（Python/Node 技术栈的绝大多数）直接
 * HTTP 400，而那条消息已经落在历史里、不再属于可回滚的尾部——REPL 会话从此
 * 每次都以同一个字节偏移失败。
 * 非法字节的真实来源不是攻击，是日常：`run_command` 的 stdout/stderr（cat 到
 * 二进制文件、Latin-1 文件名的 ls、非 UTF-8 locale 的日志）、动作脚本首行注释、
 * 结构脚本头。实测（2026-09-21）三条来源都能让第一次请求就 400。
 *
 * 判据按 RFC 3629：拒绝过长编码（C0/C1 开头）、拒绝 UTF-16 代理区
 * （ED A0..BF）、拒绝 > U+10FFFF（F4 90.. 与 F5..FF）。比对严格网关的解码器
 * 更紧：这些序列在 Python/Node 的严格解码下同样非法。 */
struct Utf8Step {
	unsigned len;	/* 合法序列的字节数；0 = 非法 */
	unsigned cp;	/* 已解码码点（仅 len>0 有意义） */
};

inline Utf8Step utf8DecodeAt(const std::string& s, std::size_t i) {
	const unsigned char c0 = static_cast<unsigned char>(s[i]);
	if (c0 < 0x80) { return {1, c0}; }

	/* 首字节 → 续字节数与解码出的**首段**值（用最小合法首字节取值） */
	unsigned need = 0;
	unsigned cp = 0;
	if (c0 >= 0xc2 && c0 <= 0xdf) {
		need = 1;
		cp = c0 & 0x1fu;
	} else if (c0 >= 0xe0 && c0 <= 0xef) {
		need = 2;
		cp = c0 & 0x0fu;
	} else if (c0 >= 0xf0 && c0 <= 0xf4) {
		need = 3;
		cp = c0 & 0x07u;
	} else {
		return {0, 0};	/* 0x80-0xC1（孤立续字节/过长编码首字节）、0xF5-0xFF */
	}
	if (i + need >= s.size()) { return {0, 0}; }	/* 续字节越过末尾：序列被截断 */

	for (unsigned k = 1; k <= need; ++k) {
		const unsigned char c = static_cast<unsigned char>(s[i + k]);
		if ((c & 0xc0u) != 0x80u) { return {0, 0}; }	/* 续字节必须是 10xxxxxx */
		cp = (cp << 6) | (c & 0x3fu);
	}
	/* 过长编码：解码值必须落在该长度对应的最小码点之上 */
	if (need == 2 && cp < 0x800u) { return {0, 0}; }
	if (need == 3 && cp < 0x10000u) { return {0, 0}; }
	if (cp >= 0xd800u && cp <= 0xdfffu) { return {0, 0}; }	/* UTF-16 代理区 */
	if (cp > 0x10ffffu) { return {0, 0}; }
	return {need + 1, cp};
}

inline bool isValidUtf8(const std::string& s) {
	for (std::size_t i = 0; i < s.size();) {
		const Utf8Step st = utf8DecodeAt(s, i);
		if (st.len == 0) { return false; }
		i += st.len;
	}
	return true;
}

inline constexpr const char* kInvalidUtf8Marker = "[! 非法 UTF-8 字节已省略";

/* 消毒：把非法序列替换成 U+FFFD（EF BF BD），保持合法前缀逐字节不变。
 * 连续非法字节折叠成**一个** U+FFFD 并计数——64 个连续坏字节最多产出
 * 「1 个 U+FFFD + 一句说明」，而不是 64 个替换符。折叠是安全的：消毒只发生在
 * 「数据进入模型上下文」的边界，那些字节本就是无法解读的噪声，不是要保真的内容。
 * 热路径（run_command 输出）也必须过这一关：漏一处就等于没做。 */
inline std::string sanitizeUtf8(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	std::size_t i = 0;
	while (i < s.size()) {
		const Utf8Step st = utf8DecodeAt(s, i);
		if (st.len > 0) {
			out.append(s, i, st.len);
			i += st.len;
			continue;
		}
		std::size_t run = 0;
		while (i < s.size() && utf8DecodeAt(s, i).len == 0) {
			++i;
			++run;
		}
		out += "\xef\xbf\xbd";
		if (run > 1) {
			out += kInvalidUtf8Marker;
			out += std::to_string(run);
			out += " 字节]";
		}
	}
	return out;
}

/* ---- 受限读取：策略 + 单点实现（B2-3 余部，2026-09-21）----
 * 收敛前「带上限地读一个文件」有 4 份实现，超限语义分叉：提示词包响亮拒绝，
 * 而配置与密钥文件**静默截断**后交给下游——用户看到的是「不是合法 JSON」
 * 「密钥错误」，真实成因（文件过大）完全丢失，是排障里最难查的一类。
 * 2026-09-20 先把整文件读取统一为 Reject；2026-09-21 把最后一处独立实现
 * （agentmode 的 readHead：提示词预算，截断是刻意的）也并进来，超限语义改由
 * CapPolicy 显式声明，实现只剩这一份。
 *   Reject  —— 读满 cap 后再探测 1 字节，超限即失败（配置/密钥/提示词包）
 *   Truncate—— 读满 cap 即停，正常返回（结构脚本头摘录等提示词预算）
 * 错误文案留在各调用方（grep 守门锚定各处原文），本函数只回状态。
 * followSymlinks=false（缺省）走 O_NOFOLLOW 防线（一步 open，不做
 * lstat→open 的 check-then-use）；Truncate 的调用方传 true 以保持其既有语义
 * （路径来自 CLI，非远程动作源）。注：O_NOFOLLOW 只看最后一段，中间目录软链
 * 仍可被穿过——残余与旧声明相同。 */
enum class CapPolicy { Reject, Truncate };

enum class CapReadStatus {
	OK,           /* 成功（Truncate 时可能已按 cap 截断） */
	TOO_LARGE,    /* 仅 Reject：文件大于 cap */
	OPEN_FAILED,  /* 打不开（含被 O_NOFOLLOW 拒绝的符号链接） */
	READ_FAILED,  /* 读中途出错；已读到的部分保留在 out 里 */
};

inline CapReadStatus readFileCapped(const std::string& path, std::size_t cap, CapPolicy policy,
		std::string& out, bool followSymlinks = false) {
	out.clear();
	int flags = O_RDONLY | O_CLOEXEC;
	if (!followSymlinks) { flags |= O_NOFOLLOW; }
	int fd = ::open(path.c_str(), flags);
	if (fd < 0) { return CapReadStatus::OPEN_FAILED; }
	std::array<char, 8192> buf;
	for (;;) {
		if (out.size() >= cap) {
			if (policy == CapPolicy::Truncate) { break; }
			/* 已读满 cap 后再探测 1 字节即可判定超限（无需读完整个文件） */
			char probe = 0;
			ssize_t n = ::read(fd, &probe, 1);
			if (n > 0) {
				::close(fd);
				return CapReadStatus::TOO_LARGE;
			}
			break;
		}
		std::size_t want = buf.size();
		if (want > cap - out.size()) { want = cap - out.size(); }
		ssize_t n = ::read(fd, buf.data(), want);
		if (n < 0) {
			if (errno == EINTR) { continue; }
			::close(fd);
			return CapReadStatus::READ_FAILED;
		}
		if (n == 0) { break; }
		out.append(buf.data(), static_cast<std::size_t>(n));
	}
	::close(fd);
	return CapReadStatus::OK;
}

/* 整文件受限读取（Reject 策略的薄封装）：保留原有两句报错锚点
 * 「无法打开<what> …」与「<what> … 超过字节上限 N」；
 * what 只影响报错主体名（"文件" / "提示词文件" / "密钥文件"）。 */
inline bool readWholeFileLimited(const std::string& path, std::size_t cap,
		std::string& out, std::string& err, const char* what = "文件") {
	switch (readFileCapped(path, cap, CapPolicy::Reject, out)) {
	case CapReadStatus::OK:
		return true;
	case CapReadStatus::TOO_LARGE:
		err = std::string(what) + " " + path + " 超过字节上限 " + std::to_string(cap);
		break;
	default:
		/* OPEN_FAILED 与 READ_FAILED 同文案：与收敛前实现逐字一致 */
		err = std::string("无法打开") + what + " " + path;
		break;
	}
	return false;
}

/* ---- 受限 FD 读取（M2/B2-3 单点，2026-09-20）----
 * agenttools::readFileLimited 与 mtinclude::readIncludeFile 此前是同一骨架的
 * 两份拷贝（open 三旗 + 8192 循环 + EINTR + 上限），只报错文案不同——
 * 读放大/软链穿越判据漂移即防线不一致，收敛到此。
 * 只读、超限即停；**不抛错**——错误文案留在调用方（grep 守门锚定各处原文）。
 * totalBytes：非空时累计已读字节（mtinclude 的多文件总量预算用）；
 * totalCap 与 totalBytes 同用（0 表示不校验总量）。
 * 注：O_NOFOLLOW 只看最后一段（中间目录软链仍可被穿过）——残余与旧声明相同，
 * 能写 script 目录的进程本就能直接放真文件，不构成增量能力。 */
enum class FdReadStatus {
	OK,              /* 读满成功 */
	SYMLINK,         /* open 被 O_NOFOLLOW 拒绝（符号链接 ELOOP） */
	OPEN_FAILED,     /* open 其他失败 */
	READ_FAILED,     /* read 失败 */
	TOO_LARGE,       /* 超过单文件上限 */
	TOTAL_TOO_LARGE, /* 累计超过总量上限 */
};
inline FdReadStatus readFdNoFollowLimited(const std::string& path, std::size_t cap,
		std::string& out, std::size_t* totalBytes = nullptr, std::size_t totalCap = 0) {
	out.clear();
	int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ELOOP) { return FdReadStatus::SYMLINK; }
		return FdReadStatus::OPEN_FAILED;
	}
	std::array<char, 8192> buf;
	for (;;) {
		ssize_t n = ::read(fd, buf.data(), buf.size());
		if (n < 0) {
			if (errno == EINTR) { continue; }
			::close(fd);
			return FdReadStatus::READ_FAILED;
		}
		if (n == 0) { break; }
		out.append(buf.data(), static_cast<std::size_t>(n));
		if (out.size() > cap) {			/* 单文件上限先判（与旧语义一致） */
			::close(fd);
			return FdReadStatus::TOO_LARGE;
		}
		if (totalBytes) {
			*totalBytes += static_cast<std::size_t>(n);
			if (totalCap > 0 && *totalBytes > totalCap) {
				::close(fd);
				return FdReadStatus::TOTAL_TOO_LARGE;
			}
		}
	}
	::close(fd);
	return FdReadStatus::OK;
}

/* ---- EINTR 重试单点（2026-09-20 重构阶段 4.2）----
 * main.cpp 安装 SIGINT/SIGTERM 处理器时**明确不用 SA_RESTART**
 *（为了让 accept 等阻塞点被中断以便检查停止标志），因此 EINTR 在本项目里是
 * 常态而非异常：可中断系统调用返回 -1/EINTR 的语义是「再试一次」，
 * 既不是「出错」也不是「EOF」。
 * 此前这条语义散落在 3 处，其中 2 处误判为终态：run_command 排空把 EINTR 当
 * EOF（截断子进程输出，并因提前关闭读端给仍在跑的子进程送 SIGPIPE）；
 * waitpid 的 EINTR 被当「已无子进程」（留下僵尸）。收敛到这里，
 * 调用点只表达「我要调什么」。
 * 形状约束：被包装的调用以返回 -1 表示失败、并以 errno 报因。 */
template <typename Fn>
auto retryEintr(Fn&& fn) -> decltype(fn()) {
	for (;;) {
		auto r = fn();
		if (r == -1 && errno == EINTR) { continue; }
		return r;
	}
}

} // namespace bot