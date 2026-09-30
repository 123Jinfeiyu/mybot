/* test_cross_lang_limits.cpp —— C++ llm_limits ↔ Rust limits 取值区间自动校验
 *
 * 两侧各自声明同一组上下界（llmconfig.hpp 的 llm_limits 与 config/mod.rs 的
 * limits 模块），此前只靠人记对齐——改一侧忘一侧不会出现运行期症状（配置文件
 * 在 C++ 后端接受的值，Rust agent 可能拒收或反过来），只能像
 * mcpbackend_toolresult_schema_matches_rust_outcome（test_mcpbackend.cpp）那样
 * 读源比对钉死。
 *
 * 映射表（2026-09-30 对齐）：
 *   C++ kTimeoutMinSec/MaxSec          ↔ Rust TIMEOUT_MIN_SEC/MAX_SEC
 *   C++ kMaxTokensMin/Max              ↔ Rust MAX_TOKENS_MIN/MAX
 *   C++ kMaxTurnsMin/Max               ↔ Rust MAX_TURNS_MIN/MAX
 *   C++ kTaskTimeoutMinSec/MaxSec      ↔ Rust TASK_TIMEOUT_MIN_SEC/MAX_SEC
 *   C++ kMaxHistoryBytesLimit 等三字节  ↔ Rust BUDGET_BYTES_MAX（三者同值）
 *   C++ kMaxCallsPerRoundMin/Max       ↔ Rust MAX_CALLS_PER_ROUND_MIN/MAX
 *   C++ kMaxRetriesMin/Max             ↔ Rust MAX_RETRIES_MIN/MAX
 *   C++ kRetryBackoffMsMin/Max         ↔ Rust RETRY_BACKOFF_MS_MIN/MAX
 *
 * C++ 侧独有（后端专属，Rust 不镜像）：kRetryBackoffCapMs、kRunCommandTimeout*、
 * kRunCommandMaxTimeout*、kActionScriptTimeout*——这些由后端独自校验，agent
 * 进程不持有这些字段（config/mod.rs 注释明确说明）。
 *
 * Rust 侧独有：SUMMARY_BYTES_MIN/MAX（历史摘要正文区间），C++ 侧无对应量。 */

#include "harness.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "llmconfig.hpp"

namespace {

#ifndef MYBOT_SOURCE_DIR
#define MYBOT_SOURCE_DIR "."
#endif

std::string readRepoSource(const std::string& rel) {
	const std::string path = std::string(MYBOT_SOURCE_DIR) + "/" + rel;
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("契约钉子读不到源文件: " + path); }
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 求常量表达式的值：支持非负整数字面量（含 Rust 下划线分隔符如 200_000）、
 * 位后缀 u/i64、'*'、'+'、'<<' 与空白。解析失败返回 false。 */
bool evalConstExpr(const std::string& raw, long long& out) {
	std::string expr;
	expr.reserve(raw.size());
	for (char c : raw) {
		if (c == '_') { continue; }
		expr.push_back(c);
	}

	std::size_t i = 0;
	const auto skipWs = [&] {
		while (i < expr.size() && (expr[i] == ' ' || expr[i] == '\t'
				|| expr[i] == '\n' || expr[i] == '\r')) { ++i; }
	};
	const auto digits = [&](long long& v) {
		v = 0;
		const std::size_t begin = i;
		while (i < expr.size() && expr[i] >= '0' && expr[i] <= '9') {
			v = v * 10 + (expr[i] - '0');
			++i;
		}
		while (i < expr.size() && (expr[i] == 'u' || expr[i] == 'i'
				|| (expr[i] >= 'a' && expr[i] <= 'z'))) { ++i; }
		return i > begin;
	};
	long long sum = 0;
	skipWs();
	if (!digits(sum)) { return false; }
	for (;;) {
		skipWs();
		if (i + 1 < expr.size() && expr[i] == '<' && expr[i + 1] == '<') {
			i += 2;
			long long shift = 0;
			skipWs();
			if (!digits(shift)) { return false; }
			sum <<= shift;
			continue;
		}
		if (i < expr.size() && expr[i] == '*') {
			++i;
			long long factor = 0;
			skipWs();
			if (!digits(factor)) { return false; }
			sum *= factor;
			continue;
		}
		if (i < expr.size() && expr[i] == '+') {
			++i;
			long long term = 0;
			skipWs();
			if (!digits(term)) { return false; }
			sum += term;
			continue;
		}
		break;
	}
	skipWs();
	if (i != expr.size()) { return false; }
	out = sum;
	return true;
}

/* 取 Rust 源中 "pub const NAME: T = RHS;" 声明的右侧原文 */
std::string rustConstRhs(const std::string& src, const std::string& decl) {
	const std::size_t at = src.find(decl);
	if (at == std::string::npos) { return {}; }
	const std::size_t eq = src.find('=', at);
	const std::size_t semi = src.find(';', eq);
	if (eq == std::string::npos || semi == std::string::npos) { return {}; }
	return src.substr(eq + 1, semi - eq - 1);
}

struct LimitPair {
	const char* rustName;
	long long cppValue;
};

} // namespace

/* ---- 取值区间逐值对齐：C++ llm_limits ↔ Rust limits ---- */
TH_TEST(cross_lang_limits_llm_config_ranges_match) {
	const std::string rs = readRepoSource("agent/src/config/mod.rs");
	const std::size_t modBegin = rs.find("pub mod limits {");
	TH_CHECK_MSG(modBegin != std::string::npos,
		"config/mod.rs 找不到 pub mod limits { 块");
	const std::size_t modEnd = rs.find('}', modBegin);
	TH_CHECK_MSG(modEnd != std::string::npos,
		"config/mod.rs 找不到 limits 模块的结束 }");
	const std::string block = rs.substr(modBegin, modEnd - modBegin);

	const LimitPair pairs[] = {
		{"TIMEOUT_MIN_SEC",		bot::llm_limits::kTimeoutMinSec},
		{"TIMEOUT_MAX_SEC",		bot::llm_limits::kTimeoutMaxSec},
		{"MAX_TOKENS_MIN",		bot::llm_limits::kMaxTokensMin},
		{"MAX_TOKENS_MAX",		bot::llm_limits::kMaxTokensMax},
		{"MAX_TURNS_MIN",		bot::llm_limits::kMaxTurnsMin},
		{"MAX_TURNS_MAX",		bot::llm_limits::kMaxTurnsMax},
		{"TASK_TIMEOUT_MIN_SEC",	bot::llm_limits::kTaskTimeoutMinSec},
		{"TASK_TIMEOUT_MAX_SEC",	bot::llm_limits::kTaskTimeoutMaxSec},
		{"BUDGET_BYTES_MAX",		bot::llm_limits::kMaxHistoryBytesLimit},
		{"MAX_CALLS_PER_ROUND_MIN",	bot::llm_limits::kMaxCallsPerRoundMin},
		{"MAX_CALLS_PER_ROUND_MAX",	bot::llm_limits::kMaxCallsPerRoundMax},
		{"MAX_RETRIES_MIN",		bot::llm_limits::kMaxRetriesMin},
		{"MAX_RETRIES_MAX",		bot::llm_limits::kMaxRetriesMax},
		{"RETRY_BACKOFF_MS_MIN",	bot::llm_limits::kRetryBackoffMsMin},
		{"RETRY_BACKOFF_MS_MAX",	bot::llm_limits::kRetryBackoffMsMax},
	};
	const int n = static_cast<int>(sizeof(pairs) / sizeof(pairs[0]));

	int checked = 0;
	for (int k = 0; k < n; ++k) {
		const std::string decl = std::string("pub const ") + pairs[k].rustName;
		const std::string rhs = rustConstRhs(block, decl);
		TH_CHECK_MSG(!rhs.empty(),
			std::string("limits 模块找不到 ") + decl + " 声明");
		long long val = 0;
		TH_CHECK_MSG(evalConstExpr(rhs, val),
			std::string("无法解析 ") + pairs[k].rustName + " 的值: '" + rhs + "'"
			+ "——可能引入了新语法（负数/位运算/常量引用），需要扩展 evalConstExpr");
		TH_CHECK_MSG(val == pairs[k].cppValue,
			std::string(pairs[k].rustName) + " Rust=" + std::to_string(val)
			+ " C++=" + std::to_string(pairs[k].cppValue)
			+ "——取值区间是跨进程契约，改一侧必须同步另一侧");
		++checked;
	}
	TH_CHECK_MSG(checked == n,
		"应校验 " + std::to_string(n) + " 对常量，实际校验 " + std::to_string(checked)
		+ "——新增共享常量必须同步到 Rust limits 模块并在此登记");

	/* Rust 侧独有常量（SUMMARY_BYTES_*）应在源中存在——若被误删，本用例红 */
	TH_CHECK_MSG(block.find("SUMMARY_BYTES_MIN") != std::string::npos,
		"limits 模块缺少 SUMMARY_BYTES_MIN（Rust 侧独有，不应被误删）");
	TH_CHECK_MSG(block.find("SUMMARY_BYTES_MAX") != std::string::npos,
		"limits 模块缺少 SUMMARY_BYTES_MAX（Rust 侧独有，不应被误删）");
}

/* ---- 三字节上限同源：kMaxHistoryBytesLimit / kMaxTaskToolResultBytesLimit /
 * kMaxTaskImageBytesLimit 在 C++ 侧同值（64 MiB），Rust 侧共用 BUDGET_BYTES_MAX ---- */
TH_TEST(cross_lang_limits_three_byte_caps_share_one_rust_constant) {
	TH_CHECK_MSG(bot::llm_limits::kMaxHistoryBytesLimit
			== bot::llm_limits::kMaxTaskToolResultBytesLimit,
		"kMaxHistoryBytesLimit 与 kMaxTaskToolResultBytesLimit 应同值（Rust 侧共用"
		" BUDGET_BYTES_MAX）");
	TH_CHECK_MSG(bot::llm_limits::kMaxHistoryBytesLimit
			== bot::llm_limits::kMaxTaskImageBytesLimit,
		"kMaxHistoryBytesLimit 与 kMaxTaskImageBytesLimit 应同值（Rust 侧共用"
		" BUDGET_BYTES_MAX）");

	const std::string rs = readRepoSource("agent/src/config/mod.rs");
	const std::size_t modBegin = rs.find("pub mod limits {");
	const std::size_t modEnd = rs.find('}', modBegin);
	const std::string block = rs.substr(modBegin, modEnd - modBegin);

	const std::string rhs = rustConstRhs(block, "pub const BUDGET_BYTES_MAX");
	TH_CHECK_MSG(!rhs.empty(), "limits 模块找不到 BUDGET_BYTES_MAX");
	long long val = 0;
	TH_CHECK_MSG(evalConstExpr(rhs, val) && val == bot::llm_limits::kMaxHistoryBytesLimit,
		"BUDGET_BYTES_MAX（" + rhs + " → " + std::to_string(val)
		+ "）应与 C++ 三字节上限（" + std::to_string(bot::llm_limits::kMaxHistoryBytesLimit)
		+ "）相等——三者共用此常量，改值必须四侧同步");
}
