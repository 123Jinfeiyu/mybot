/* test_robot.cpp —— 结构脚本装配（RobotStruct 的解析与派发前置校验）
 *
 * 只覆盖「不需要 Lua 运行时」的两段：
 *   构造函数 = readSQL（Lexer + Parser + MatrixObject 拼装）
 *   run()    = 动作矩阵读取 + @Alias 注册 + @Action 派发前的列名/设备名校验
 * 这两段的失败都发生在真机资源申请之前，报错必须精确到表名/列名/行号。
 */

#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "harness.hpp"
#include "matrix.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace {

const char* const kBasicSql =
	"/* 结构脚本自检夹具\n"
	"   多行块注释里出现 ; , ( ) 都不应影响解析 */\n"
	"CREATE TABLE Timer (\n"
	"\tname String\n"
	");\n"
	"INSERT INTO Timer (name) VALUES ('timer-0'); -- 行注释\n"
	"\n"
	"CREATE TABLE Relay (\n"
	"\tname       String,\n"
	"\tcontroller String,\n"
	"\tnote       String\n"
	");\n"
	"INSERT INTO Relay (name, controller, note) VALUES ('relay0', 'timer-0', 'hello-relay');\n";

/* 只做结构脚本装配，返回报错文本（无异常返回空串） */
std::string sqlError(const std::string& sql) {
	try {
		bot::MemoryReader reader(sql);
		bot::RobotStruct robot(reader);
	} catch (const std::exception& e) {
		return std::string(e.what());
	}
	return std::string();
}

/* 跑一遍「结构脚本 + 动作脚本」，返回报错文本（无异常返回空串） */
std::string runTexts(const std::string& sql, const std::string& mt) {
	try {
		bot::MemoryReader sr(sql);
		bot::RobotStruct robot(sr);
		bot::MemoryReader mr(mt);
		robot.run(mr);
	} catch (const std::exception& e) {
		return std::string(e.what());
	}
	return std::string();
}

} // namespace

/* ---------------- readSQL ---------------- */

TH_TEST(robot_struct_accepts_a_well_formed_structure_script) {
	/* 含块注释、行注释、制表符缩进、列名对齐空格的真实方言写法 */
	TH_CHECK(sqlError(kBasicSql).empty());
}

TH_TEST(robot_struct_accepts_empty_script) {
	TH_CHECK(sqlError("").empty());
	TH_CHECK(sqlError("-- 只有注释\n").empty());
}

TH_TEST(robot_struct_reports_undefined_insert_target) {
	const std::string msg = sqlError(
		"CREATE TABLE Relay ( name String );\n"
		"INSERT INTO Relays ( name ) VALUES ( 'r0' );\n");
	TH_CHECK(msg.find("Relays") != std::string::npos);
	TH_CHECK(msg.find("table is not defined") != std::string::npos);
}

TH_TEST(robot_struct_reports_insert_before_create) {
	const std::string msg = sqlError("INSERT INTO Relay ( name ) VALUES ( 'r0' );\n");
	TH_CHECK(msg.find("No table created yet") != std::string::npos);
}

TH_TEST(robot_struct_propagates_syntax_errors) {
	const std::string missingType = sqlError("CREATE TABLE Relay ( name );\n");
	TH_CHECK(missingType.find("Token )") != std::string::npos);

	const std::string swapped = sqlError("CREATE RELAY TABLE T ( name String );\n");
	TH_CHECK(swapped.find("Token name 'RELAY'") != std::string::npos);

	const std::string truncated = sqlError("CREATE TABLE Relay ( name String\n");
	TH_CHECK(truncated.find("unexpected end of script") != std::string::npos);

	const std::string stray = sqlError("; CREATE TABLE Relay ( name String );\n");
	TH_CHECK(stray.find("Token ;") != std::string::npos);
}

/* ---------------- C3：INSERT 的落位必须按列名（2026-09-20 审计） ---------------- */

namespace {

/* 只做 SQL → 矩阵对象（不经硬件装配），用于断言 INSERT 落位 */
std::vector<mt::MatrixObject> loadMatrices(const std::string& sql) {
	std::vector<mt::MatrixObject> out;
	bot::MemoryReader reader(sql);
	bot::ScriptLoader::readSQL(reader, out);
	return out;
}

const mt::MatrixObject* matrixNamed(const std::vector<mt::MatrixObject>& ms, const std::string& n) {
	for (const auto& m : ms) {
		if (m.getName() == n) { return &m; }
	}
	return nullptr;
}

/* 显式 size_t 下标：MatrixObject 同时有 size_t 与 int64_t 两个 operator()，
 * 字面量实参会在两个整型转换间产生歧义 */
std::string cell(const mt::MatrixObject& m, std::size_t r, std::size_t c) { return m(r, c); }

} // namespace

/* INSERT 的列名清单只决定「哪个值属于哪一列」；值必须落到建表声明序上。
 * 旧实现把值逐位 appendRow（列名仅参与 Number 校验），于是列序不一致时
 * 值被静默换列——声明 Number 的列拿到字符串，而 Number 校验看的是另一组配对。 */
TH_TEST(insert_values_land_in_the_declared_column_order) {
	auto ms = loadMatrices(
		"CREATE TABLE Timer (\n"
		"	name  String,\n"
		"	delay Number\n"
		");\n"
		"INSERT INTO Timer (delay, name) VALUES (50, 'timer-0');\n");
	const mt::MatrixObject* t = matrixNamed(ms, "Timer");
	TH_CHECK(t != nullptr);
	if (t == nullptr) { return; }
	TH_CHECK(t->rows() == 2);
	TH_CHECK(cell(*t, 1, 0) == "timer-0");	/* name 列：建表序第 0 列 */
	TH_CHECK(cell(*t, 1, 1) == "50");		/* delay 列：建表序第 1 列 */
}

/* 顺序一致时行为逐字不变（绝大多数脚本走这条路径） */
TH_TEST(insert_values_unchanged_when_column_order_matches) {
	auto ms = loadMatrices(
		"CREATE TABLE Timer (\n"
		"	name  String,\n"
		"	delay Number\n"
		");\n"
		"INSERT INTO Timer (name, delay) VALUES ('timer-0', 50);\n");
	const mt::MatrixObject* t = matrixNamed(ms, "Timer");
	TH_CHECK(t != nullptr);
	if (t == nullptr) { return; }
	TH_CHECK(cell(*t, 1, 0) == "timer-0");
	TH_CHECK(cell(*t, 1, 1) == "50");
}

/* 多行 INSERT 逐行按列名落位，互不影响 */
TH_TEST(insert_multiple_rows_each_land_by_column_name) {
	auto ms = loadMatrices(
		"CREATE TABLE T (\n"
		"	name String,\n"
		"	v    Number\n"
		");\n"
		"INSERT INTO T (v, name) VALUES (1, 'a');\n"
		"INSERT INTO T (name, v) VALUES ('b', 2);\n");
	const mt::MatrixObject* t = matrixNamed(ms, "T");
	TH_CHECK(t != nullptr);
	if (t == nullptr) { return; }
	TH_CHECK(t->rows() == 3);
	TH_CHECK(cell(*t, 1, 0) == "a" && cell(*t, 1, 1) == "1");
	TH_CHECK(cell(*t, 2, 0) == "b" && cell(*t, 2, 1) == "2");
}

/* 未知列名此前被静默顺序落位（并挤掉后续列），现显式报错 */
TH_TEST(insert_rejects_an_unknown_column_name) {
	const std::string msg = sqlError(
		"CREATE TABLE T ( name String );\n"
		"INSERT INTO T (nome) VALUES ('x');\n");
	TH_CHECK(msg.find("no such column") != std::string::npos);
	TH_CHECK(msg.find("nome") != std::string::npos);
}

/* 漏列此前会产生比表头短的行（错位），现显式报错 */
TH_TEST(insert_rejects_a_missing_column) {
	const std::string msg = sqlError(
		"CREATE TABLE T ( name String, v Number );\n"
		"INSERT INTO T (name) VALUES ('x');\n");
	TH_CHECK(msg.find("missing value for column") != std::string::npos);
	TH_CHECK(msg.find("v") != std::string::npos);
}

/* 同列名写两次此前是「后者静默覆盖」的位置关系，现显式报错 */
TH_TEST(insert_rejects_a_duplicated_column) {
	const std::string msg = sqlError(
		"CREATE TABLE T ( name String, v Number );\n"
		"INSERT INTO T (name, name) VALUES ('x', 'y');\n");
	TH_CHECK(msg.find("listed twice") != std::string::npos);
}

/* ---------------- run(): 动作矩阵与派发前置校验 ---------------- */

TH_TEST(run_requires_an_action_matrix) {
	const std::string msg = runTexts(kBasicSql, "@Alias\nname alias;\nx duty;\n@\n");
	TH_CHECK(msg.find("No @Action matrix found") != std::string::npos);
}

TH_TEST(run_reports_undeclared_device_in_action) {
	/* 列头写了没声明的设备名：必须点名是哪个设备、第几行 */
	const std::string msg = runTexts(kBasicSql, "@Action\nrelay9.deg;\n  200;\n@\n");
	TH_CHECK(msg.find("'relay9'") != std::string::npos);
	TH_CHECK(msg.find("not defined in your hardware declaration section") != std::string::npos);
	TH_CHECK(msg.find("row 1") != std::string::npos);
}

TH_TEST(run_rejects_action_column_without_device_dot_param) {
	const std::string msg = runTexts(kBasicSql, "@Action\nrelay0;\n  200;\n@\n");
	TH_CHECK(msg.find("expected format: <device>.<param>") != std::string::npos);
	TH_CHECK(msg.find("Action column 'relay0'") != std::string::npos);

	const std::string msg2 = runTexts(kBasicSql, "@Action\nrelay0.a.b;\n  200;\n@\n");
	TH_CHECK(msg2.find("relay0.a.b") != std::string::npos);
}

TH_TEST(run_rejects_non_numeric_action_cell) {
	const std::string msg = runTexts(kBasicSql, "@Action\nrelay0.deg;\n  abc;\n@\n");
	TH_CHECK(msg.find("cannot parse matrix cell 'abc'") != std::string::npos);
}

/* C4（2026-09-20 审计）：单元格报错必须转义 + 截断用户可控原文。
 * 修复前原样回显——20 万字节单元格实测灌 200KB 进 stderr，且 ANSI 转义
 * 直写操作者终端（表达式报错早有同一条防线，这里漏了）。 */
TH_TEST(run_truncates_a_huge_non_numeric_action_cell) {
	const std::string msg =
		runTexts(kBasicSql, "@Action\nrelay0.deg;\n  " + std::string(200000, 'A') + ";\n@\n");
	TH_CHECK(!msg.empty());
	/* 报错整体必须远小于被回显的原文（截断生效） */
	TH_CHECK(msg.size() < 4096);
	TH_CHECK(msg.find("cannot parse matrix cell") != std::string::npos);
	TH_CHECK(msg.find("...") != std::string::npos);		/* 截断标记 */
}

/* ANSI/控制字节转成可见的 \xNN，不再直写终端 */
TH_TEST(run_escapes_control_bytes_in_a_rejected_action_cell) {
	const std::string msg =
		runTexts(kBasicSql, "@Action\nrelay0.deg;\n  \x1b[31mX\x1b[0m;\n@\n");
	TH_CHECK(msg.find("cannot parse matrix cell") != std::string::npos);
	TH_CHECK(msg.find("\\x1b") != std::string::npos);
	/* 原始 ESC 字节不得出现在报错里 */
	TH_CHECK(msg.find('\x1b') == std::string::npos);
}

TH_TEST(run_reports_ragged_action_row_with_line_number) {
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.a relay0.b;\n  200;\n@\n");
	TH_CHECK(msg.find("Does Not Sync") != std::string::npos);
	TH_CHECK(msg.find("we got line: 3") != std::string::npos);
}

TH_TEST(run_registers_inline_alias_before_dispatch) {
	/* @Alias 先于 @Action 生效；这里只验「别名注册阶段不报错」，
	 * 派发仍会因设备未声明而失败——错误必须来自派发而不是别名阶段。 */
	const std::string msg = runTexts(kBasicSql,
		"@Alias\nname alias;\nspeed duty;\n@\n"
		"@Action\nrelay0.speed;\n  200;\n@\n");
	TH_CHECK(msg.find("no 'name' column") == std::string::npos);
	TH_CHECK(msg.find("Duplicate alias name") == std::string::npos);
	TH_CHECK(msg.find("'relay0'") != std::string::npos);
}

TH_TEST(run_rejects_duplicate_alias_in_one_action_script) {
	const std::string msg = runTexts(kBasicSql,
		"@Alias\nname alias;\nspeed duty;\nspeed pos;\n@\n"
		"@Action\nrelay0.speed;\n  200;\n@\n");
	TH_CHECK(msg.find("Duplicate alias name 'speed'") != std::string::npos);
}

TH_TEST(run_rejects_alias_matrix_without_columns) {
	const std::string msg = runTexts(kBasicSql, "@Alias\nfoo;\nbar;\n@\n");
	TH_CHECK(msg.find("Alias matrix 'Alias' has no 'name' column") != std::string::npos);
}

TH_TEST(run_registers_inline_vars_before_dispatch) {
	/* @Vars 先于 @Action 生效；派发因设备未声明而失败——错误必须来自派发，
	 * 而不是 Vars 注册阶段。 */
	const std::string msg = runTexts(kBasicSql,
		"@Vars\nname value;\nspd 90;\n@\n"
		"@Action\nrelay9.spd;\n  200;\n@\n");
	TH_CHECK(msg.find("no 'value' column") == std::string::npos);
	TH_CHECK(msg.find("not a valid Number") == std::string::npos);
	TH_CHECK(msg.find("Duplicate Vars name") == std::string::npos);
	TH_CHECK(msg.find("'relay9'") != std::string::npos);
}

TH_TEST(run_rejects_duplicate_vars_in_one_action_script) {
	const std::string msg = runTexts(kBasicSql,
		"@Vars\nname value;\nspd 1;\nspd 2;\n@\n"
		"@Action\nrelay0.spd;\n  200;\n@\n");
	TH_CHECK(msg.find("Duplicate Vars name 'spd'") != std::string::npos);
}

TH_TEST(run_rejects_vars_matrix_without_value_column) {
	const std::string msg = runTexts(kBasicSql, "@Vars\nname;\nspd;\n@\n");
	TH_CHECK(msg.find("Vars matrix 'Vars' has no 'value' column") != std::string::npos);
}

TH_TEST(run_rejects_non_numeric_vars_value) {
	/* @Vars 仅支持 Number：非数值在注册阶段即报错 */
	const std::string msg = runTexts(kBasicSql, "@Vars\nname value;\nspd fast;\n@\n");
	TH_CHECK(msg.find("neither a Number") != std::string::npos);
}

/* ---------------- HwRegistry（R-2：单一注册表） ---------------- */

namespace {

std::unique_ptr<bot::MatrixVars_Hardware> dummyHw() {
	return std::unique_ptr<bot::MatrixVars_Hardware>(new fx::LambdaHw(
		[](const bot::Json&) { return bot::JsonNull(); }));
}

} // namespace

TH_TEST(hw_registry_add_and_find) {
	bot::HwRegistry reg;
	reg.add("a", dummyHw(), " (in table 'T')");
	reg.add("b", dummyHw());
	TH_CHECK(reg.contains("a"));
	TH_CHECK(reg.find("a") != nullptr);
	TH_CHECK(reg.find("missing") == nullptr);
	/* 销毁序 = 声明序，会话别名不进入 */
	TH_CHECK(reg.sequence().size() == 2);
}

TH_TEST(hw_registry_rejects_duplicate_name) {
	bot::HwRegistry reg;
	reg.add("a", dummyHw(), " (in table 'T')");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Duplicate hardware variable name: 'a'",
		reg.add("a", dummyHw(), " (in table 'U')"));
	/* 失败不破坏既有注册 */
	TH_CHECK(reg.sequence().size() == 1);
	TH_CHECK(reg.find("a") != nullptr);
}

TH_TEST(hw_registry_session_override_shadows_but_keeps_destroy_order) {
	bot::HwRegistry reg;
	reg.add("a", dummyHw());
	bot::MatrixVars_Hardware* structureVar = reg.find("a");
	reg.override("a", dummyHw());
	/* 名字索引指向别名，但结构变量仍在销毁序中 */
	TH_CHECK(reg.find("a") != nullptr);
	TH_CHECK(reg.find("a") != structureVar);
	TH_CHECK(reg.sequence().size() == 1);
	TH_CHECK(reg.sequence()[0] == structureVar);
}

/* ---- @Action 设备列别名解析（2026-09-19 设计评估；解析单点白盒锚定）---- */

/* 构造 @Alias 对象（一行 name/value 的别名矩阵，行 1 = 数据行） */
static std::unique_ptr<bot::MtVars_Alias> aliasHw(const std::string& name,
	const std::string& target) {
	std::string text = "@Alias\nname  alias;\n   " + name + "  " + target + ";\n@\n";
	return bot::MtVars_Alias::Factory(fx::oneMatrix(text), 1);
}

TH_TEST(action_device_alias_resolves_to_driver) {
	bot::HwRegistry reg;
	reg.add("dev1", dummyHw(), "");
	reg.add("leg", aliasHw("leg", "dev1"), "");
	/* 别名 → 目标名；非标量真名原样；未知名原样（派发期保留行号上下文报错） */
	TH_CHECK(bot::ActionRunner::resolveActionDevice(reg, "leg") == "dev1");
	TH_CHECK(bot::ActionRunner::resolveActionDevice(reg, "dev1") == "dev1");
	TH_CHECK(bot::ActionRunner::resolveActionDevice(reg, "ghost") == "ghost");
}

TH_TEST(action_device_alias_to_missing_target_throws) {
	bot::HwRegistry reg;
	reg.add("leg", aliasHw("leg", "ghost"), "");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "resolves via @Alias to 'ghost'",
		(void)bot::ActionRunner::resolveActionDevice(reg, "leg"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is not a defined hardware variable",
		(void)bot::ActionRunner::resolveActionDevice(reg, "leg"));
}

TH_TEST(action_device_alias_to_another_alias_throws) {
	/* 一层为止：别名指向别名在解析点即报错，不做链式穿透 */
	bot::HwRegistry reg;
	reg.add("leg2", aliasHw("leg2", "dev1"), "");
	reg.add("leg", aliasHw("leg", "leg2"), "");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is itself a scalar",
		(void)bot::ActionRunner::resolveActionDevice(reg, "leg"));
}

TH_TEST(action_device_vars_rejected) {
	/* @Vars 是数值常量，永远不能命名设备——显式拒绝而非落到 not defined */
	bot::HwRegistry reg;
	reg.add("v", bot::MtVars_Vars::Factory(
		fx::oneMatrix("@Vars\nname value;\n   v  35;\n@\n"), 1,
		[](const std::string&) { return 0.0; }), "");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is a numeric constant and cannot name a device",
		(void)bot::ActionRunner::resolveActionDevice(reg, "v"));
}

TH_TEST(action_dispatch_through_alias_device) {
	/* 派发端到端（桩驱动）：设备列 leg（别名 → dev1）的参数落到 dev1 的一次 play */
	bot::HwRegistry reg;
	bot::Json played;
	reg.add("dev1", std::make_unique<fx::LambdaHw>(
		[&played](const bot::Json& in) {
			played = in;
			auto o = bot::JsonObject();
			o.put("result", bot::JsonString("OK"));
			return o;
		}), "");
	reg.add("leg", aliasHw("leg", "dev1"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\nleg.test;\n  200;\n@\n");
	bot::ActionRunner(reg).runAction(mats);
	TH_CHECK(played.isObj());
	TH_CHECK(played.get("test").has_value() && played.get("test")->getFloat() == 200.0);
}

TH_TEST(action_dispatch_allows_same_device_different_params) {
	/* 同一设备两个参数列（别名列 + 真名列都解析到 dev1）合并为一次 play：
	 * 查重键是 (设备, 参数)，同设备不同参数是合法的一拍多参 */
	bot::HwRegistry reg;
	bot::Json played;
	int plays = 0;
	reg.add("dev1", std::make_unique<fx::LambdaHw>(
		[&played, &plays](const bot::Json& in) {
			played = in;
			++plays;
			auto o = bot::JsonObject();
			o.put("result", bot::JsonString("OK"));
			return o;
		}), "");
	reg.add("leg", aliasHw("leg", "dev1"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\nleg.test  dev1.note;\n  200      7;\n@\n");
	bot::ActionRunner(reg).runAction(mats);
	TH_CHECK(plays == 1);
	TH_CHECK(played.get("test").has_value() && played.get("test")->getFloat() == 200.0);
	TH_CHECK(played.get("note").has_value() && played.get("note")->getFloat() == 7.0);
}

TH_TEST(action_dispatch_rejects_duplicate_resolved_param) {
	/* 撞设备第二层防线：不同列头（别名 + 真名）解析出同一 (设备, 参数)
	 * ——归并期文本查重不可见，派发前必须显式报错，不得逐行静默覆盖 */
	bot::HwRegistry reg;
	reg.add("dev1", dummyHw(), "");
	reg.add("leg", aliasHw("leg", "dev1"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\nleg.test  dev1.test;\n  200      300;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "already targeted by column",
		bot::ActionRunner(reg).runAction(mats));
}

/* ---- $(...) 类型分派：别名入式与文本拼接（2026-09-20 设计评估）----
 * 运算/拼接只发生在 $(...) 之内；'+' 出现文本操作数即拼接（数值真值
 * formatNumber 参与），全数值仍是算术。求值都在派发期。 */

/* 构造 @Vars 对象（字面量值；resolver 仅 $(...) 值需要，测试不用表达式值） */
static std::unique_ptr<bot::MtVars_Vars> varsHw(const std::string& name,
	const std::string& value) {
	return bot::MtVars_Vars::Factory(
		fx::oneMatrix("@Vars\nname value;\n   " + name + "  " + value + ";\n@\n"), 1,
		[](const std::string&) { return 0.0; });
}

/* 捕获 play 入参的桩驱动（OK 契约），返回登记好的指针引用 */
static std::unique_ptr<bot::MatrixVars_Hardware> captureHw(bot::Json& played) {
	return std::make_unique<fx::LambdaHw>([&played](const bot::Json& in) {
		played = in;
		auto o = bot::JsonObject();
		o.put("result", bot::JsonString("OK"));
		return o;
	});
}

TH_TEST(action_device_expr_concat_lands_on_resolved_device) {
	/* 设备位 $(pfx+n)：别名值 "relay-" + 数值真值 "2" 拼出 "relay-2"，
	 * 派发落在该名字命名的驱动上 */
	bot::HwRegistry reg;
	bot::Json played;
	reg.add("relay-2", captureHw(played), "");
	reg.add("pfx", aliasHw("pfx", "relay-"), "");
	reg.add("n", varsHw("n", "2"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\n$(pfx+n).test;\n  200;\n@\n");
	bot::ActionRunner(reg).runAction(mats);
	TH_CHECK(played.isObj());
	TH_CHECK(played.get("test").has_value() && played.get("test")->getFloat() == 200.0);
}

TH_TEST(action_device_expr_product_resolves_like_bare_name) {
	/* $(p) ≡ p：产物命中别名走与裸名相同的解析（一层为止规则的对称面）；
	 * 产物是别名名 → 再解一层到驱动（eval 已解引用一次，产物按裸名规则） */
	bot::HwRegistry reg;
	reg.add("dev1", dummyHw(), "");
	reg.add("p", aliasHw("p", "dev1"), "");
	TH_CHECK(bot::ActionRunner::resolveActionDevice(reg, "$(p)") == "dev1");
	reg.add("x", aliasHw("x", "leg"), "");
	reg.add("leg", aliasHw("leg", "dev1"), "");
	TH_CHECK(bot::ActionRunner::resolveActionDevice(reg, "$(x)") == "dev1");
}

TH_TEST(action_device_expr_product_naming_vars_rejected) {
	/* 产物命中 @Vars → 与裸名同样的显式拒绝（数值常量不能命名设备） */
	bot::HwRegistry reg;
	reg.add("v", varsHw("v", "35"), "");
	reg.add("q", aliasHw("q", "v"), "");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is a numeric constant and cannot name a device",
		(void)bot::ActionRunner::resolveActionDevice(reg, "$(q)"));
}

TH_TEST(action_device_expr_rejects_driver_and_undefined_reference) {
	/* 表达式里引用驱动/守护名：名字是拿来做算术/拼接的，不是拿来做播放的；
	 * 未定义名沿用既有文案 */
	bot::HwRegistry reg;
	reg.add("dev1", dummyHw(), "");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "driver/daemon variable; only @Alias/@Vars may appear in an expression",
		(void)bot::ActionRunner::resolveActionDevice(reg, "$(dev1)"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not a defined hardware variable",
		(void)bot::ActionRunner::resolveActionDevice(reg, "$(ghost)"));
}

TH_TEST(action_param_expr_product_is_param_name) {
	/* 参数位 $(...)：产物即参数名（不再解析）；裸名不命中保持原文。
	 * $(...) 内标识符是严格引用（未定义即抛）——拼接文本只能来自别名。 */
	bot::HwRegistry reg;
	reg.add("p", aliasHw("p", "note"), "");
	TH_CHECK(bot::ActionRunner::resolveActionParam(reg, "$(p)") == "note");
	reg.add("a1", aliasHw("a1", "no"), "");
	reg.add("a2", aliasHw("a2", "te"), "");
	TH_CHECK(bot::ActionRunner::resolveActionParam(reg, "$(a1+a2)") == "note");
	TH_CHECK(bot::ActionRunner::resolveActionParam(reg, "note") == "note");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not a defined hardware variable",
		(void)bot::ActionRunner::resolveActionParam(reg, "$(t+est)"));
}

TH_TEST(action_cell_text_param_via_concat) {
	/* 数据格拼接：$(g+n) 产物是文本 → 文本参数（JsonString），与 SQL 元数据
	 * 列同型；数值格不受影响 */
	bot::HwRegistry reg;
	bot::Json played;
	reg.add("dev1", captureHw(played), "");
	reg.add("g", aliasHw("g", "hello-"), "");
	reg.add("n", varsHw("n", "2"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\ndev1.note;\n  $(g+n);\n@\n");
	bot::ActionRunner(reg).runAction(mats);
	TH_CHECK(played.get("note").has_value() && played.get("note")->isStr());
	TH_CHECK(played.get("note")->getValue() == "hello-2");
}

TH_TEST(action_cell_numeric_expr_unchanged) {
	/* 全数值表达式仍是算术（拼接只出现在文本参与时）——既有行为钉 */
	bot::HwRegistry reg;
	bot::Json played;
	reg.add("dev1", captureHw(played), "");
	reg.add("n", varsHw("n", "2"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\ndev1.test;\n  $(n*100);\n@\n");
	bot::ActionRunner(reg).runAction(mats);
	TH_CHECK(played.get("test").has_value() && played.get("test")->getFloat() == 200.0);
}

TH_TEST(action_dispatch_rejects_expr_param_colliding_with_bare) {
	/* 查重键是解析后 (设备, 参数)：$(...) 拼出的参数名与裸名同键也必须抓
	 * （归并期文本查重不可见，不得逐行静默覆盖） */
	bot::HwRegistry reg;
	reg.add("dev1", dummyHw(), "");
	reg.add("p", aliasHw("p", "note"), "");
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Action\ndev1.$(p)  dev1.note;\n  200      300;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "already targeted by column",
		bot::ActionRunner(reg).runAction(mats));
}

/* ==================== 会话变量作用域（2026-09-20 重构阶段 0/1）====================
 * 缺陷实证（本组三条用例在修复前全部为红）：
 *   动作脚本内联的 @Alias/@Vars 经 HwRegistry::override 注册后**永不回收**——
 *     ① 跨 run() 残留（agent 长驻模式下无界增长，且脚本 A 的别名对脚本 B 可见）；
 *     ② 与结构变量同名时**永久遮蔽**结构定义，直到进程退出；
 *     ③ 注册发生在派发之前（RobotStruct::run 先注册后 runAction），
 *        于是一个连 @Action 都没有、以报错收场的脚本也已完成注册表改写。
 * 契约（修复后）：会话别名的作用域 = 单次 run()；run() 出口（含异常路径）完全回滚，
 * 结构视图与销毁序一字不动，byName() 结构清单不增长。
 */

/* 跑一遍动作脚本并吞掉异常：本组关心「注册表副作用」，不是派发结果
 * （没有 active() 时派发必然因设备未注册而失败，注册却已完成） */
static void runSwallow(bot::RobotStruct& robot, const std::string& mt) {
	try { bot::MemoryReader mr(mt); robot.run(mr); } catch (const std::exception&) {}
}

/* 跑一遍动作脚本并返回报错文本（无异常返回空串） */
static std::string runCapture(bot::RobotStruct& robot, const std::string& mt) {
	try {
		bot::MemoryReader mr(mt);
		robot.run(mr);
	} catch (const std::exception& e) {
		return std::string(e.what());
	}
	return std::string();
}

TH_TEST(session_alias_does_not_survive_a_run) {
	bot::MemoryReader sr(kBasicSql);
	bot::RobotStruct robot(sr);
	const std::size_t namesBefore = robot.registry().byName().size();
	TH_CHECK(!robot.registry().contains("ghost"));

	runSwallow(robot, "@Alias\nname  alias;\n ghost timer-0;\n@\n\n@Action\nghost.delay;\n 1;\n@\n");

	/* 会话结束：别名不再可见，结构名字索引一个不增 */
	TH_CHECK(!robot.registry().contains("ghost"));
	TH_CHECK(robot.registry().find("ghost") == nullptr);
	TH_CHECK(robot.registry().byName().size() == namesBefore);
}

TH_TEST(session_alias_does_not_shadow_structure_variable) {
	bot::MemoryReader sr(kBasicSql);
	bot::RobotStruct robot(sr);
	robot.active();
	auto* before = robot.registry().find("relay0");
	TH_CHECK(before != nullptr && std::string(before->kindName()) == "lua");

	/* 会话内把 relay0 别名为 timer-0（旧实现：结构定义被永久改写） */
	runSwallow(robot, "@Alias\nname  alias;\n relay0 timer-0;\n@\n");

	auto* after = robot.registry().find("relay0");
	TH_CHECK(after != nullptr);
	TH_CHECK(after == before);
	TH_CHECK(std::string(after->kindName()) == "lua");
	robot.deactivate();
}

TH_TEST(failed_action_script_does_not_mutate_registry) {
	bot::MemoryReader sr(kBasicSql);
	bot::RobotStruct robot(sr);
	const std::size_t namesBefore = robot.registry().byName().size();

	/* 有 @Alias、没有 @Action：注册先于派发，旧实现在这里已经改写了注册表 */
	std::string err = runCapture(robot, "@Alias\nname  alias;\n ghost timer-0;\n@\n");
	TH_CHECK(err.find("No @Action matrix found") != std::string::npos);

	TH_CHECK(!robot.registry().contains("ghost"));
	TH_CHECK(robot.registry().byName().size() == namesBefore);
}

TH_TEST(session_repeated_runs_do_not_grow_the_registry) {
	/* agent 长驻模式的等价形态：同一 RobotStruct 反复 run()。
	 * byName() 结构清单规模必须恒定（旧实现每个新别名名字都留一份）。 */
	bot::MemoryReader sr(kBasicSql);
	bot::RobotStruct robot(sr);
	const std::size_t namesBefore = robot.registry().byName().size();
	/* 200 轮；名字带序号，旧实现会留 200 份 */
	for (int i = 0; i < 200; ++i) {
		runSwallow(robot, "@Alias\nname  alias;\n ghost" + std::to_string(i)
			+ " timer-0;\n@\n");
	}
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	TH_CHECK(!robot.registry().contains("ghost0"));
	TH_CHECK(!robot.registry().contains("ghost199"));
}
