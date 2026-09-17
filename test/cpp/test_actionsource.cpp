/* test_actionsource.cpp —— 动作输入源的信任边界（审计 #2）
 *
 * --tcp / --input / stdin 三条通道共用同一个字节上限包装器。这里钉住：
 *   LimitedReader  第 cap+1 个字节即抛，EOF 不计入配额，未超限时字节原样透传
 *   异常类型       ScriptTooLarge 属 MatrixParseExcept 家族，
 *                  调用方按「脚本解析期错误」统一处理即可拿到原文报错；
 *                  kind 标签区分「动作脚本 / 结构脚本」两类输入（F-02）
 *   配置校验       端口 / 监听地址 / 空闲超时非法时一律返回 nullptr（不占端口、不裸崩）
 */

#include <cstddef>
#include <memory>
#include <string>

#include "actionsource.hpp"
#include "harness.hpp"
#include "matrix.hpp"
#include "util.hpp"

TH_TEST(limited_reader_passes_bytes_under_the_cap) {
	std::unique_ptr<bot::Reader> inner(new bot::MemoryReader("abc"));
	bot::LimitedReader lim(std::move(inner), 3);
	TH_CHECK(lim.read() == 'a');
	TH_CHECK(lim.read() == 'b');
	TH_CHECK(lim.read() == 'c');
	/* 恰好等于上限：读完最后一个字节仍不抛 */
	TH_CHECK(lim.read() == -1);
	TH_CHECK(lim.read() == -1);
}

TH_TEST(limited_reader_throws_one_byte_past_the_cap) {
	std::unique_ptr<bot::Reader> inner(new bot::MemoryReader("abcde"));
	bot::LimitedReader lim(std::move(inner), 3);
	TH_CHECK(lim.read() == 'a');
	TH_CHECK(lim.read() == 'b');
	TH_CHECK(lim.read() == 'c');
	TH_CHECK_THROWS_MSG(bot::ScriptTooLarge, "exceeds the byte limit of 3", lim.read());
	/* 越限后不会「吞掉异常继续放行」：后续每个字节都继续被拒 */
	TH_CHECK_THROWS(bot::ScriptTooLarge, lim.read());
}

TH_TEST(limited_reader_kind_label_distinguishes_script_kinds) {
	/* F-02：同一上限包装器服务 .mt 与 .sql 两类输入，报错首词须能区分类别 */
	std::unique_ptr<bot::Reader> inner(new bot::MemoryReader("abcd"));
	bot::LimitedReader lim(std::move(inner), 2, "structure script");
	TH_CHECK(lim.read() == 'a');
	TH_CHECK(lim.read() == 'b');
	TH_CHECK_THROWS_MSG(bot::ScriptTooLarge,
		"structure script exceeds the byte limit of 2", lim.read());
}

TH_TEST(limited_reader_eof_does_not_consume_quota) {
	/* EOF 反复返回 -1 不得累加计数（TCP 空闲超时后正是这种形态） */
	std::unique_ptr<bot::Reader> inner(new bot::MemoryReader(""));
	bot::LimitedReader lim(std::move(inner), 1);
	for (int k = 0; k < 100; ++k) { TH_CHECK(lim.read() == -1); }
}

TH_TEST(action_script_too_large_is_a_matrix_parse_error) {
	/* 派生关系是「保留原文报错」的前提：调用方按 MatrixParseExcept 处理即可 */
	bot::ScriptTooLarge e(8);
	const mt::MatrixParseExcept& base = e;
	TH_CHECK(std::string(base.what()).find("exceeds the byte limit of 8") != std::string::npos);
	const std::exception& std_e = base;
	TH_CHECK(std::string(std_e.what()) == std::string(e.what()));
}

TH_TEST(too_large_survives_matrix_parser_without_downgrade) {
	/* 端到端一点：把超限流喂给 MatrixParser，报错文本必须仍是原始那句，
	 * 不能被 parse() 的兜底分支折成 "Parse failed at line N"。 */
	const std::string script = "@Action\nrelay0.deg;\n  200;\n  200;\n  200;\n@\n";
	std::unique_ptr<bot::Reader> inner(new bot::MemoryReader(script));
	bot::LimitedReader lim(std::move(inner), 20);
	mt::MatrixParser parser(lim);
	TH_CHECK_THROWS_MSG(mt::MatrixParseExcept, "exceeds the byte limit of 20", parser.parse());
}

TH_TEST(open_action_source_rejects_missing_input_file) {
	bot::CliOptions opts;
	opts.input = "./definitely-not-here-9f3a1c.mt";
	TH_CHECK(bot::openActionSource(opts) == nullptr);
}

TH_TEST(open_action_source_rejects_bad_tcp_port) {
	bot::CliOptions opts;
	for (const char* bad : {"0", "-1", "65536", "abc", ""}) {
		opts = bot::CliOptions{};
		opts.tcp = true;
		opts.tcpPort = bad;
		TH_CHECK(bot::openActionSource(opts) == nullptr);
	}
}

TH_TEST(open_action_source_rejects_bad_tcp_host) {
	bot::CliOptions opts;
	opts.tcp = true;
	opts.tcpPort = "19999";
	for (const char* bad : {"not-an-ip", "localhost", "999.1.1.1", "127.0.0.1:80"}) {
		opts.tcpHost = bad;
		TH_CHECK(bot::openActionSource(opts) == nullptr);
	}
}

TH_TEST(open_action_source_rejects_bad_tcp_idle) {
	/* 空闲超时非法即拒绝：缺省 30s 的兜底不能被 "0" 变成「永不超时」 */
	bot::CliOptions opts;
	opts.tcp = true;
	opts.tcpPort = "19999";
	opts.tcpHost = "127.0.0.1";
	for (const char* bad : {"0", "-5", "86401", "soon"}) {
		opts.tcpIdle = bad;
		TH_CHECK(bot::openActionSource(opts) == nullptr);
	}
}

/* ---------------- M1：恒时比较 safeEquals ---------------- */

TH_TEST(safe_equals_matches_identical_strings) {
	TH_CHECK(bot::safeEquals("", ""));
	TH_CHECK(bot::safeEquals("token", "token"));
	TH_CHECK(bot::safeEquals(std::string("a\0b", 3), std::string("a\0b", 3)));
}

TH_TEST(safe_equals_rejects_mismatched_content) {
	/* 首字节即错与末字节才错都拒绝（恒时语义对结果无差别） */
	TH_CHECK(!bot::safeEquals("token", "Xoken"));
	TH_CHECK(!bot::safeEquals("token", "tokeX"));
	TH_CHECK(!bot::safeEquals("token", "tokEn"));
}

TH_TEST(safe_equals_rejects_length_difference) {
	TH_CHECK(!bot::safeEquals("token", "tokenx"));
	TH_CHECK(!bot::safeEquals("token", "toke"));
	TH_CHECK(!bot::safeEquals("", "x"));
}
