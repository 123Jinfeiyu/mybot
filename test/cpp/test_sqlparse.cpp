/* test_sqlparse.cpp —— 结构脚本词法/语法分析（SQL4cj 移植）
 *
 * 覆盖两条只在负路径上才显形的契约：
 *   C2  连续分隔符不得走 return next(sr) 尾递归（--tcp 下可被远程输入打爆栈）
 *   引号语义  值里的 "--"、块注释起始符、";"、"," 是数据而非注释或语句边界
 * 以及 Parser 的报错形态：一律「Token <词元>」指出出问题的位置；
 * F5-1 起词元带 1 起行列，报错追加「 (line N, column M)」（既有子串锚点不受影响），
 * 引号态跨越 EOF 报「Unterminated string」并给开引号落点（旧实现静默放行半截串）。
 * 唯一的例外是语句首词元为删除/改写类语句（DROP/DELETE/UPDATE）时报
 * 「Unsupported statement …」——词元级消息在那三句上答不了「是不支持还是写错了」。
 */

#include <cstddef>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "harness.hpp"
#include "sqlparse.hpp"
#include "util_except.hpp"

using sql4cj::Event;
using sql4cj::EventType;
using sql4cj::Token;
using sql4cj::TokenType;
using sql4cj::VarType;

namespace {

std::vector<Event> events(const std::string& text) {
	std::vector<Token> ts = fx::tokens(text);
	sql4cj::Parser parser(ts);
	std::vector<Event> out;
	for (;;) {
		Event e = parser.next();
		if (e.type == EventType::END) { break; }
		out.push_back(std::move(e));
	}
	return out;
}

} // namespace

/* ---------------- Token::parse ---------------- */

TH_TEST(token_keyword_matching_is_ascii_case_insensitive) {
	TH_CHECK(Token::parse("CREATE").type == TokenType::CREATE);
	TH_CHECK(Token::parse("create").type == TokenType::CREATE);
	TH_CHECK(Token::parse("cReAtE").type == TokenType::CREATE);
	TH_CHECK(Token::parse("String").type == TokenType::STRING);
	TH_CHECK(Token::parse("STRING").type == TokenType::STRING);
	TH_CHECK(Token::parse("Number").type == TokenType::NUMBER);
	TH_CHECK(Token::parse("NUMBER").type == TokenType::NUMBER);
	TH_CHECK(Token::parse("nUmBeR").type == TokenType::NUMBER);
	TH_CHECK(Token::parse("VALUES").type == TokenType::VALUES);
	/* 长度不同即不是关键字：不能被前缀匹配误判 */
	TH_CHECK(Token::parse("Strings").type == TokenType::NAME);
	TH_CHECK(Token::parse("Numbers").type == TokenType::NAME);
	TH_CHECK(Token::parse("Createx").type == TokenType::NAME);
	TH_CHECK(Token::parse("Relay").type == TokenType::NAME);
	TH_CHECK(Token::parse("Relay").name == "Relay");
}

TH_TEST(token_name_strips_quotes) {
	TH_CHECK(Token::parse("'r0'").name == "r0");
	TH_CHECK(Token::parse("\"r0\"").name == "r0");
	/* 只有首尾成对的引号才剥；中间引号属数据 */
	TH_CHECK(Token::parse("a'b").name == "a'b");
	/* serialize() 的 NAME 形态带 `name '...'` 前缀，便于日志定位 */
	TH_CHECK(Token::parse("'r0'").serialize() == "name 'r0'");
	TH_CHECK(Token::parse(";").serialize() == ";");
	TH_CHECK(Token::parse("CREATE").serialize() == "CREATE");
}

/* ---------------- Lexer ---------------- */

TH_TEST(lexer_create_table_token_stream) {
	std::vector<Token> ts = fx::tokens("CREATE TABLE Relay ( name String, note String );");
	/* 尾随逗号延后一拍产出：先词元本身，再 COMMA（见 Lexer::next 的 comma_ 挂起） */
	TH_CHECK(fx::fingerprint(ts)
		== "CREATE TABLE name 'Relay' ( name 'name' STRING , name 'note' STRING ) ;");
}

TH_TEST(lexer_insert_token_stream) {
	std::vector<Token> ts = fx::tokens(
		"INSERT INTO Relay ( name, note ) VALUES ( 'r0', 'hello' );");
	TH_CHECK(fx::fingerprint(ts) ==
		"INSERT INTO name 'Relay' ( name 'name' , name 'note' ) VALUES ( name 'r0' , name 'hello' ) ;");
}

TH_TEST(lexer_quoted_dashes_are_data_not_comment) {
	/* 引号内的 "--" 曾被当行注释，吞掉同一值之后的全部词元 */
	std::vector<Token> ts = fx::tokens(
		"INSERT INTO Alias ( name, alias ) VALUES ( 'x', 'a--b' );");
	TH_CHECK(ts.size() == 15);
	TH_CHECK(ts[12].type == TokenType::NAME && ts[12].name == "a--b");
	/* 引号内的 "--" 没有吃掉后面的 ')' 与 ';' */
	TH_CHECK(ts[13].type == TokenType::RGHT);
	TH_CHECK(ts[14].type == TokenType::END);
}

TH_TEST(lexer_quoted_block_comment_marker_is_data) {
	std::vector<Token> ts = fx::tokens("INSERT INTO T ( v ) VALUES ( 'a/*b' );");
	bool found = false;
	for (const Token& t : ts) {
		if (t.type == TokenType::NAME && t.name == "a/*b") { found = true; }
	}
	TH_CHECK(found);
}

TH_TEST(lexer_quoted_semicolon_and_comma_are_data) {
	std::vector<Token> ts = fx::tokens("INSERT INTO T ( v ) VALUES ( 'a;b,c' );");
	bool found = false;
	std::size_t ends = 0;
	for (const Token& t : ts) {
		if (t.type == TokenType::NAME && t.name == "a;b,c") { found = true; }
		if (t.type == TokenType::END) { ++ends; }
	}
	TH_CHECK(found);
	/* 引号里的 ';' 不得提前收尾：整句只应有末尾一个 END */
	TH_CHECK(ends == 1);
}

/* 评估 2026-09-20：括号是分隔符，与空白无关——此前 '(' 只在「token 起始」
 * 有分支、')' 只在「token 非空」有分支，于是紧跟标识符的 '(' 被并进名字，
 * `INSERT INTO T(a,b) VALUES(1,2)` 解析失败（`Token ,`）。SQL 空白不敏感是
 * 常识，仓内夹具恰好一律写成 `T (a, b)` 故从未暴露。以下两条断言在修复前
 * 必失败（前者指纹里出现 `name 'Relay(name'`，后者直接抛异常）。 */
TH_TEST(lexer_parens_without_surrounding_space_are_delimiters) {
	std::vector<Token> ts = fx::tokens("INSERT INTO Relay(name, delay) VALUES('r0', 200);");
	TH_CHECK(fx::fingerprint(ts) ==
		"INSERT INTO name 'Relay' ( name 'name' , name 'delay' ) VALUES ( name 'r0' , name '200' ) ;");
}

TH_TEST(lexer_parens_token_stream_matches_spaced_form) {
	/* 同一句话两种写法（紧凑 / 留白）必须产出逐字节相同的词元流 */
	std::vector<Token> compact = fx::tokens("CREATE TABLE T(a Number);INSERT INTO T(a) VALUES(1);");
	std::vector<Token> spaced = fx::tokens("CREATE TABLE T ( a Number ); INSERT INTO T ( a ) VALUES ( 1 );");
	TH_CHECK(fx::fingerprint(compact) == fx::fingerprint(spaced));
	TH_CHECK(fx::fingerprint(compact) ==
		"CREATE TABLE name 'T' ( name 'a' NUMBER ) ; INSERT INTO name 'T' ( name 'a' ) VALUES ( name '1' ) ;");
}

TH_TEST(lexer_lone_parens_before_space) {
	/* 修复前 `String )` 里的 ')' 会被当成名字（name ')'）；现在与 `String)` 同流 */
	std::vector<Token> ts = fx::tokens("CREATE TABLE T ( a Number );");
	TH_CHECK(ts.size() == 8);
	TH_CHECK(ts[4].type == TokenType::NAME && ts[4].name == "a");
	TH_CHECK(ts[5].type == TokenType::NUMBER);
	TH_CHECK(ts[6].type == TokenType::RGHT);
	TH_CHECK(ts[7].type == TokenType::END);
}

TH_TEST(lexer_quote_state_is_incremental_on_large_values) {
	/* F-02：旧实现每追加一个字符就重扫整个 token（insideQuotes O(长度)），
	 * 100 KB 引号值实测 4.84 s 纯 CPU；1 MB 按平方标度需 ~8 分钟，会把
	 * run_all.sh 的 60 s 单测超时打红。增量引号态后线性通过，
	 * 同时钉住大值逐字节保真。 */
	const std::size_t n = 1u << 20;
	std::string sql = "INSERT INTO T ( v ) VALUES ( '";
	sql.append(n, 'x');
	sql += "' );";
	std::vector<Token> ts = fx::tokens(sql);
	const Token* val = nullptr;
	for (const Token& t : ts) {
		if (t.type == TokenType::NAME && t.name.size() == n) { val = &t; }
	}
	TH_CHECK(val != nullptr);
	if (val != nullptr) {
		TH_CHECK(val->name.front() == 'x' && val->name.back() == 'x');
	}
	TH_CHECK(ts.back().type == TokenType::END);
}

TH_TEST(lexer_quote_state_matches_legacy_semantics) {
	/* 增量状态与旧 insideQuotes 的逐字符等价性回归（三处易错点）：
	 *   1) 引号内的空白是数据，不得断词；
	 *   2) 只有打开引号态的那一种引号字符能闭合——'a"b' 里的 " 是数据；
	 *   3) 引号闭合后同 token 续写（'a'b）仍是一个 NAME，name 只剥首尾引号。 */
	auto nameOf = [](const std::string& text, const std::string& want) {
		for (const Token& t : fx::tokens(text)) {
			if (t.type == TokenType::NAME && t.name == want) { return true; }
		}
		return false;
	};
	TH_CHECK(nameOf("INSERT INTO T ( v ) VALUES ( 'a b c' );", "a b c"));
	TH_CHECK(nameOf("INSERT INTO T ( v ) VALUES ( 'a\"b' );", "a\"b"));
	TH_CHECK(nameOf("INSERT INTO T ( v ) VALUES ( 'a'b );", "a'b"));
	/* SQL 标准 '' 双写转义（#33，2026-09-24）：连续两个同种引号 = 一个字面
	 * 引号。修复前此处为「双写转义不实现、原样保留」（值 it''s）；现按标准
	 * 转义为 it's——本断言已随行为变更同步更新。 */
	TH_CHECK(nameOf("INSERT INTO T ( v ) VALUES ( 'it''s' );", "it's"));
}

TH_TEST(lexer_comments_outside_quotes_are_skipped) {
	std::vector<Token> a = fx::tokens("CREATE TABLE T ( -- 行注释\n name String );");
	std::vector<Token> b = fx::tokens("CREATE TABLE T ( name String );");
	TH_CHECK(fx::fingerprint(a) == fx::fingerprint(b));

	std::vector<Token> c = fx::tokens("CREATE /* 块\n注释 */ TABLE T ( name String );");
	TH_CHECK(fx::fingerprint(c) == fx::fingerprint(b));
}

TH_TEST(lexer_comment_start_flushes_pending_token) {
	/* C3-x：注释起始须先提交挂起 token——"50--c\n60" 曾被静默拼成 "5060"，
	 * 通过 Number 列校验产出错误数值。注释等价于空白，两侧 token 必须分开。 */
	std::vector<Token> a = fx::tokens("INSERT INTO T ( v ) VALUES ( 50--c\n60 );");
	/* 注释等价于空白：基准不带逗号（逗号是分隔符词元，注释不产出词元） */
	std::vector<Token> b = fx::tokens("INSERT INTO T ( v ) VALUES ( 50 60 );");
	TH_CHECK(fx::fingerprint(a) == fx::fingerprint(b));
	/* 块注释同族缺陷同修 */
	std::vector<Token> c = fx::tokens("INSERT INTO T ( v ) VALUES ( 50/*c*/60 );");
	TH_CHECK(fx::fingerprint(c) == fx::fingerprint(b));
}

TH_TEST(lexer_trailing_line_comment_terminates_cleanly) {
	std::vector<Token> ts = fx::tokens("CREATE TABLE T ( name String ); -- 收尾注释\n");
	TH_CHECK(!ts.empty());
	TH_CHECK(ts.back().type == TokenType::END);
	TH_CHECK(ts.back().serialize() == ";");
}

TH_TEST(lexer_repeated_delimiters_do_not_recurse) {
	/* C2 防线：8 个连续逗号 + 分号必须平铺产出，而不是 9 层真实递归 */
	std::vector<Token> ts = fx::tokens(",,,,,,,,;");
	TH_CHECK(ts.size() == 9);
	std::size_t commas = 0;
	for (const Token& t : ts) {
		if (t.type == TokenType::COMMA) { ++commas; }
	}
	TH_CHECK(commas == 8);
	TH_CHECK(ts.back().type == TokenType::END);
	/* F5-1 语义变更：未闭合的引号不再一路吃到 EOF 后剥引号放行（值被无声
	 * 截断），而是报「Unterminated string」并给开引号落点（此处第 1 行第 1 列） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Unterminated string starting at line 1, column 1",
		fx::tokens("'unterminated"));
}

TH_TEST(lexer_statement_end_flag_emits_one_semicolon) {
	/* "a;"：分号挂在词元上延后一拍产出，保证 INSERT ...; 后仍有 END 收尾 */
	std::vector<Token> ts = fx::tokens("name;");
	TH_CHECK(ts.size() == 2);
	TH_CHECK(ts[0].type == TokenType::NAME);
	TH_CHECK(ts[1].type == TokenType::END);
}

/* ---------------- Parser ---------------- */

TH_TEST(parser_create_table_event) {
	std::vector<Event> eves = events("CREATE TABLE Relay ( name String, controller String );");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::CREATE_TABLE);
	TH_CHECK(eves[0].create.tableName == "Relay");
	TH_CHECK(eves[0].create.tableColumns.size() == 2);
	TH_CHECK(eves[0].create.tableColumns[0].name == "name");
	TH_CHECK(eves[0].create.tableColumns[1].name == "controller");
}

TH_TEST(parser_insert_event_pairs_names_with_values) {
	std::vector<Event> eves = events(
		"CREATE TABLE Relay ( name String );\n"
		"INSERT INTO Relay ( name ) VALUES ( 'r0' );\n"
		"INSERT INTO Relay ( name ) VALUES ( 'r1' );\n");
	TH_CHECK(eves.size() == 3);
	TH_CHECK(eves[1].type == EventType::INSERT_TABLE);
	TH_CHECK(eves[1].insert.targetTabName == "Relay");
	TH_CHECK(eves[1].insert.tableRows.size() == 1);
	TH_CHECK(eves[1].insert.tableRows[0][0].columnName == "name");
	TH_CHECK(eves[1].insert.tableRows[0][0].value == "r0");
	TH_CHECK(eves[2].insert.tableRows[0][0].value == "r1");
}

TH_TEST(lexer_doubled_quote_escape_is_literal_quote) {
	/* #33：SQL 标准 '' 双写转义——'it''s' 应产出单个值 "it's"，
	 * 而不是被拆成 'it' 与 's' 两个 token（旧行为：每个同种引号都翻转引号态）。 */
	std::vector<Token> ts = fx::tokens("INSERT INTO T ( v ) VALUES ( 'it''s' );");
	TH_CHECK(fx::fingerprint(ts) ==
		"INSERT INTO name 'T' ( name 'v' ) VALUES ( name 'it's' ) ;");
	/* 双写后的引号是数据：不产生额外 token，整句仍只有一个 END */
	std::size_t ends = 0;
	for (const Token& t : ts) { if (t.type == TokenType::END) { ++ends; } }
	TH_CHECK(ends == 1);
}

TH_TEST(lexer_doubled_quote_empty_and_lone_quote) {
	/* '' = 空串（开即闭，与旧行为一致）；'''' = 一个字面引号（双写转义） */
	std::vector<Token> ts = fx::tokens("INSERT INTO T ( v ) VALUES ( '', '''' );");
	TH_CHECK(fx::fingerprint(ts) ==
		"INSERT INTO name 'T' ( name 'v' ) VALUES ( name '' , name ''' ) ;");
}

TH_TEST(parser_doubled_quote_escape_value) {
	/* #33：Parser 级验证转义值落到事件载荷 */
	std::vector<Event> eves = events(
		"CREATE TABLE Relay ( name String, note String );\n"
		"INSERT INTO Relay ( name, note ) VALUES ( 'r0', 'it''s' );\n");
	TH_CHECK(eves.size() == 2);
	TH_CHECK(eves[1].type == EventType::INSERT_TABLE);
	TH_CHECK(eves[1].insert.tableRows[0][0].value == "r0");
	TH_CHECK(eves[1].insert.tableRows[0][1].value == "it's");
}

TH_TEST(parser_create_table_records_number_column_type) {
	std::vector<Event> eves = events("CREATE TABLE T ( a String, b Number, c Number );");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].create.tableColumns.size() == 3);
	TH_CHECK(eves[0].create.tableColumns[0].type_ == VarType::String_);
	TH_CHECK(eves[0].create.tableColumns[1].type_ == VarType::Number);
	TH_CHECK(eves[0].create.tableColumns[2].type_ == VarType::Number);
}

TH_TEST(parser_number_column_accepts_numeric_values) {
	/* Integer / 小数 / 带符号 / 引号包裹的数值字面量都算数值；
	 * 值仍按文本驻留（本层不做类型转换）。 */
	std::vector<Event> eves = events(
		"CREATE TABLE T ( v Number );\n"
		"INSERT INTO T ( v ) VALUES ( 42 );\n"
		"INSERT INTO T ( v ) VALUES ( -1.5 );\n"
		"INSERT INTO T ( v ) VALUES ( +2e3 );\n"
		"INSERT INTO T ( v ) VALUES ( '7' );\n");
	TH_CHECK(eves.size() == 5);
	TH_CHECK(eves[1].insert.tableRows[0][0].value == "42");
	TH_CHECK(eves[2].insert.tableRows[0][0].value == "-1.5");
	TH_CHECK(eves[3].insert.tableRows[0][0].value == "+2e3");
	TH_CHECK(eves[4].insert.tableRows[0][0].value == "7");
}

TH_TEST(parser_number_column_rejects_non_numeric_value) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "declared Number",
		events("CREATE TABLE T ( v Number );\nINSERT INTO T ( v ) VALUES ( abc );"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not numeric",
		events("CREATE TABLE T ( v Number );\nINSERT INTO T ( v ) VALUES ( 'x1' );"));
	/* 空值与 nan/inf 字面量同样拒绝（与动作矩阵数值语义一致） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not numeric",
		events("CREATE TABLE T ( v Number );\nINSERT INTO T ( v ) VALUES ( '' );"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not numeric",
		events("CREATE TABLE T ( v Number );\nINSERT INTO T ( v ) VALUES ( nan );"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not numeric",
		events("CREATE TABLE T ( v Number );\nINSERT INTO T ( v ) VALUES ( inf );"));
}

TH_TEST(parser_number_check_is_by_column_name_only) {
	/* String 列的自由文本不受影响；Number 校验按列名匹配，不误伤同表其它列 */
	const std::vector<Event> eves = events(
		"CREATE TABLE T ( name String, v Number );\n"
		"INSERT INTO T ( name, v ) VALUES ( abc, 12 );\n");
	TH_CHECK(eves[1].insert.tableRows[0][0].value == "abc");
	TH_CHECK(eves[1].insert.tableRows[0][1].value == "12");

	/* 表未在本脚本中 CREATE 时无从校验（消费方负责「表未定义」报错） */
	const std::vector<Event> other = events("INSERT INTO Other ( v ) VALUES ( abc );");
	TH_CHECK(other[0].insert.tableRows[0][0].value == "abc");
}

TH_TEST(parser_rejects_multi_row_values_tuple) {
	/* 本方言一条 INSERT 只允许一组值：多组必须在解析期报错，
	 * 绝不能静默只取第一组（那会让结构脚本少装硬件而无声）。 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token ,",
		events("INSERT INTO Relay ( name, note ) VALUES ( 'a', 'b' ), ( 'c', 'd' );"));
}

TH_TEST(parser_keywords_do_not_misfire_on_quoted_identifiers) {
	/* UPDATE / DROP / SET 现均为关键字，作为标识符（表名/列名/值）须带引号
	 * （引号在词法层剥，仍是合法 NAME）——裸写的同名标识符按关键字处理，
	 * 这是关键字语言的既有口径（同 from/where/values）。 */
	const std::vector<Event> made = events(
		"CREATE TABLE 'Update' ( 'drop' String, 'set' String );");
	TH_CHECK(made[0].create.tableName == "Update");
	TH_CHECK(made[0].create.tableColumns[0].name == "drop");
	TH_CHECK(made[0].create.tableColumns[1].name == "set");
	const std::vector<Event> ins = events(
		"INSERT INTO 'Update' ( 'drop' ) VALUES ( 'DROP' );");
	TH_CHECK(ins[0].insert.targetTabName == "Update");
	TH_CHECK(ins[0].insert.tableRows[0][0].value == "DROP");
	/* 值位的裸 SET 是关键字冲突：报错点名词元而不是被当成名字吞掉 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token SET",
		events("INSERT INTO T ( a ) VALUES ( SET );"));
}

TH_TEST(parser_reports_the_offending_token) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token ;",
		events(";"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token )",
		events("CREATE TABLE T ( name );"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'x'",
		events("CREATE x TABLE T ( name String );"));
}

TH_TEST(parser_error_carries_token_position) {
	/* F5-1：报错带词元 1 起行列——跨过行注释、块注释后仍须正确（列按字节计，
	 * 测试用 ASCII 注释避免多字节偏移干扰断言）。
	 * 第 3 行块注释收尾后的 CREATE x：x 落在第 3 行第 19 列（前两行被注释态消费）。
	 * 完整形态断言（非子串）：定位段必须是「 (line N, column M)」。 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Token name 'x' (line 3, column 19)",
		events("-- line one comment\n/* block\ncomment */ CREATE x;\n"));
	/* 空词元分隔符：位置即字符落点 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Token SELECT (line 1, column 8)",
		events("CREATE SELECT ;"));
	/* 挂起分隔符（非空词元后的 ';' 挂到下一次 next() 产出）：落点须跟随
	 * 分隔符字符本身，而不是被消费时的读取位置 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Token ; (line 1, column 8)",
		events("INSERT ;"));
	/* 多行脚本：第二行的错误词元行号正确、列从行首重计 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Token ; (line 2, column 1)",
		events("CREATE TABLE T ( name String );\n;\n"));
}

TH_TEST(lexer_reports_unterminated_string_position) {
	/* F5-1：引号态跨越 EOF 必须报错并给出开引号落点——旧实现把 'abc
	 * 剥引号后当名字 abc 静默放行，值被无声截断。开引号在第 2 行第 33 列。 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Unterminated string starting at line 2, column 33",
		events("CREATE TABLE T ( name String );\nINSERT INTO T ( name ) VALUES ( 'abc"));
	/* 双引号同型（" 在第 1 行第 33 列） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Unterminated string starting at line 1, column 33",
		events("CREATE TABLE T ( name String ); \"oops"));
}

TH_TEST(parser_reports_unexpected_end_of_script) {
	/* 截断的结构脚本必须报「脚本意外结束」，而不是拿空事件继续装配 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("CREATE TABLE"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("CREATE TABLE T ( name"));
}

TH_TEST(parser_rejects_param_value_count_mismatch) {
	/* fix-plan 3-4：数量不匹配报错必须带表名与两侧列数，便于定位拼写/漏列 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Insert into table 'T' has 1 column(s) but 2 value(s).",
		events("INSERT INTO T ( name ) VALUES ( 'a', 'b' );"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException,
		"Insert into table 'T' has 2 column(s) but 1 value(s).",
		events("INSERT INTO T ( name, note ) VALUES ( 'a' );"));
}

TH_TEST(parser_next_after_last_event_returns_end) {
	std::vector<Token> ts = fx::tokens("CREATE TABLE T ( name String );");
	sql4cj::Parser parser(ts);
	TH_CHECK(parser.next().type == EventType::CREATE_TABLE);
	/* 幂等：END 之后反复调用仍返回 END（readSQL 的循环靠这个收敛） */
	for (int k = 0; k < 3; ++k) {
		TH_CHECK(parser.next().type == EventType::END);
	}
}

/* ---------------- M-1 深层输入不得栈溢出 ---------------- */

TH_TEST(parser_deep_column_list_is_iterative) {
	/* M-1：列段原为按元素互递归，3 万列即打爆 8MB 栈（Release 实测 SIGSEGV）。
	 * 迭代化后必须在常量栈上解析出完整事件。 */
	const int n = 60000;
	std::string sql = "CREATE TABLE T (";
	for (int k = 0; k < n; ++k) {
		if (k) { sql += ", "; }
		sql += "c" + std::to_string(k) + " String";
	}
	sql += ");";
	std::vector<Event> eves = events(sql);
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::CREATE_TABLE);
	TH_CHECK(static_cast<int>(eves[0].create.tableColumns.size()) == n);
	TH_CHECK(eves[0].create.tableColumns[n - 1].name == "c" + std::to_string(n - 1));
}

TH_TEST(parser_deep_param_list_is_iterative) {
	/* M-1 同款：INSERT 参数/值段原为互递归，8 万参数实测栈溢出。 */
	const int n = 60000;
	std::string sql = "CREATE TABLE T ( name String );\nINSERT INTO T (";
	for (int k = 0; k < n; ++k) {
		if (k) { sql += ", "; }
		sql += "p" + std::to_string(k);
	}
	sql += ") VALUES (";
	for (int k = 0; k < n; ++k) {
		if (k) { sql += ", "; }
		sql += "v" + std::to_string(k);
	}
	sql += ");";
	std::vector<Event> eves = events(sql);
	TH_CHECK(eves.size() == 2);
	TH_CHECK(eves[1].type == EventType::INSERT_TABLE);
	TH_CHECK(eves[1].insert.tableRows.size() == 1);
	TH_CHECK(static_cast<int>(eves[1].insert.tableRows[0].size()) == n);
	TH_CHECK(eves[1].insert.tableRows[0][n - 1].columnName == "p" + std::to_string(n - 1));
	TH_CHECK(eves[1].insert.tableRows[0][n - 1].value == "v" + std::to_string(n - 1));
}

/* ---------------- SELECT（/sql 查询命令）---------------- */

TH_TEST(parser_select_star_from_table_event) {
	std::vector<Event> eves = events("SELECT * FROM Vars;");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::SELECT_TABLE);
	TH_CHECK(eves[0].select.sourceTableName == "Vars");
}

TH_TEST(parser_select_accepts_mixed_case) {
	/* 关键字大小写不敏感（沿用词法既约行为）；* 与 FROM 需空白分隔，
	 * 否则「*」会并进 FROM 前的标识符（* 不是词法分隔符）。
	 * 语句须以 ; 收尾：EOF 处词法产出 UNEXPECTED_END（对 CREATE 同样如此）。 */
	std::vector<Event> a = events("select * from Relay;");
	std::vector<Event> b = events("SeLeCt * FrOm Relay;");
	TH_CHECK(a.size() == 1 && a[0].type == EventType::SELECT_TABLE
		&& a[0].select.sourceTableName == "Relay");
	TH_CHECK(a[0].select.sourceTableName == b[0].select.sourceTableName);
}

TH_TEST(parser_select_can_follow_structure_statements) {
	std::vector<Event> eves = events(
		"CREATE TABLE Vars ( k String );\nINSERT INTO Vars ( k ) VALUES ( 'v0' );\n"
		"SELECT * FROM Vars;");
	TH_CHECK(eves.size() == 3);
	TH_CHECK(eves[2].type == EventType::SELECT_TABLE);
	TH_CHECK(eves[2].select.sourceTableName == "Vars");
}

TH_TEST(parser_select_rejects_non_star_and_missing_parts) {
	/* 只支持 `SELECT * FROM t`：非星号列 / 缺 FROM / 缺表名一律解析期报错 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "SELECT supports only",
		events("SELECT a FROM T;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "name 'T'",
		events("SELECT * T;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("SELECT * FROM"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("SELECT *"));
}

/* ---------------- WHERE 等值条件（2026-09-23）---------------- */

TH_TEST(lexer_where_and_eq_token_stream) {
	/* '=' 是第五个分隔词元（同 ; , ( ) 的「空直返/非空挂起」模式）；
	 * WHERE 是大小写不敏感的关键字。 */
	std::vector<Token> ts = fx::tokens("SELECT * FROM Vars WHERE name = 'v1';");
	TH_CHECK(fx::fingerprint(ts)
		== "SELECT name '*' FROM name 'Vars' WHERE name 'name' = name 'v1' ;");
}

TH_TEST(lexer_eq_compact_form_matches_spaced_form) {
	/* col=value 紧贴写法靠 eq_ 挂起标志切分，与留白写法词元流逐字节相同
	 * （取向同 lexer_parens_token_stream_matches_spaced_form）。 */
	std::vector<Token> compact = fx::tokens("SELECT * FROM T WHERE k='v';");
	std::vector<Token> spaced = fx::tokens("SELECT * FROM T WHERE k = 'v' ;");
	TH_CHECK(fx::fingerprint(compact) == fx::fingerprint(spaced));
	TH_CHECK(fx::fingerprint(compact)
		== "SELECT name '*' FROM name 'T' WHERE name 'k' = name 'v' ;");
}

TH_TEST(lexer_quoted_eq_is_data) {
	/* 引号内的 '=' 是数据（既有规则）：值 'a=b' 仍是一个 NAME 词元 */
	std::vector<Token> ts = fx::tokens("INSERT INTO T ( v ) VALUES ( 'a=b' );");
	bool found = false;
	for (const Token& t : ts) {
		if (t.type == TokenType::NAME && t.name == "a=b") { found = true; }
	}
	TH_CHECK(found);
}

TH_TEST(parser_select_where_event_payload) {
	std::vector<Event> eves = events("SELECT * FROM Servo WHERE name = 's1';");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::SELECT_TABLE);
	TH_CHECK(eves[0].select.sourceTableName == "Servo");
	TH_CHECK(eves[0].select.where.has_value());
	TH_CHECK(eves[0].select.where->conds.size() == 1);
	TH_CHECK(eves[0].select.where->conds[0].column == "name");
	TH_CHECK(eves[0].select.where->conds[0].value == "s1");
	/* 裸值（数字/标识符）与引号值等价：值一律按文本驻留 */
	std::vector<Event> bare = events("SELECT * FROM Servo WHERE channel = 0;");
	TH_CHECK(bare[0].select.where.has_value());
	TH_CHECK(bare[0].select.where->conds[0].column == "channel");
	TH_CHECK(bare[0].select.where->conds[0].value == "0");
}

TH_TEST(parser_select_without_where_has_no_condition) {
	std::vector<Event> eves = events("SELECT * FROM Vars;");
	TH_CHECK(!eves[0].select.where.has_value());
}

TH_TEST(parser_select_where_accepts_mixed_case) {
	std::vector<Event> a = events("select * from T where K = 'v';");
	TH_CHECK(a[0].select.where.has_value());
	TH_CHECK(a[0].select.where->conds[0].column == "K");
	TH_CHECK(a[0].select.where->conds[0].value == "v");
}

TH_TEST(parser_select_where_rejects_malformed_conditions) {
	/* 三个位置（列名/等号/值）共用一句用法级报错，报错锚点稳定 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("SELECT * FROM T WHERE = 'x';"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("SELECT * FROM T WHERE a 'x';"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("SELECT * FROM T WHERE a = ;"));
	/* `WHERE;` 收到的是 END 词元（用法级报错点名它）；EOF 才是 unexpected end */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("SELECT * FROM T WHERE;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("SELECT * FROM T WHERE"));
	/* OR 不在方言：多写词元由 nextEnd 的词元级报错兜住 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'OR'",
		events("SELECT * FROM T WHERE a = 'x' OR b = 'y';"));
	/* AND 后面必须还有一个完整条件——悬空 AND 落到 expect 的脚本结束报错 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("SELECT * FROM T WHERE a = 'x' AND"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("SELECT * FROM T WHERE a = 'x' AND = 'y';"));
}

/* ---------------- AND 多条件（2026-09-23 热删除同步扩展）---------------- */

TH_TEST(lexer_and_is_a_keyword_case_insensitively) {
	TH_CHECK(Token::parse("AND").type == TokenType::AND);
	TH_CHECK(Token::parse("and").type == TokenType::AND);
	TH_CHECK(Token::parse("aNd").type == TokenType::AND);
	TH_CHECK(Token::parse("AND").serialize() == "AND");
	/* 长度不同即不是关键字：前缀匹配不得命中（同 Strings/Numbers 既有判据） */
	TH_CHECK(Token::parse("Andy").type == TokenType::NAME);
	TH_CHECK(Token::parse("AND1").type == TokenType::NAME);
	/* 关键回归：'and' 作为**引用值**时是数据。仓内 144 个 .sql 里唯一的
	 * 两处 `and` 都在引号值内（robots/test/sqlquote.sql），词法引号态在
	 * 分隔符/关键字判定之前消费字符，故把它们变成关键字是安全的——
	 * 本断言就是这条安全性的守门。 */
	std::vector<Token> ts = fx::tokens(
		"INSERT INTO T ( v ) VALUES ( 'tag,with (parens) and space' );");
	bool found = false;
	for (const Token& t : ts) {
		if (t.type == TokenType::NAME && t.name == "tag,with (parens) and space") { found = true; }
	}
	TH_CHECK(found);
}

TH_TEST(parser_delete_where_accepts_multiple_and_conditions) {
	std::vector<Event> eves = events(
		"DELETE FROM Servo WHERE controller = 'dummy-0' AND channel = '6';");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::DELETE_TABLE);
	TH_CHECK(eves[0].del.targetTabName == "Servo");
	TH_CHECK(eves[0].del.where.has_value());
	TH_CHECK(eves[0].del.where->conds.size() == 2);
	TH_CHECK(eves[0].del.where->conds[0].column == "controller");
	TH_CHECK(eves[0].del.where->conds[0].value == "dummy-0");
	TH_CHECK(eves[0].del.where->conds[1].column == "channel");
	TH_CHECK(eves[0].del.where->conds[1].value == "6");
	/* 三个条件（AND 是循环而非二元特例） */
	std::vector<Event> three = events("DELETE FROM T WHERE a = 1 AND b = 2 AND c = 3;");
	TH_CHECK(three[0].del.where->conds.size() == 3);
	TH_CHECK(three[0].del.where->conds[2].column == "c");
	/* 小写 and / 关键字大小写不敏感 */
	std::vector<Event> lower = events("delete from T where a = 1 and b = 2;");
	TH_CHECK(lower[0].type == EventType::DELETE_TABLE);
	TH_CHECK(lower[0].del.targetTabName == "T");
	TH_CHECK(lower[0].del.where->conds.size() == 2);
}

TH_TEST(lexer_and_compact_form_matches_spaced_form) {
	/* AND 两侧不需要留白（既是关键字也是天然分隔符）：两种写法词元流逐字节
	 * 相同，取向同 parens/eq 的紧凑写法用例。 */
	std::vector<Token> compact = fx::tokens("DELETE FROM T WHERE a=1 AND b='x';");
	std::vector<Token> spaced = fx::tokens("DELETE FROM T WHERE a = 1 AND b = 'x' ;");
	TH_CHECK(fx::fingerprint(compact) == fx::fingerprint(spaced));
	TH_CHECK(fx::fingerprint(compact)
		== "DELETE FROM name 'T' WHERE name 'a' = name '1' AND name 'b' = name 'x' ;");
}

TH_TEST(parser_select_accepts_multiple_and_conditions) {
	/* SELECT 与 DELETE 共用条件列表解析：同一条多条件语法在两条语句上都成立 */
	std::vector<Event> eves = events("SELECT * FROM Servo WHERE controller = 'c0' AND channel = 1;");
	TH_CHECK(eves[0].type == EventType::SELECT_TABLE);
	TH_CHECK(eves[0].select.where->conds.size() == 2);
	TH_CHECK(eves[0].select.where->conds[1].column == "channel");
}

TH_TEST(parser_delete_without_where_deletes_all_rows) {
	/* 无条件 = 删全表（SQL 语义）：事件层以「where 无值」表达，不另设标志位 */
	std::vector<Event> eves = events("DELETE FROM Timer;");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::DELETE_TABLE);
	TH_CHECK(eves[0].del.targetTabName == "Timer");
	TH_CHECK(!eves[0].del.where.has_value());
}

TH_TEST(parser_delete_rejects_malformed_statements) {
	/* 缺 FROM / 缺表名 / 悬空条件一律解析期报错，且报错带词元定位 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'T'",
		events("DELETE T WHERE a = 1;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("DELETE FROM"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("DELETE FROM T WHERE;"));
}

/* ---------------- DROP TABLE（2026-09-23 整表撤下）----------------
 * DROP 是 DDL，作用于**整张表**：`DROP TABLE 表名`，**不带 WHERE**（WHERE 是 DML
 * 的行级筛选，加在这里会「长得像在筛行、实际却整表删」）。误写 WHERE 由 nextEnd
 * 的词元级报错兜住。解析层只产出事件，不查表、不碰活体——表存在性与断电/撤注册
 * 在消费方（readSQL / applySql）。本组钉住：词法、事件载荷、报错形态与定位。 */

TH_TEST(lexer_drop_is_a_keyword_case_insensitively) {
	TH_CHECK(Token::parse("DROP").type == TokenType::DROP);
	TH_CHECK(Token::parse("drop").type == TokenType::DROP);
	TH_CHECK(Token::parse("dRoP").type == TokenType::DROP);
	TH_CHECK(Token::parse("DROP").serialize() == "DROP");
	/* 长度不同即不是关键字：前缀匹配不得命中（同 Strings/Numbers 既有判据） */
	TH_CHECK(Token::parse("Dropx").type == TokenType::NAME);
	TH_CHECK(Token::parse("DROPS").type == TokenType::NAME);
}

TH_TEST(lexer_drop_token_stream) {
	std::vector<Token> ts = fx::tokens("DROP TABLE Timer;");
	TH_CHECK(fx::fingerprint(ts) == "DROP TABLE name 'Timer' ;");
	/* 无 FROM、无 WHERE：两个词元（DROP/TABLE）之后直接是表名 */
	std::vector<Token> compact = fx::tokens("DROP TABLE T;");
	TH_CHECK(fx::fingerprint(compact) == "DROP TABLE name 'T' ;");
}

TH_TEST(parser_drop_targets_the_whole_table) {
	std::vector<Event> eves = events("DROP TABLE Timer;");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::DROP_TABLE);
	TH_CHECK(eves[0].drop.targetTabName == "Timer");
}

TH_TEST(parser_drop_accepts_mixed_case) {
	std::vector<Event> a = events("drop table T;");
	TH_CHECK(a.size() == 1);
	TH_CHECK(a[0].type == EventType::DROP_TABLE);
	TH_CHECK(a[0].drop.targetTabName == "T");
}

TH_TEST(parser_drop_rejects_malformed_statements) {
	/* 缺 TABLE / 缺表名一律解析期报错，且报错带词元定位 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'T'",
		events("DROP T;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("DROP TABLE"));
	/* 表名位的 DROP 是关键字冲突（不再是合法 NAME）：报错点名该词元，
	 * 与「写错语法」可区分 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token DROP",
		events("DROP TABLE DROP;"));
}

TH_TEST(parser_drop_rejects_a_where_clause) {
	/* DROP 不带 WHERE（DDL 作用于整张表）：误写的 WHERE 由 nextEnd 词元级报错
	 * 兜住，自带行列定位——而不是被静默当作「守卫条件」接受。 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token WHERE",
		events("DROP TABLE T WHERE a = 1;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token WHERE",
		events("DROP TABLE T where name = 'x';"));
	/* 悬空 WHERE 同型（词元级，不是用法级） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token WHERE",
		events("DROP TABLE T WHERE;"));
	/* 其它尾随词元同族：DROP 之后只能有分号 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'x'",
		events("DROP TABLE T x;"));
}

/* ---------------- UPDATE（2026-09-23 改写行：断电重建）----------------
 * `UPDATE t SET col = value [, ...] [WHERE col = value [AND ...]]`。
 * 解析层只产出事件：语义（命中行按「断电摘除 + 重新装配上电」改写）与边界
 * （被引用拒改、批内独占）都在消费方（readSQL / applySql），本组钉住词法、
 * 事件载荷、WHERE 复用与报错形态。 */

TH_TEST(lexer_update_and_set_are_keywords_case_insensitively) {
	TH_CHECK(Token::parse("UPDATE").type == TokenType::UPDATE);
	TH_CHECK(Token::parse("update").type == TokenType::UPDATE);
	TH_CHECK(Token::parse("UpDaTe").type == TokenType::UPDATE);
	TH_CHECK(Token::parse("UPDATE").serialize() == "UPDATE");
	TH_CHECK(Token::parse("SET").type == TokenType::SET);
	TH_CHECK(Token::parse("set").type == TokenType::SET);
	TH_CHECK(Token::parse("Set").serialize() == "SET");
	/* 长度不同即不是关键字：前缀匹配不得命中（同 DROP 既有判据） */
	TH_CHECK(Token::parse("Updates").type == TokenType::NAME);
	TH_CHECK(Token::parse("sets").type == TokenType::NAME);
}

TH_TEST(parser_update_parses_single_assignment) {
	std::vector<Event> eves = events("UPDATE Timer SET delay = 50;");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::UPDATE_TABLE);
	TH_CHECK(eves[0].update.targetTabName == "Timer");
	TH_CHECK(eves[0].update.assignments.size() == 1);
	TH_CHECK(eves[0].update.assignments[0].columnName == "delay");
	TH_CHECK(eves[0].update.assignments[0].value == "50");
	/* 无 WHERE = 改写全部数据行（事件层以「where 无值」表达，与 DELETE 同口径） */
	TH_CHECK(!eves[0].update.where.has_value());
}

TH_TEST(parser_update_accepts_multiple_assignments_and_where) {
	std::vector<Event> eves = events(
		"UPDATE Servo SET angle = 90, offset = '-4' WHERE name = 's1' AND channel = '0';");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::UPDATE_TABLE);
	TH_CHECK(eves[0].update.assignments.size() == 2);
	TH_CHECK(eves[0].update.assignments[1].columnName == "offset");
	TH_CHECK(eves[0].update.assignments[1].value == "-4");
	TH_CHECK(eves[0].update.where.has_value());
	TH_CHECK(eves[0].update.where->conds.size() == 2);
	TH_CHECK(eves[0].update.where->conds[0].column == "name");
	TH_CHECK(eves[0].update.where->conds[1].value == "0");
	/* 小写 / 关键字大小写不敏感 */
	std::vector<Event> lower = events("update t set a = 1 where b = 2;");
	TH_CHECK(lower[0].type == EventType::UPDATE_TABLE);
	TH_CHECK(lower[0].update.targetTabName == "t");
	TH_CHECK(lower[0].update.where->conds.size() == 1);
	/* 紧贴写法（= 不留白，靠词法器的挂起等号切分）与留白写法词元流一致 */
	std::vector<Token> compact = fx::tokens("UPDATE T SET a=1;");
	std::vector<Token> spaced = fx::tokens("UPDATE T SET a = 1 ;");
	TH_CHECK(fx::fingerprint(compact) == fx::fingerprint(spaced));
	TH_CHECK(fx::fingerprint(compact) == "UPDATE name 'T' SET name 'a' = name '1' ;");
}

TH_TEST(parser_update_validates_number_columns) {
	/* 与 INSERT 同一判据单点（validateNumberValues）：同脚本 CREATE 过的表，
	 * Number 列的 SET 值必须是数值字面量或 $(表达式)；String 列不受限 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is declared Number, but value 'abc'",
		events("CREATE TABLE T ( name String, delay Number );"
		       "UPDATE T SET delay = 'abc' WHERE name = 'x';"));
	std::vector<Event> ok = events(
		"CREATE TABLE T ( name String, delay Number );"
		"UPDATE T SET delay = 50, name = 'x';");
	TH_CHECK(ok[1].update.assignments[0].value == "50");
	/* $(表达式) 合法（语法在此早校验）；表达式值须带引号——词法器把 '(' 当
	 * 分隔符，不带引号会在 '$' 后切开（与 INSERT 的 VALUES 同一既有口径） */
	std::vector<Event> expr = events(
		"CREATE TABLE T ( name String, delay Number );"
		"UPDATE T SET delay = '$(1 + 2)';");
	TH_CHECK(expr[1].update.assignments[0].value == "$(1 + 2)");
	/* 表未在本脚本 CREATE 则无从校验（消费方负责） */
	std::vector<Event> other = events("UPDATE Other SET v = abc;");
	TH_CHECK(other[0].update.assignments[0].value == "abc");
}

TH_TEST(parser_update_rejects_malformed_statements) {
	/* 缺 SET / 缺表名 / 赋值缺等号 / 悬空逗号 / 尾随词元一律解析期报错；
	 * SET 用法级报错与 WHERE 同风格（从「Token ,」反推语法对模型太绕） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token WHERE",
		events("UPDATE T WHERE a = 1;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("UPDATE T SET"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "SET supports only",
		events("UPDATE T SET a 1;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "SET supports only",
		events("UPDATE T SET a = ;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("UPDATE T SET a = 1,"));
	/* 用 AND 连接赋值是错的写法：终止判据只认逗号，AND 留给 nextEnd 词元级报错
	 * （取向同 WHERE 的 OR——「还能写什么」交给既有的定位报错，不在本函数里猜） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token AND",
		events("UPDATE T SET a = 1 AND b = 2;"));
	/* WHERE 的用法级报错与 SELECT/DELETE 同一入口（三语句不会漂移的既有判据） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "WHERE supports only",
		events("UPDATE T SET a = 1 WHERE b;"));
	/* 尾随词元（含误写的 OR——它不是关键字，落成 NAME）由 nextEnd 兜住 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'OR'",
		events("UPDATE T SET a = 1 WHERE b = 2 OR c = 3;"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'x'",
		events("UPDATE T SET a = 1 x;"));
}

/* ---------------- SHOW TABLES（只读：列出全部表）----------------
 * `SHOW TABLES;`——无载荷的只读语句，解析层只认「TABLES 关键字 + 分号」，不带
 * WHERE/LIKE（过滤交给 SELECT * FROM 表 WHERE）。表名清单与数据行数在消费方
 * （agenttools 的 renderShowTables）枚举运行时矩阵得到，本组只钉词法/语法/报错。 */

TH_TEST(lexer_show_and_tables_are_keywords_case_insensitively) {
	TH_CHECK(Token::parse("SHOW").type == TokenType::SHOW);
	TH_CHECK(Token::parse("show").type == TokenType::SHOW);
	TH_CHECK(Token::parse("ShOw").type == TokenType::SHOW);
	TH_CHECK(Token::parse("SHOW").serialize() == "SHOW");
	TH_CHECK(Token::parse("TABLES").type == TokenType::TABLES);
	TH_CHECK(Token::parse("tables").type == TokenType::TABLES);
	TH_CHECK(Token::parse("Tables").type == TokenType::TABLES);
	TH_CHECK(Token::parse("TABLES").serialize() == "TABLES");
	/* 长度不同即不是关键字：前缀匹配不得命中（同 Strings/Dropx 既有判据）。
	 * 关键：TABLES(6) 不会误命中既有关键字 TABLE(5)，二者是不同词元。 */
	TH_CHECK(Token::parse("Showx").type == TokenType::NAME);
	TH_CHECK(Token::parse("SHOWS").type == TokenType::NAME);
	TH_CHECK(Token::parse("Tablesx").type == TokenType::NAME);
	TH_CHECK(Token::parse("TABLE").type == TokenType::TABLE);
}

TH_TEST(lexer_show_tables_token_stream) {
	std::vector<Token> ts = fx::tokens("SHOW TABLES;");
	TH_CHECK(fx::fingerprint(ts) == "SHOW TABLES ;");
}

TH_TEST(parser_show_tables_event) {
	std::vector<Event> eves = events("SHOW TABLES;");
	TH_CHECK(eves.size() == 1);
	TH_CHECK(eves[0].type == EventType::SHOW_TABLES);
	/* 关键字大小写不敏感（沿用词法既约行为） */
	std::vector<Event> lower = events("show tables;");
	TH_CHECK(lower.size() == 1 && lower[0].type == EventType::SHOW_TABLES);
	/* 可跟在结构语句之后（与 SELECT 同一「只读语句可混排」取向） */
	std::vector<Event> seq = events(
		"CREATE TABLE Vars ( k String );\nINSERT INTO Vars ( k ) VALUES ( 'v0' );\n"
		"SHOW TABLES;");
	TH_CHECK(seq.size() == 3);
	TH_CHECK(seq[2].type == EventType::SHOW_TABLES);
}

TH_TEST(parser_show_tables_rejects_malformed) {
	/* 漏 S 的 `SHOW TABLE;`：TABLES 位收到 TABLE 关键字，expectType 词元级报错带定位 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token TABLE",
		events("SHOW TABLE;"));
	/* 缺 TABLES：`SHOW;` 收到 END 词元 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token ;",
		events("SHOW;"));
	/* 缺分号：EOF 处词法产出 UNEXPECTED_END，nextEnd 报脚本结束 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected end of script",
		events("SHOW TABLES"));
	/* 不带 LIKE：尾随词元由 nextEnd 的词元级报错兜住（LIKE 不是关键字，落成 NAME） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Token name 'LIKE'",
		events("SHOW TABLES LIKE 'x';"));
}
