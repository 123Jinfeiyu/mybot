/* test_util.cpp —— util.hpp / actionsource.hpp 全局助手的直接锚点（重构计划 1）
 *
 * 此前 util.hpp 的函数只靠调用方间接覆盖。本轮重复收敛（阶段 1）落进来的
 * 几个助手是全局判据/防线，值得有直接单测钉住契约：
 *   1) isSafeIdent：注入名防线（合成 SQL + modules/<表名>.lua 解析）的判据单点
 *   2) DepthLimitGuard：超限抛且不计数、未超限计数对称、nullptr 只计数
 *   3) rangeErrorText：越界文案模板（CLI 与配置文件两路共用）
 *   4) cli::parseKeyValue：--k=v / --k v / 裸 --k 切分与 strictLookahead 两语义
 *   5) readFileCapped：受限读取单点的策略语义（Reject 超限即失败 / Truncate 截断
 *      正常返回）与软链判据（followSymlinks 开关决定是否带 O_NOFOLLOW）
 */

#include <fstream>
#include <string>

#include "fixtures.hpp"
#include "harness.hpp"
#include "util.hpp"

namespace {

void writeText(const std::filesystem::path& p, const std::string& text) {
	std::ofstream f(p, std::ios::binary);
	f << text;
}

} // namespace

/* ---- isSafeIdent：agentmode 预检与 RobotStruct::injectLuaDriver 共用 ---- */

TH_TEST(utils_is_safe_ident_accepts_plain_identifiers) {
	TH_CHECK(bot::isSafeIdent("LlmOpenAI"));
	TH_CHECK(bot::isSafeIdent("llm0"));
	TH_CHECK(bot::isSafeIdent("_private"));
	TH_CHECK(bot::isSafeIdent("a"));		/* 单字符 */
	TH_CHECK(bot::isSafeIdent("a_1"));
}

TH_TEST(utils_is_safe_ident_rejects_injection_shapes) {
	TH_CHECK(!bot::isSafeIdent(""));			/* 空 */
	TH_CHECK(!bot::isSafeIdent("1abc"));		/* 首字符数字 */
	TH_CHECK(!bot::isSafeIdent("a-b"));		/* 连字符 */
	TH_CHECK(!bot::isSafeIdent("a b"));		/* 空格 */
	TH_CHECK(!bot::isSafeIdent("Llm; DROP"));	/* 合成 SQL 注入形态 */
	TH_CHECK(!bot::isSafeIdent("x'y"));		/* 引号 */
	TH_CHECK(!bot::isSafeIdent("a.b"));		/* 点（表名按 modules/<表名>.lua 解析） */
}

/* ---- DepthLimitGuard：检查+计数合一（1-B2）---- */

TH_TEST(utils_depth_limit_guard_counts_symmetrically_under_limit) {
	int depth = 0;
	constexpr int kLimit = 2;
	{
		bot::DepthLimitGuard g(depth, &kLimit, [] { return std::string(); });
		TH_CHECK(depth == 1);
		{
			bot::DepthLimitGuard g2(depth, &kLimit, [] { return std::string(); });
			TH_CHECK(depth == 2);
		}
		TH_CHECK(depth == 1);	/* 析构对称 */
	}
	TH_CHECK(depth == 0);
}

TH_TEST(utils_depth_limit_guard_throws_without_counting_at_limit) {
	int depth = 0;
	constexpr int kLimit = 2;
	depth = kLimit;		/* 已达上限：再进一层即判环 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "circular chain", [&] {
		bot::DepthLimitGuard g(depth, &kLimit,
			[] { return std::string("circular chain detected (limit 2)"); });
		(void)g;
	}());
	TH_CHECK(depth == kLimit);	/* 抛前未 ++（构造失败不跑析构，计数不脏） */
	/* limit = nullptr：只计数不设限（destroy 收集路径语义） */
	{
		bot::DepthLimitGuard g(depth, nullptr, [] { return std::string(); });
		TH_CHECK(depth == kLimit + 1);
	}
	TH_CHECK(depth == kLimit);
}

/* ---- rangeErrorText：越界文案模板（1-B3）---- */

TH_TEST(utils_range_error_text_formats_subject_and_range) {
	TH_CHECK(bot::rangeErrorText("选项 --llm-timeout", 1, 600, "900")
		== "选项 --llm-timeout 需要在 [1, 600] 范围内，实际是 900");
	TH_CHECK(bot::rangeErrorText("配置文件 c 的字段 'timeout'", 1, 600, "1e+300")
		== "配置文件 c 的字段 'timeout' 需要在 [1, 600] 范围内，实际是 1e+300");
}

/* ---- cli::parseKeyValue：argv 选项切分单点（第 10 项）---- */

TH_TEST(utils_cli_parse_key_value_inline_form) {
	char a0[] = "--key=value";
	char* argv[] = {a0};
	int k = 0;
	auto tok = bot::cli::parseKeyValue(k, 1, argv, true);
	TH_CHECK(tok.key == "key");
	TH_CHECK(tok.value == "value");
	TH_CHECK(tok.hasValue);
	TH_CHECK(k == 0);		/* 内联值不消费下一参数 */
}

TH_TEST(utils_cli_parse_key_value_consumes_next_arg) {
	char a0[] = "--out";
	char a1[] = "merged.mt";
	char* argv[] = {a0, a1};
	int k = 0;
	auto tok = bot::cli::parseKeyValue(k, 2, argv, true);
	TH_CHECK(tok.key == "out");
	TH_CHECK(tok.value == "merged.mt");
	TH_CHECK(tok.hasValue);
	TH_CHECK(k == 1);		/* 消费了下一参数 */
}

TH_TEST(utils_cli_parse_key_value_strict_lookahead_rejects_flag_as_value) {
	char a0[] = "--tcp";
	char a1[] = "--tcp-host";
	char* argv[] = {a0, a1};
	/* strict（mybot CLI，审计中-3）：下一参数以 -- 开头不算值 */
	int k = 0;
	auto tok = bot::cli::parseKeyValue(k, 2, argv, true);
	TH_CHECK(tok.key == "tcp");
	TH_CHECK(!tok.hasValue);
	TH_CHECK(k == 0);		/* 不吞 flag */
	/* lenient（mtmerge takeVal 原语义）：只要存在即消费 */
	int k2 = 0;
	auto tok2 = bot::cli::parseKeyValue(k2, 2, argv, false);
	TH_CHECK(tok2.hasValue);
	TH_CHECK(tok2.value == "--tcp-host");
	TH_CHECK(k2 == 1);
}

TH_TEST(utils_cli_parse_key_value_bare_flag_at_end) {
	char a0[] = "--agent";
	char* argv[] = {a0};
	int k = 0;
	auto tok = bot::cli::parseKeyValue(k, 1, argv, true);
	TH_CHECK(tok.key == "agent");
	TH_CHECK(!tok.hasValue);
	TH_CHECK(tok.value.empty());
	TH_CHECK(k == 0);
}

/* ==================== EINTR 重试单点（2026-09-20 重构阶段 0/4.2）====================
 * main.cpp 安装信号处理器时明确不用 SA_RESTART，故 EINTR 是本项目的常态。
 * 契约：retryEintr 只在 errno==EINTR 且返回 -1 时重试，其余结果（含其它 errno
 * 的 -1、0、正值）原样返回，且重试不吞掉调用次数。 */
TH_TEST(retry_eintr_retries_only_on_eintr) {
	int calls = 0;
	int r = bot::retryEintr([&]() -> int {
		++calls;
		if (calls < 3) { errno = EINTR; return -1; }
		return 7;
	});
	TH_CHECK(r == 7);
	TH_CHECK(calls == 3);
}

TH_TEST(retry_eintr_passes_through_other_errors) {
	int calls = 0;
	int r = bot::retryEintr([&]() -> int {
		++calls;
		errno = EAGAIN;
		return -1;
	});
	TH_CHECK(r == -1);
	TH_CHECK(calls == 1);
	TH_CHECK(errno == EAGAIN);
}

TH_TEST(retry_eintr_passes_through_zero_and_positive) {
	int calls = 0;
	TH_CHECK(bot::retryEintr([&]() -> long { ++calls; return 0; }) == 0);
	TH_CHECK(bot::retryEintr([&]() -> long { ++calls; return 42; }) == 42);
	TH_CHECK(calls == 2);
}

/* ---------------- C4：报错回显的转义 + 截断（echoSource 单点） ---------------- */

TH_TEST(echo_source_keeps_a_short_printable_string_verbatim) {
	TH_CHECK(bot::echoSource("abc") == "abc");
	TH_CHECK(bot::echoSource("") == "");
}

TH_TEST(echo_source_truncates_at_the_cap_with_an_ellipsis) {
	const std::string s(bot::kMaxEchoChars + 10, 'A');
	const std::string out = bot::echoSource(s);
	TH_CHECK(out.size() == bot::kMaxEchoChars + 3);
	TH_CHECK(out.compare(0, bot::kMaxEchoChars, std::string(bot::kMaxEchoChars, 'A')) == 0);
	TH_CHECK(out.compare(bot::kMaxEchoChars, 3, "...") == 0);
	/* 恰好到上限时不加省略标记（与旧 substr 口径一致） */
	TH_CHECK(bot::echoSource(std::string(bot::kMaxEchoChars, 'A')).size() == bot::kMaxEchoChars);
}

TH_TEST(echo_source_escapes_control_bytes_instead_of_writing_them) {
	/* ANSI 转义序列不得原样进入 stderr（终端注入面） */
	const std::string esc = "\x1b[31mred\x1b[0m";
	const std::string out = bot::echoSource(esc);
	TH_CHECK(out.find('\x1b') == std::string::npos);
	TH_CHECK(out.find("\\x1b[31mred\\x1b[0m") != std::string::npos);

	/* 其它控制字节与 DEL 同样转义；可打印内容逐字保留 */
	const std::string mixed("a\0b\tc\x7f", 6);
	TH_CHECK(bot::echoSource(mixed) == "a\\x00b\\x09c\\x7f");
}

/* ---- readFileCapped：受限读取单点（B2-3 余部）----
 * 四份实现收敛成一份后，超限语义由 CapPolicy 显式声明。这里钉住两种策略的
 * 分界（Reject 探测 1 字节即失败 / Truncate 读满即停且正常返回）与软链判据
 * （followSymlinks 决定是否带 O_NOFOLLOW）——它们正是原 readHead 与
 * readWholeFileLimited 的行为差异所在。 */
TH_TEST(read_file_capped_reject_fails_only_beyond_cap) {
	fx::TempDir dir("capread");
	const std::filesystem::path exact = dir.path() / "exact.bin";
	const std::filesystem::path over = dir.path() / "over.bin";
	writeText(exact, "1234");
	writeText(over, "12345");

	std::string out;
	TH_CHECK(bot::readFileCapped(exact.string(), 4, bot::CapPolicy::Reject, out)
		== bot::CapReadStatus::OK);
	TH_CHECK(out == "1234");

	/* 恰好 cap 成功、cap+1 失败：边界在「多出的那 1 字节」上 */
	TH_CHECK(bot::readFileCapped(over.string(), 4, bot::CapPolicy::Reject, out)
		== bot::CapReadStatus::TOO_LARGE);
}

TH_TEST(read_file_capped_truncate_stops_at_cap_without_error) {
	fx::TempDir dir("capread_trunc");
	const std::filesystem::path over = dir.path() / "over.bin";
	writeText(over, "1234567890");

	std::string out;
	TH_CHECK(bot::readFileCapped(over.string(), 4, bot::CapPolicy::Truncate, out)
		== bot::CapReadStatus::OK);
	TH_CHECK(out == "1234");	/* 截断是刻意的：提示词预算语义 */
}

TH_TEST(read_file_capped_missing_file_is_open_failed) {
	fx::TempDir dir("capread_missing");
	std::string out;
	TH_CHECK(bot::readFileCapped((dir.path() / "nope.bin").string(), 16,
			bot::CapPolicy::Reject, out) == bot::CapReadStatus::OPEN_FAILED);
	TH_CHECK(out.empty());
}

TH_TEST(read_file_capped_symlink_policy_is_explicit) {
	fx::TempDir dir("capread_link");
	const std::filesystem::path target = dir.path() / "target.bin";
	const std::filesystem::path link = dir.path() / "link.bin";
	writeText(target, "abcdef");
	std::error_code ec;
	std::filesystem::create_symlink(target, link, ec);
	if (ec) { return; }		/* 文件系统不支持软链则跳过（本机/CI 均支持） */

	std::string out;
	/* 缺省（不跟随）：O_NOFOLLOW 拒绝软链——配置/密钥/提示词包路径的防线 */
	TH_CHECK(bot::readFileCapped(link.string(), 16, bot::CapPolicy::Reject, out)
		== bot::CapReadStatus::OPEN_FAILED);
	/* 显式跟随：结构脚本头摘录沿用的旧语义（路径来自 CLI） */
	TH_CHECK(bot::readFileCapped(link.string(), 16, bot::CapPolicy::Reject, out, true)
		== bot::CapReadStatus::OK);
	TH_CHECK(out == "abcdef");
}

/* 薄封装的报错锚点：Reject 的两句文案（grep 守门与测试都锚定它们） */
TH_TEST(read_whole_file_limited_keeps_both_error_messages) {
	fx::TempDir dir("capread_msg");
	const std::filesystem::path over = dir.path() / "over.bin";
	writeText(over, "12345");
	std::string out, err;
	TH_CHECK(!bot::readWholeFileLimited(over.string(), 4, out, err, "提示词文件"));
	TH_CHECK(err.find("超过字节上限 4") != std::string::npos);
	TH_CHECK(!bot::readWholeFileLimited((dir.path() / "nope.bin").string(), 4, out, err));
	TH_CHECK(err.find("无法打开") == 0);
}
