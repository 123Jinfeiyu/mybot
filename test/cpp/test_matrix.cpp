/* test_matrix.cpp —— .mt 矩阵解析器（MatrixParser / MatrixObject）
 *
 * 状态机按字符推进，历史上出过三类无声故障，这里逐条钉住：
 *   C5   indexOfCol 未命中的 -1 隐式转 size_t 回绕 → 越界访问报错无指向性
 *   行号计数  注释行 / @Name 行末换行不计数 → DoesNotSync 报错误导
 *   矩阵名污染  多矩阵之间只有单个换行时，残留 nl_/nm_ 把 '@' 吞进名字
 * 另外固化 "-" 占位符的前向填充语义（首数据行不继承，保留字面值）。
 */

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "harness.hpp"
#include "matrix.hpp"
#include "util.hpp"

TH_TEST(matrix_parses_name_rows_and_cols) {
	mt::MatrixObject m = fx::oneMatrix("@Timer\nname period;\nt0 100;\n@\n");
	TH_CHECK(m.hasName());
	TH_CHECK(m.getName() == "Timer");
	TH_CHECK(m.rows() == 2);
	TH_CHECK(m.cols() == 2);
	TH_CHECK(m.indexOfCol(0, "name") == 0);
	TH_CHECK(m.indexOfCol(0, "period") == 1);
	TH_CHECK(m.indexOfCol(0, "nope") == -1);
	TH_CHECK(fx::cell(m, 0, 1) == "period");
	TH_CHECK(fx::cell(m, 1, 0) == "t0");
	TH_CHECK(fx::cell(m, 1, 1) == "100");
}

TH_TEST(matrix_tabs_and_trailing_spaces_are_separators) {
	/* 制表符对齐、行尾多余空格都不应产生空单元格（.mt 常按列对齐手写） */
	mt::MatrixObject m = fx::oneMatrix("@Action\nrelay0.a\trelay0.b;\n  200   300 ;  \n@\n");
	TH_CHECK(m.cols() == 2);
	TH_CHECK(m.rows() == 2);
	TH_CHECK(fx::cell(m, 0, 0) == "relay0.a");
	TH_CHECK(fx::cell(m, 0, 1) == "relay0.b");
	TH_CHECK(fx::cell(m, 1, 0) == "200");
	TH_CHECK(fx::cell(m, 1, 1) == "300");
}

TH_TEST(matrix_comment_lines_do_not_shift_line_numbers) {
	const std::string text =
		"@Relay\n"
		"# 表头注释\n"
		"name note;\n"
		"# 数据注释\n"
		"r0 hello;\n"
		"r0;\n"
		"@\n";
	TH_CHECK_THROWS_MSG(mt::DoesNotSync, "we got line: 6", fx::oneMatrix(text));
}

TH_TEST(matrix_ragged_row_reports_cell_counts) {
	/* 报错必须自带「这行几格 / 上一行几格」，否则用户只能逐列数空格 */
	TH_CHECK_THROWS_MSG(mt::DoesNotSync, "this row has 1 cell(s), but the row above has 2",
		fx::oneMatrix("@Relay\nname note;\nr0 hello;\nr0;\n@\n"));
	TH_CHECK_THROWS_MSG(mt::DoesNotSync, "this row has 3 cell(s), but the row above has 1",
		fx::oneMatrix("@Relay\nname;\nr0;\na b c;\n@\n"));
}

TH_TEST(matrix_dash_placeholder_forwards_from_previous_row) {
	mt::MatrixObject m = fx::oneMatrix(
		"@Relay\nname note;\nr0 hello;\nr1 -;\nr2 -;\n@\n");
	TH_CHECK(fx::cell(m, 2, 1) == "-");
	TH_CHECK(fx::inh(m, 1, 1) == "hello");
	TH_CHECK(fx::inh(m, 2, 1) == "hello");
	/* 连续多行 "-" 由前向填充一次成型（旧实现是逐格递归，O(n²) 且深栈） */
	TH_CHECK(fx::inh(m, 3, 1) == "hello");
	/* 非占位符原样返回 */
	TH_CHECK(fx::inh(m, 3, 0) == "r2");
}

TH_TEST(matrix_first_data_row_dash_stays_literal) {
	/* 首数据行（row<=1）无可继承，"-" 保留字面值，由调用方决定含义 */
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname note;\nr0 -;\n@\n");
	TH_CHECK(fx::cell(m, 1, 1) == "-");
	TH_CHECK(fx::inh(m, 1, 1) == "-");
}

TH_TEST(matrix_inherited_bounds_and_negative_index) {
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname note;\nr0 hello;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "out of bounds", fx::inh(m, 9, 0));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "out of bounds", fx::cell(m, 0, 9));
	/* C5 防线：负下标必须直说「大概是缺 name 列」，而不是回绕成天文数字下标 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "negative index", fx::cell(m, -1, 0));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "name", fx::cell(m, 1, -1));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "negative index", fx::inh(m, -1, 1));
}

TH_TEST(matrix_sequential_objects_keep_their_names) {
	/* 矩阵之间只有单个换行：第二个矩阵名不得变成 "@Action" */
	std::vector<mt::MatrixObject> all = fx::manyMatrices(
		"@Timer\nname;\nt0;\n@\n"
		"@Action\ntimer0.delay;\n  50;\n@\n");
	TH_CHECK(all.size() == 2);
	TH_CHECK(all[0].getName() == "Timer");
	TH_CHECK(all[1].getName() == "Action");
	TH_CHECK(all[1].rows() == 2);
	TH_CHECK(fx::cell(all[1], 1, 0) == "50");
}

TH_TEST(matrix_crlf_line_endings_parse_identically) {
	std::vector<mt::MatrixObject> all = fx::manyMatrices(
		"@Timer\r\nname;\r\nt0;\r\n@\r\n@Action\r\ntimer0.delay;\r\n  50;\r\n@\r\n");
	TH_CHECK(all.size() == 2);
	/* 单元格以 ';' 收尾，'\r' 落在行首空白位被吞掉，因此数据格不含 \r；
	 * 但 @Name 行没有 ';'，'\r' 会留在名字里——消费方（findMt）一律 trim，
	 * 这里按同一口径断言，而不是要求解析器自己 trim。 */
	TH_CHECK(bot::trim(all[0].getName()) == "Timer");
	TH_CHECK(bot::trim(all[1].getName()) == "Action");
	TH_CHECK(bot::trim(fx::cell(all[1], 0, 0)) == "timer0.delay");
	TH_CHECK(all[1].cols() == 1);
	TH_CHECK(fx::cell(all[1], 1, 0) == "50");
}

TH_TEST(matrix_blank_input_yields_no_object) {
	bot::MemoryReader empty("");
	mt::MatrixParser p1(empty);
	TH_CHECK(!p1.parse().has_value());

	bot::MemoryReader blanks("\n\n# only a comment\n");
	mt::MatrixParser p2(blanks);
	TH_CHECK(!p2.parse().has_value());
}

TH_TEST(matrix_unclosed_object_still_returns_when_named) {
	/* 缺闭合 '@' 但已到 EOF：已解析出的行仍可用（脚本尾部截断时的实际形态） */
	bot::MemoryReader r("@Timer\nname;\nt0;\n");
	mt::MatrixParser p(r);
	auto m = p.parse();
	TH_CHECK(m.has_value());
	if (m) {
		TH_CHECK(m->getName() == "Timer");
		TH_CHECK(m->rows() == 2);
	}
}

TH_TEST(matrix_does_not_sync_survives_parser_rethrow) {
	/* parse() 对 MatrixParseExcept 是原样 rethrow：行号与列数信息不得被降级掉 */
	try {
		fx::oneMatrix("@Relay\nname note;\nr0 hello;\nr0;\n@\n");
		::th::reportFailure("应抛出 DoesNotSync", __FILE__, __LINE__, "实际未抛出");
	} catch (const mt::MatrixParseExcept& e) {
		const std::string msg = e.what();
		TH_CHECK(msg.find("Does Not Sync") != std::string::npos);
		TH_CHECK(msg.find("Parse failed at line") == std::string::npos);
		/* 以 std::exception 身份捕获也必须是同一个对象（派生链完整） */
	}
}
