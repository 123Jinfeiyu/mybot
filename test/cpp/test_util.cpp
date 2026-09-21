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
	/* 软链创建失败 = 文件系统不支持（tmpfs 挂载参数、某些容器 overlay 等）。
	 * 这里**必须**显式表态（AG-5）：此前是 `if (ec) { return; }`——一条断言都没跑
	 * 就结束，harness 按「无失败」记 [PASS]，于是「软链策略是否真的生效」在守门
	 * 里从来没有人验证过。现在 harness 把零断言判失败，环境不满足就走 TH_SKIP
	 * 并给出原因（守门日志能回答「为什么这条没跑」）。 */
	if (ec) { TH_SKIP("该文件系统不支持创建符号链接，无法验证 O_NOFOLLOW 策略"); }

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

/* ---- UTF-8 判据与消毒（2026-09-21 审计 AG-1）----
 * 这组用例钉的是「什么算非法」这条边界：它比「转义控制字符」更容易写错，
 * 且错了不会本地报错——只会在严格网关那里变成 400（实测就是如此）。 */
namespace {

/* 用显式字节构造串：源码里的 \xff 之类字面量在不同编译器/字面量类型下
 * 语义不同，测试必须逐字节确定。 */
std::string bytes(std::initializer_list<unsigned> bs) {
	std::string s;
	for (unsigned b : bs) { s.push_back(static_cast<char>(b)); }
	return s;
}

} // namespace

TH_TEST(utf8_accepts_ascii_and_boundary_sequences) {
	TH_CHECK(bot::isValidUtf8(""));
	TH_CHECK(bot::isValidUtf8("plain ascii"));
	TH_CHECK(bot::isValidUtf8(bytes({0x00, 0x7f})));		/* 控制字符合法 */
	TH_CHECK(bot::isValidUtf8(bytes({0xc2, 0x80})));	/* U+0080 最短二字节 */
	TH_CHECK(bot::isValidUtf8(bytes({0xdf, 0xbf})));	/* U+07FF */
	TH_CHECK(bot::isValidUtf8(bytes({0xe0, 0xa0, 0x80})));	/* U+0800 最短三字节 */
	TH_CHECK(bot::isValidUtf8(bytes({0xef, 0xbf, 0xbd})));	/* U+FFFD */
	TH_CHECK(bot::isValidUtf8(bytes({0xf0, 0x90, 0x80, 0x80})));	/* U+10000 */
	TH_CHECK(bot::isValidUtf8(bytes({0xf4, 0x8f, 0xbf, 0xbf})));	/* U+10FFFF 上限 */
	TH_CHECK(bot::isValidUtf8("舵机温度"));			/* 真多字节 */
}

TH_TEST(utf8_rejects_every_illegal_class) {
	/* 原始坏字节（AG-1 的复现输入） */
	TH_CHECK(!bot::isValidUtf8(bytes({0xff})));
	TH_CHECK(!bot::isValidUtf8(bytes({0x80})));
	/* 孤立续字节 */
	TH_CHECK(!bot::isValidUtf8(bytes({0x41, 0x80, 0x42})));
	/* 过长编码：C0/C1 开头，以及三/四字节里编码了过小的码点 */
	TH_CHECK(!bot::isValidUtf8(bytes({0xc0, 0xaf})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xc1, 0xbf})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xe0, 0x80, 0xaf})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xf0, 0x80, 0x80, 0xaf})));
	/* UTF-16 代理区（严格解码器一律拒绝） */
	TH_CHECK(!bot::isValidUtf8(bytes({0xed, 0xa0, 0x80})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xed, 0xbf, 0xbf})));
	/* > U+10FFFF */
	TH_CHECK(!bot::isValidUtf8(bytes({0xf4, 0x90, 0x80, 0x80})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xf5, 0x80, 0x80, 0x80})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xfe})));
	/* 序列被截断（含「截断正好落在多字节中间」——cap 截断会真实产生这种输入） */
	TH_CHECK(!bot::isValidUtf8(bytes({0xe4, 0xb8})));
	TH_CHECK(!bot::isValidUtf8(bytes({0xf0, 0x9f, 0x98})));
}

TH_TEST(utf8_sanitize_replaces_illegal_bytes_and_preserves_valid_prefix) {
	const std::string bad = bytes({0x41, 0xff, 0x42});	/* "A\xffB" = AG-1 的复现输入 */
	const std::string fixed = bot::sanitizeUtf8(bad);
	TH_CHECK(fixed == bytes({0x41, 0xef, 0xbf, 0xbd, 0x42}));
	TH_CHECK(bot::isValidUtf8(fixed));

	/* 合法内容逐字节不变（含真多字节与 NUL） */
	const std::string good = std::string("舵机") + bytes({0x00, 0x7f});
	TH_CHECK(bot::sanitizeUtf8(good) == good);
}

TH_TEST(utf8_sanitize_folds_consecutive_illegal_runs) {
	/* 连续坏字节折叠成一个 U+FFFD + 计数说明，而不是 N 个替换符 */
	std::string run;
	for (int i = 0; i < 5; ++i) { run.push_back(static_cast<char>(0xff)); }
	const std::string fixed = bot::sanitizeUtf8(bytes({0x41}) + run + bytes({0x42}));
	TH_CHECK(bot::isValidUtf8(fixed));
	TH_CHECK(fixed.find("[! 非法 UTF-8 字节已省略5 字节]") != std::string::npos);
	/* 折叠后仍以 A/B 收尾，坏字节只占一个替换符 */
	TH_CHECK(fixed.compare(0, 1, "A") == 0);
	TH_CHECK(fixed.back() == 'B');

	/* 单个坏字节不加计数说明（避免噪音） */
	TH_CHECK(bot::sanitizeUtf8(bytes({0xff})) == bytes({0xef, 0xbf, 0xbd}));
}

TH_TEST(utf8_sanitize_output_is_always_valid_and_idempotent) {
	/* 混合：合法多字节 + 各类非法序列 + ASCII */
	std::string mixed = std::string("舵")
		+ bytes({0xff, 0xff, 0x80})
		+ std::string("机")
		+ bytes({0xe0, 0x80, 0xaf})
		+ std::string("温");
	TH_CHECK(!bot::isValidUtf8(mixed));
	const std::string once = bot::sanitizeUtf8(mixed);
	TH_CHECK(bot::isValidUtf8(once));
	TH_CHECK(bot::sanitizeUtf8(once) == once);	/* 幂等 */
	/* 合法多字节内容原样保留（首尾各验一次；别拿 .back() 与多字节首字节比——
	 * 那比的是同一个字符的最后一字节 vs 第一字节，必然不等） */
	TH_CHECK(once.compare(0, 3, "舵") == 0);
	TH_CHECK(once.size() >= 3 && once.compare(once.size() - 3, 3, "温") == 0);
}

TH_TEST(utf8_sanitize_handles_truncated_tail_from_output_cap) {
	/* 64KB 上限正好切在多字节序列中间：消毒后必须合法（否则网关 400） */
	std::string s = std::string(10, 'a') + bytes({0xe4, 0xb8});	/* 半个「中」 */
	TH_CHECK(!bot::isValidUtf8(s));
	TH_CHECK(bot::isValidUtf8(bot::sanitizeUtf8(s)));
}

/* ---- AG-11：`agent:` 作用域过滤器的完备性（必须放在**主套件**里）----
 * 起因：docs/agent.md 推荐的 `./build/mybot_tests agent` 是子串匹配，只命中用例名
 * 里含 "agent" 的那部分——promptpack / llmconfig / cliargs 这些同属 agent 配置面的
 * 用例全都不命中（实测 86/436，而注释写着「proto / driver / tools / loop / config」）。
 * 现在加了显式作用域表（harness.hpp 的 scopeTable），但显式表的代价是**新增测试
 * 文件时容易忘记补一行**——那会让新用例对 `agent:` 隐形，正是本项要消灭的「看不见」。
 *
 * 本用例必须在 mybot_tests 里而不是 harness_selftest 里：骨架自证二进制只注册了
 * 它自己那几个例句用例，看不到主套件的 438 例，作用域完备性在那里无从判断。
 * 放在这里还有一层好处——它本身是一条普通用例，**不可能被静默跳过**：
 * 少登记一个成员、或成员前缀拼错，都会立刻红。 */
TH_TEST(harness_scope_agent_lists_every_member_with_cases) {
	std::size_t n = 0;
	const th::ScopeEntry* table = th::scopeTable(n);
	TH_CHECK_MSG(n > 0, "作用域表为空");

	std::size_t members = 0;
	for (std::size_t i = 0; i < n; ++i) {
		const std::string prefix = table[i].name;
		std::size_t hits = 0;
		for (const auto& c : th::cases()) {
			if (c.name.rfind(prefix, 0) == 0) { ++hits; }
		}
		++members;
		TH_CHECK_MSG(hits > 0, std::string("作用域成员 '") + prefix
			+ "' 没有命中任何用例（拼错了？还是测试文件被重命名了？）");
	}

	/* 作用域整体命中数必须 ≥ 各成员之和的下界（成员数），且明显多于旧的子串语义：
	 * 子串 'agent' 只命中 80 余例，作用域应覆盖 100 例以上（含 llmconfig/promptpack/
	 * cliargs）。这条断言防的是「表在、匹配没生效」这类静默失效。 */
	std::size_t scoped = 0;
	for (const auto& c : th::cases()) {
		if (th::caseInScope(c.name, "agent")) { ++scoped; }
	}
	TH_CHECK_MSG(scoped >= members && scoped >= 100,
		"agent: 作用域命中 " + std::to_string(scoped) + " 例（成员数 "
			+ std::to_string(members) + "），明显偏少——过滤是否失效？");
}

