/* test_robot.cpp —— 结构脚本装配（RobotStruct 的解析与派发前置校验）
 *
 * 只覆盖「不需要 Lua 运行时」的两段：
 *   构造函数 = readSQL（Lexer + Parser + MatrixObject 拼装）
 *   run()    = 动作矩阵读取 + @Alias 注册 + @Action 派发前的列名/设备名校验
 * 这两段的失败都发生在真机资源申请之前，报错必须精确到表名/列名/行号。
 */

#include <stdexcept>
#include <string>

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
	TH_CHECK(swapped.find("Token name'RELAY'") != std::string::npos);

	const std::string truncated = sqlError("CREATE TABLE Relay ( name String\n");
	TH_CHECK(truncated.find("unexpected end of script") != std::string::npos);

	const std::string stray = sqlError("; CREATE TABLE Relay ( name String );\n");
	TH_CHECK(stray.find("Token ;") != std::string::npos);
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
