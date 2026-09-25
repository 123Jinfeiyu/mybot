/* test_robot.cpp —— 结构脚本装配（RobotStruct 的解析与派发前置校验）
 *
 * 主体只覆盖「不需要 Lua 运行时」的两段：
 *   构造函数 = readSQL（Lexer + Parser + MatrixObject 拼装）
 *   run()    = 动作矩阵读取 + @Alias 注册 + @Action 派发前的列名/设备名校验
 * 这两段的失败都发生在真机资源申请之前，报错必须精确到表名/列名/行号。
 *
 * 例外：文件末尾两条「析构兜底断电」用例需要 Lua 运行时——断电的可观测证据
 * 由 test/cpp/LifetimeProbe.lua 落成文件痕迹（destroy 每次追加 1 字节）。
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "harness.hpp"
#include "daemonvar.hpp"
#include "matrix.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace fs = std::filesystem;
using fx::TempDir;

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
std::deque<mt::MatrixObject> loadMatrices(const std::string& sql) {
	std::deque<mt::MatrixObject> out;
	bot::MemoryReader reader(sql);
	bot::ScriptLoader::readSQL(reader, out);
	return out;
}

const mt::MatrixObject* matrixNamed(const std::deque<mt::MatrixObject>& ms, const std::string& n) {
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

/* P0 回归守门（2026-09-23 审计）：readSQL 定位表下标不得用 `root - &out[0]`
 * 指针差——容器是 std::deque，元素不保证连续（libstdc++ 每块 512/sizeof = 5 个
 * MatrixObject），第 6 张表起跨块指针差是 UB，rowOrigin[垃圾下标] 越界读写。
 * 实测 spider1.sql（6 张表）在启动解析期 100% 段错误；全部仓内夹具 ≤5 张表，
 * 恰好从未越过块边界——这正是该缺陷躲过 662 个单测的原因。
 * 本用例用 7 张表（跨两个块边界）逐表 INSERT，断言行数与内容逐一正确；
 * 内容断言在「侥幸不崩但下标静默漂移」的堆布局下同样会红。 */
TH_TEST(robot_readsql_locates_tables_across_deque_chunk_boundary) {
	std::string sql;
	for (int i = 0; i < 7; ++i) {
		sql += "CREATE TABLE T" + std::to_string(i) + " (name String, v String);\n";
	}
	for (int i = 0; i < 7; ++i) {
		/* INSERT 故意全部放在 CREATE 之后：目标表下标 5、6 落在第二块里，
		 * appendRowTracked 取 rowOrigin 下标正是在这两条上踩雷 */
		sql += "INSERT INTO T" + std::to_string(i) + " (name, v) VALUES ('n"
			+ std::to_string(i) + "', 'x');\n";
	}
	const std::deque<mt::MatrixObject> ms = loadMatrices(sql);
	TH_CHECK(ms.size() == 7);
	for (int i = 0; i < 7; ++i) {
		const mt::MatrixObject* m = matrixNamed(ms, "T" + std::to_string(i));
		TH_CHECK(m != nullptr);
		if (m == nullptr) { continue; }
		TH_CHECK(m->rows() == 2);	/* 列头 + 恰好 1 条数据行 */
		TH_CHECK(cell(*m, 1, 0) == "n" + std::to_string(i));
		TH_CHECK(cell(*m, 1, 1) == "x");
	}
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

/* ==================== 析构兜底断电（审计 P7 / 阶段 1.4）====================
 * 断电此前只是 main.cpp:203 的一个调用点义务：任何漏掉它的新退出路径（早退
 * return、catch 之外的意外）都会把执行器留在最后指令位置、I2C 从设备滞留中间态。
 * 现在它是生命周期不变式——~RobotStruct 补一次，且幂等。
 * 可观测证据由 test/cpp/LifetimeProbe.lua 提供：destroy 每被调用一次就往
 * data.path 追加 1 字节 'x'，于是「调用次数 == 文件长度」，两条性质都能直接断言。 */
namespace {

/* BOT4CJ_MODULE_PATH 临时指向 modules:test/cpp（探针驱动在后者），出作用域还原。
 * 沙箱按该变量逐段构建 dofile 白名单（src/luasandbox.cpp），不设则探针加载不到。 */
class ModulePathGuard {
public:
	explicit ModulePathGuard(const char* path) : had_(::getenv(kVar) != nullptr) {
		if (had_) { old_ = ::getenv(kVar); }
		::setenv(kVar, path, 1);
	}
	~ModulePathGuard() {
		if (had_) { ::setenv(kVar, old_.c_str(), 1); }
		else { ::unsetenv(kVar); }
	}
	ModulePathGuard(const ModulePathGuard&) = delete;
	ModulePathGuard& operator=(const ModulePathGuard&) = delete;

private:
	static constexpr const char* kVar = "BOT4CJ_MODULE_PATH";
	bool had_;
	std::string old_;
};

std::string markerOf(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) { return std::string(); }
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string probeSql(const fs::path& marker) {
	return "CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + marker.string() + "');\n";
}

} // namespace

/* build 期的停止响应（2026-09-21 第七轮审计）：信号处理器只置标志
 * （async-signal-safe），真正的响应点在各处循环里——而 active() 的逐件 build
 * 循环此前**不查标志**。后果是：多驱动上电（每个 build 内含真机延时）期间
 * 第一次 Ctrl-C 毫无可见效果，运维必然再按一次，而第二次是 `SIG_DFL + raise`
 * 强杀 ⇒ 已 build 成功的器件原地带电、deactivate 一行不执行。 */
TH_TEST(robot_active_aborts_when_stop_requested_before_build) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("stop-build");
	const fs::path marker = tmp.path() / "destroyed";

	/* 标志必须复位，否则污染后续所有用例（含 agent 侧那些查 stopRequested 的） */
	struct StopGuard {
		StopGuard() { bot::stopRequested().store(true); }
		~StopGuard() { bot::stopRequested().store(false); }
	} stopGuard;

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	TH_CHECK_THROWS_MSG(std::runtime_error, "上电被中断", robot.active());

	/* 中止后清理链照常可走：deactivate 不得抛出，且幂等（第二次直接返回）。
	 * 未 build 的器件 destroy 只计入告警，这是本仓既有口径（见 active() 注释）。 */
	robot.deactivate();
	robot.deactivate();
}

TH_TEST(destructor_deactivates_when_caller_forgot) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("lifetime-forgot");
	const fs::path marker = tmp.path() / "destroyed";
	{
		bot::MemoryReader sr(probeSql(marker));
		bot::RobotStruct robot(sr);
		robot.active();
		/* 故意不调用 deactivate：作用域结束即析构，兜底必须把 destroy 跑到 */
	}
	TH_CHECK(markerOf(marker) == "D");
}

TH_TEST(destructor_does_not_destroy_twice_after_explicit_deactivate) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("lifetime-idempotent");
	const fs::path marker = tmp.path() / "destroyed";
	{
		bot::MemoryReader sr(probeSql(marker));
		bot::RobotStruct robot(sr);
		robot.active();
		robot.deactivate();	/* main 的正常路径 */
	}
	/* 仍是 1 字节：析构认得「已断电」，没有重复 destroy（重复去初始化对
	 * 真器件不是无害的——已关断的器件再写寄存器可能落在无效状态机上） */
	TH_CHECK(markerOf(marker) == "D");
}

TH_TEST(destructor_is_a_noop_when_never_activated) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("lifetime-inactive");
	const fs::path marker = tmp.path() / "destroyed";
	{
		bot::MemoryReader sr(probeSql(marker));
		bot::RobotStruct robot(sr);
		/* 只解析结构、从未上电（如 --agent 的配置校验失败早退）：无事可做 */
	}
	TH_CHECK(markerOf(marker).empty());
}

/* ---- 断电屏障的顺序回归（审计 P2 / 阶段 1.1）----
 * Daemon 表**声明在探针之前**，于是 build 序 = 守护、探针，逆序 destroy =
 * 探针先、守护后——正是「执行器已去初始化、守护还在拍」的那个形状。
 * 守护每拍追加 'T'（LifetimeTick.lua），探针 destroy 追加 'D'（LifetimeProbe.lua），
 * 两者写同一文件，于是末字节就是顺序判据：
 *   屏障在位 → 守护先退场，之后才 destroy → 末字节 'D'
 *   屏障缺失 → destroy 之后守护继续拍（join 延后到它自己的 destroy）→ 末字节 'T'
 * 另断言 trace 里确实有 'T'：守护根本没跑起来的话，末字节 'D' 是假绿。 */
TH_TEST(deactivate_joins_daemons_before_hardware_deinit) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("lifetime-barrier");
	const fs::path marker = tmp.path() / "order";
	TH_CHECK(fs::exists(tmp.path()));
	::setenv("BOT4CJ_LIFETIME_MARK", marker.c_str(), 1);
	{
		bot::MemoryReader sr(
			"CREATE TABLE Daemon (name String, chunk String, interval String);\n"
			"INSERT INTO Daemon (name, chunk, interval) VALUES ('d0', 'LifetimeTick.lua', '10');\n"
			+ probeSql(marker));
		bot::RobotStruct robot(sr);
		robot.active();
		/* 时序（chunk 每拍 = 睡 150ms 后写 'T'，interval=10ms）：
		 * t≈150ms 第一拍写 T；t≈160ms 第二拍开始睡；t=220ms 这里发停机请求，
		 * 第二拍仍在途，它的 'T' 要到 t≈310ms 才落地。
		 * 有屏障 → 先等到 t≈310ms（T）、join 成功，再 destroy 写 'D' → 末字节 D；
		 * 无屏障 → destroy 立刻写 'D'，t≈310ms 又来一个 'T' → 末字节 T。 */
		std::this_thread::sleep_for(std::chrono::milliseconds(220));
		robot.deactivate();
	}
	::unsetenv("BOT4CJ_LIFETIME_MARK");

	const std::string trace = markerOf(marker);
	TH_CHECK(trace.find('T') != std::string::npos);
	TH_CHECK(!trace.empty() && trace.back() == 'D');
}

/* ==================== F-2（2026-09-22，原 TD-2 剩余部分）：daemon-grace-ms 参数化 ====================
 * 借用 test_daemon.cpp 的慢拍夹具（3s/拍 > joinTimeout=2s → 必走弃管路径）。
 * daemon 弃管后的二段宽限由 RobotStruct::deactivateGraceMs_ 控制（原写死
 * kMaxGraceMs=30000），本用例钉住：(1) setter 语义正确传递到 deactivate()；
 * (2) 用小宽限值（100ms）快速走完二段等待而不卡死测试；(3) stderr 可观测。 */

TH_TEST(robot_daemon_grace_ms_setter_affects_deactivate_barrier) {
	/* EnvVar 守卫：析构自动恢复——BOT4CJ_MODULE_PATH 会影响后续用例的沙箱
	 * dofile 白名单（见 test_luaend.cpp），必须离开作用域即还原，不能像
	 * test_daemon.cpp 那样裸 setenv（那是既有技术债，此处不重复）。 */
	fx::EnvVar modPath("BOT4CJ_MODULE_PATH", ".tmp");
	/* 从 test_daemon.cpp::writeSlowTickChunk 复制慢拍夹具
	 * （3s/拍，interval=10ms，joinTimeout=max(10*5,2000)=2000ms） */
	std::ofstream out(".tmp/test_daemon_grace_slow_tick.lua", std::ios::binary | std::ios::trunc);
	out << "local ffi = require(\"ffi\")\n"
	    << "ffi.cdef(\"int usleep(unsigned int usec);\")\n"
	    << "function daemon_tick()\n"
	    << "  ffi.C.usleep(3000000)\n"
	    << "  return 1\n"
	    << "end\n";
	out.close();

	const char* sql =
		"CREATE TABLE Daemon (name String, chunk String, interval String);\n"
		"INSERT INTO Daemon (name, chunk, interval) "
		"VALUES ('d1', 'test_daemon_grace_slow_tick.lua', '10');\n";

	bot::MemoryReader sr(sql);
	bot::RobotStruct robot(sr);

	/* F-2：在 active() 之前设置宽限——100ms 足够短，测试秒级完成；
	 * 若未生效，deactivate() 仍用 30000ms 缺省值，整个用例会卡 30s */
	robot.setDeactivateGraceMs(100);

	robot.active();
	/* 让守护进慢拍（200ms < 3s），保证 destroy 必走 abandoned 路径 */
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	/* 捕获 stderr：二段日志应点名宽限 100ms（不是 30000），且很快完成 */
	std::ostringstream buf;
	std::streambuf* old = std::cerr.rdbuf(buf.rdbuf());
	auto start = std::chrono::steady_clock::now();

	robot.deactivate();

	auto elapsed = std::chrono::steady_clock::now() - start;
	std::cerr.rdbuf(old);

	/* 断言 (1)：耗时远小于 30s，证明新值接管了（容差 5s，覆盖 CI 抖动） */
	TH_CHECK(elapsed < std::chrono::seconds(5));
	/* 断言 (2)：stderr 点名宽限 100（不是 30000），证明值传递到了 deactivate()。
	 * 文案格式见 robot.cpp："granting " << graceMs << "ms grace"（数字与
	 * "ms" 之间无空格）。 */
	std::string log = buf.str();
	TH_CHECK(log.find("granting 100ms grace") != std::string::npos);
	TH_CHECK(log.find("30000") == std::string::npos);
}

/* ==================== 单消费者判据（审计 1.3）====================
 * 只跑到**装配期**（ScriptLoader::readSQL + HwFactory::createHardVars），不调
 * build()——判据本就在任何资源申请之前，故这三条用例与主机有没有 /dev/i2c-1
 * 完全无关，连真机夹具（as7262.sql，守门的 E2E 段不执行它）也能确定性守门。
 * 三类形态各钉一条，重点是第三条：防止判据被顺手放宽成「一律单消费者」
 * ——那会打中 robots/test/ 下 20 个刻意写成「直连 + 守护同址」的夹具。 */
namespace {

std::string readFileAll(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("夹具不可读: " + path); }
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 装配（不 build）：返回空串表示成功，否则返回异常文本 */
std::string assembleSql(const std::string& sql) {
	try {
		std::deque<mt::MatrixObject> matrices;
		bot::MemoryReader sr(sql);
		bot::ScriptLoader::readSQL(sr, matrices);
		bot::HwRegistry registry;
		std::vector<std::shared_ptr<bot::DaemonRunner>> daemonHolders;
		std::vector<std::shared_ptr<bot::MetaProvider>> metaProviders;
		bot::HwFactory factory(registry, daemonHolders, metaProviders);
		factory.createHardVars(matrices);
	} catch (const std::exception& e) {
		return std::string(e.what());
	}
	return std::string();
}

std::string assembleFile(const std::string& path) { return assembleSql(readFileAll(path)); }

} // namespace

TH_TEST(single_consumer_rejects_two_consumers_of_a_declaring_driver) {
	ModulePathGuard guard("modules:test/cpp");
	const std::string err = assembleFile("robots/test/single_consumer.sql");
	TH_CHECK(err.find("two concurrent consumers") != std::string::npos);
	/* 报错必须点名双方，否则用户不知道该删哪一个 */
	TH_CHECK(err.find("spec0") != std::string::npos);
	TH_CHECK(err.find("spec-d0") != std::string::npos);
}

TH_TEST(single_consumer_accepts_the_five_real_hardware_fixtures) {
	ModulePathGuard guard("modules:test/cpp");
	/* 五个声明者驱动的真机夹具：直连变量走真器件、守护走 proto=virt（内存仿真
	 * 不算真器件消费者），故各自只有一个真消费者，装配必须通过。
	 * 这几条同时钉住"改回 proto=i2c 就该报错"的正向侧——夹具被改回去时，
	 * 这里会立刻红，而不是等到有人在真机上读到错数据。 */
	for (const char* f : {"as7262", "ms5611", "as3935", "vl53l0x", "vl53l4cd"}) {
		const std::string path = std::string("robots/test/") + f + ".sql";
		const std::string err = assembleFile(path);
		TH_CHECK_MSG(err.empty(), path + " 装配失败: " + err);
	}
}

TH_TEST(single_consumer_rejects_two_direct_vars_on_same_address) {
	ModulePathGuard guard("modules:test/cpp");
	/* 直连 vs 直连（不涉及守护）：同一声明者驱动、同总线同地址的两行 */
	const std::string err = assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE MS5611 (name String, I2C String, address String);\n"
		"INSERT INTO MS5611 (name, I2C, address) VALUES ('baro0', '$i2c-1', '0x77');\n"
		"INSERT INTO MS5611 (name, I2C, address) VALUES ('baro1', '$i2c-1', '0x77');\n");
	TH_CHECK(err.find("two concurrent consumers") != std::string::npos);
	TH_CHECK(err.find("baro0") != std::string::npos);
	TH_CHECK(err.find("baro1") != std::string::npos);
}

TH_TEST(single_consumer_allows_same_driver_on_different_addresses) {
	ModulePathGuard guard("modules:test/cpp");
	/* 判据键里必须含地址：同型号两颗器件（0x76 / 0x77）是两个独立消费者，
	 * 不该被一刀切拦下——否则"按驱动名判重"这种错实现也能通过前面几条用例。 */
	TH_CHECK(assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE MS5611 (name String, I2C String, address String);\n"
		"INSERT INTO MS5611 (name, I2C, address) VALUES ('baro0', '$i2c-1', '0x76');\n"
		"INSERT INTO MS5611 (name, I2C, address) VALUES ('baro1', '$i2c-1', '0x77');\n").empty());
	/* 地址写法不同（十六进制 / 十进制）但指向同一颗器件，仍须判重 */
	TH_CHECK(!assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE MS5611 (name String, I2C String, address String);\n"
		"INSERT INTO MS5611 (name, I2C, address) VALUES ('baro0', '$i2c-1', '0x77');\n"
		"INSERT INTO MS5611 (name, I2C, address) VALUES ('baro1', '$i2c-1', '119');\n").empty());
}

TH_TEST(single_consumer_allows_dual_consumers_of_ordinary_drivers) {
	ModulePathGuard guard("modules:test/cpp");
	/* ADS1115 未声明 SINGLE_CONSUMER：HAL 的每次访问是单次 I2C_RDWR 原子事务
	 * （指针写 + 数据读合并），直连变量 adc0 与守护 adc-d0 同为
	 * /dev/i2c-1 @ 0x48 也安全。这条钉住「判据只针对声明者」。 */
	TH_CHECK(assembleFile("robots/test/ads1115.sql").empty());
}

/* ---- 守护行的三条静默旁路（2026-09-21 第七轮审计）----
 * 旧实现把「缺列」当成「不参与判据」直接 continue，而守护侧对 proto/path 都有缺省、
 * 多器件守护的器件清单在 devices 列里 ⇒ 三种被 docs/daemon.md 认可的写法都绕过了
 * 判据，运行期却真的在总线上开了 fd。漏标 = 双消费者静默数据错乱，**无运行期症状**。 */

TH_TEST(single_consumer_rejects_daemon_row_that_omits_proto_and_path) {
	ModulePathGuard guard("modules:test/cpp");
	/* Daemon 行省略 proto 与 path —— 正是 docs/daemon.md 的最小写法
	 * （守护侧缺省 proto=i2c、path=/dev/i2c-1）。直连变量 spec0 在
	 * $i2c-1(/dev/i2c-1) @ 0x49，两者必须被判为同一颗器件。 */
	const std::string err = assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE AS7262 (name String, I2C String, address String);\n"
		"INSERT INTO AS7262 (name, I2C, address) VALUES ('spec0', '$i2c-1', '0x49');\n"
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"CREATE TABLE Daemon (name String, chunk String, driver String, addr String,\n"
		"                     interval String, fields String);\n"
		"INSERT INTO Daemon (name, chunk, driver, addr, interval, fields)\n"
		"  VALUES ('spec-d0', 'lib/sensor_daemon.lua', 'AS7262', '0x49', '300', 'V|B');\n");
	TH_CHECK_MSG(err.find("two concurrent consumers") != std::string::npos,
		"省略 proto/path 的守护行仍须参与判重，实际: " + err);
	TH_CHECK(err.find("spec0") != std::string::npos);
	TH_CHECK(err.find("spec-d0") != std::string::npos);

	/* 对照：显式 proto=virt 仍须放行（内存仿真不碰真器件；五个真机夹具正是这么写的） */
	TH_CHECK_MSG(assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE AS7262 (name String, I2C String, address String);\n"
		"INSERT INTO AS7262 (name, I2C, address) VALUES ('spec0', '$i2c-1', '0x49');\n"
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"CREATE TABLE Daemon (name String, chunk String, driver String, proto String,\n"
		"                     addr String, interval String, fields String);\n"
		"INSERT INTO Daemon (name, chunk, driver, proto, addr, interval, fields)\n"
		"  VALUES ('spec-d0', 'lib/sensor_daemon.lua', 'AS7262', 'virt', '0x49', '300', 'V|B');\n"
		).empty(), "proto=virt 的守护不该参与判据（否则无硬件夹具全线误报）");
}

TH_TEST(single_consumer_parses_multi_device_daemon_devices_column) {
	ModulePathGuard guard("modules:test/cpp");
	/* 多器件守护（chunk = lib/sensor_daemon_multi.lua）没有 driver/addr 列，
	 * 器件清单在 devices 列：`driver@addr@fields[@k=v...]`，以 '|' 分隔。 */
	const std::string err = assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE AS7262 (name String, I2C String, address String);\n"
		"INSERT INTO AS7262 (name, I2C, address) VALUES ('spec0', '$i2c-1', '0x49');\n"
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"CREATE TABLE Daemon (name String, chunk String, proto String, path String,\n"
		"                     interval String, devices String);\n"
		"INSERT INTO Daemon (name, chunk, proto, path, interval, devices)\n"
		"  VALUES ('multi-d0', 'lib/sensor_daemon_multi.lua', 'i2c', '/dev/i2c-1', '300',\n"
		"          'BME280@0x76@t,h,p|AS7262@0x49@V,B');\n");
	TH_CHECK_MSG(err.find("two concurrent consumers") != std::string::npos,
		"devices 列里的声明者驱动（第 2 个 spec）必须被登记，实际: " + err);
	/* 定位串要点名到是哪一个 spec，否则用户不知道清单里哪一颗撞了 */
	TH_CHECK_MSG(err.find("devices[2]") != std::string::npos, "应点名 spec 下标，实际: " + err);
	TH_CHECK(err.find("AS7262") != std::string::npos);
	TH_CHECK(err.find("multi-d0") != std::string::npos);

	/* 对照：devices 里全是未声明者驱动 ⇒ 直连 + 守护双消费者照常放行 */
	TH_CHECK_MSG(assembleSql(
		"CREATE TABLE I2C (name String, path String);\n"
		"INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');\n"
		"CREATE TABLE ADS1115 (name String, I2C String, address String);\n"
		"INSERT INTO ADS1115 (name, I2C, address) VALUES ('adc0', '$i2c-1', '0x48');\n"
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"CREATE TABLE Daemon (name String, chunk String, proto String, path String,\n"
		"                     interval String, devices String);\n"
		"INSERT INTO Daemon (name, chunk, proto, path, interval, devices)\n"
		"  VALUES ('multi-d0', 'lib/sensor_daemon_multi.lua', 'i2c', '/dev/i2c-1', '300',\n"
		"          'ADS1115@0x48@v');\n").empty(),
		"未声明者驱动的双消费者不该被拦（判据只针对声明者）");
}

TH_TEST(daemon_defaults_match_lua_source) {
	/* 两侧钉住：C++ 判据用的守护缺省值必须逐字出现在 sensor_daemon.lua 的缺省
	 * 表达式里。任一侧漂移都会红——这类漂移**没有运行期症状**，只会让判据静默
	 * 漏判，所以只能靠读源比对（不是在两处各手抄一遍就算数）。 */
	const std::string lua = readFileAll("modules/lib/sensor_daemon.lua");
	TH_CHECK_MSG(!lua.empty(), "读不到 modules/lib/sensor_daemon.lua");
	const std::string protoExpr =
		std::string("cfg.proto or \"") + bot::HwFactory::kDaemonDefaultProto + "\"";
	TH_CHECK_MSG(lua.find(protoExpr) != std::string::npos,
		"Lua 侧 proto 缺省应与 C++ 常量一致（找 '" + protoExpr + "'）");
	const std::string pathExpr = std::string("proto == \"spi\" and \"")
		+ bot::HwFactory::kDaemonDefaultPathSpi + "\" or \""
		+ bot::HwFactory::kDaemonDefaultPathI2c + "\"";
	TH_CHECK_MSG(lua.find(pathExpr) != std::string::npos,
		"Lua 侧 path 缺省应与 C++ 常量一致（找 '" + pathExpr + "'）");
}

/* ==================== 热插表：结构 SQL 带电生效（2026-09-23）====================
 * RobotStruct::applySql 的端到端守门（P3 事务门面）。三条判据贯穿全组：
 *   ① 生效 = 真的装配 + 真的上电；
 *   ② 回滚 = 真的断电 + 注册表/矩阵一字复原；
 *   ③ 拒绝 = 活体一字未动（报错发生在任何资源申请之前）。
 * 断电的可观测证据仍是 LifetimeProbe.lua 的文件落痕。注意探针的 destroy 只在
 * **build 成功之后**才存在可调用的全局 destroy（luavar.cpp freeImpl：未 build 的
 * 变量全局 destroy 为 nil，跳过调用只释放状态），故「落痕数」精确等于
 * 「真正 build 过又被断电的器件数」——这正是区分「带电回滚」与「无副作用撤销」
 * 的依据，也是下面几条用例断言取值的由来。 */

namespace {

/* applySql 的报错文本（无异常返回空串） */
std::string hotSqlError(bot::RobotStruct& robot, const std::string& sql) {
	try {
		robot.applySql(sql);
	} catch (const std::exception& e) {
		return std::string(e.what());
	}
	return std::string();
}

} // namespace

/* 既有表插行：行落到结构矩阵、变量进注册表与销毁序，deactivate 时一并断电 */
TH_TEST(robot_hot_sql_insert_into_an_existing_table_goes_live) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-insert");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();
	TH_CHECK(robot.registry().contains("p0"));
	const std::size_t tablesBefore = robot.matrices().size();
	const std::size_t namesBefore = robot.registry().byName().size();

	const std::string msg = robot.applySql(
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + marker.string() + "');\n");

	TH_CHECK(robot.registry().contains("p1"));
	TH_CHECK(robot.declaresVariable("p1"));
	TH_CHECK(robot.matrices().size() == tablesBefore);	/* 插行不新增表 */
	TH_CHECK(robot.registry().byName().size() == namesBefore + 1);
	TH_CHECK(robot.registry().sequence().size() == namesBefore + 1);
	TH_CHECK(msg.find("已生效：新增硬件变量 1 个") != std::string::npos);
	TH_CHECK(msg.find("既有表追加 1 行") != std::string::npos);

	/* build 不落痕、destroy 才落：热插时器件已在带上是本用例的「生效」证据 */
	TH_CHECK(markerOf(marker).empty());
	robot.deactivate();
	TH_CHECK(markerOf(marker) == "DD");	/* 两行都在销毁序里 */
}

/* 新表：整表入列 → 装配 → 上电；表数与摘要都要如实反映 */
TH_TEST(robot_hot_sql_create_table_assembles_a_new_hardware_variable) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-create");
	const fs::path marker = tmp.path() / "destroyed";

	/* 结构期只有 Timer：LifetimeProbe 完全由热插长出 */
	bot::MemoryReader sr("CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n");
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t tablesBefore = robot.matrices().size();

	const std::string msg = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + marker.string() + "');\n");

	TH_CHECK(robot.matrices().size() == tablesBefore + 1);
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") != nullptr);
	TH_CHECK(msg.find("新增表 1 张") != std::string::npos);

	robot.deactivate();
	TH_CHECK(markerOf(marker) == "D");
}

/* P0 回归守门（2026-09-23 审计，热插侧）：applySql 的影子容器 = 活体全量拷贝 +
 * 本批次 CREATE，同样走 readSQL 的表下标定位。5 张活表 + 第 1 批 CREATE 出的
 * 第 6 张表让 INSERT 目标落在 deque 第二块——旧指针差实现在此 UB/崩溃（实测
 * relay-test + 连续热插到第 6 张表即段错误）。三批依次覆盖 appendRowTracked /
 * removeRowsOf（DROP 既存表擦全部行）/ DROP 本批新建表（createdHere 整表擦除）
 * 三个修复点；marker 落痕证明跨块边界下 INSERT 行真的装配上电、DROP 真的断电。 */
TH_TEST(robot_hot_sql_stages_tables_across_deque_chunk_boundary) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-cross");
	const fs::path marker = tmp.path() / "destroyed";

	/* 结构期 5 张表（deque 第一块的容量），全部纯软件驱动、无需硬件 */
	bot::MemoryReader sr(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n"
		"CREATE TABLE Relay (name String);\n"
		"INSERT INTO Relay (name) VALUES ('relay0');\n"
		"CREATE TABLE dummy (name String);\n"
		"INSERT INTO dummy (name) VALUES ('dummy0');\n"
		"CREATE TABLE Monitor (name String);\n"
		"INSERT INTO Monitor (name) VALUES ('monitor0');\n"
		"CREATE TABLE Ema (name String);\n"
		"INSERT INTO Ema (name) VALUES ('ema0');\n");
	bot::RobotStruct robot(sr);
	robot.active();
	TH_CHECK(robot.matrices().size() == 5);
	TH_CHECK(robot.registry().contains("timer-0"));
	TH_CHECK(robot.registry().contains("ema0"));

	/* 批 1：第 6 张表 + 插行（INSERT 目标下标 5 = 第二块首元素） */
	const std::string msg1 = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + marker.string() + "');\n");
	TH_CHECK(robot.matrices().size() == 6);
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(msg1.find("新增表 1 张") != std::string::npos);
	TH_CHECK(markerOf(marker).empty());	/* 已 build、未 destroy */

	/* 批 2：DROP 既存表——removeRowsOf 擦全部数据行（走表下标定位）+ 表壳摘除 */
	const std::string msg2 = robot.applySql("DROP TABLE LifetimeProbe;\n");
	TH_CHECK(robot.matrices().size() == 5);
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(msg2.find("已撤下表 1 张") != std::string::npos);
	TH_CHECK(markerOf(marker) == "D");	/* p0 真的被断电 */

	/* 批 3：本批 CREATE 后再 DROP——createdHere 整表擦除路径（第三处下标定位）。
	 * 该表从未入活体，摘要不报「已撤下表」（那是活体表壳摘除的计数），但擦除
	 * 必须精确——旧垃圾下标会 erase 错表，这里 matrices().size() == 5 即守门。 */
	const std::string msg3 = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"DROP TABLE LifetimeProbe;\n");
	TH_CHECK(robot.matrices().size() == 5);
	TH_CHECK(!robot.declaresVariable("p1"));
	TH_CHECK(msg3.find("新增硬件变量 0 个") != std::string::npos);

	robot.deactivate();
}

/* 上电中途收到停止信号：整批回滚。此处新变量尚未 build，故回滚只撤销
 * （不落痕）——正是「无副作用撤销」这一侧的证据，与下一条用例互补。 */
TH_TEST(robot_hot_sql_rolls_back_when_power_on_is_interrupted) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-stop");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t tablesBefore = robot.matrices().size();
	const std::size_t namesBefore = robot.registry().byName().size();
	const std::size_t rowsBefore = mt::findByName(robot.matrices(), "LifetimeProbe")->rows();

	bot::stopRequested().store(true);
	const std::string msg = hotSqlError(robot,
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + marker.string() + "');\n");
	bot::stopRequested().store(false);	/* 必须复位，否则污染后续用例 */

	TH_CHECK(msg.find("热插上电被中断") != std::string::npos);
	TH_CHECK(!robot.registry().contains("p1"));
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	TH_CHECK(robot.registry().sequence().size() == namesBefore);
	TH_CHECK(robot.matrices().size() == tablesBefore);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == rowsBefore);
	TH_CHECK(!robot.declaresVariable("p1"));
	TH_CHECK(markerOf(marker).empty());	/* 未 build 即中止：没有器件真的带电 */
	robot.deactivate();
}

/* Action 执行失败发生在**硬件已 build 之后**：回滚必须真的把新器件断电
 * （落痕），而不是只在注册表上摘名——这正是「全成功或全回滚」的物理含义。 */
TH_TEST(robot_hot_sql_rolls_back_and_disconnects_when_the_action_fails) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-action-fail");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t tablesBefore = robot.matrices().size();
	const std::size_t rowsBefore = mt::findByName(robot.matrices(), "LifetimeProbe")->rows();

	/* 硬件全部上电之后才轮到 Action 派发：未声明的设备在这里才炸 */
	const std::string msg = hotSqlError(robot,
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + marker.string() + "');\n"
		"CREATE TABLE Action (ghost.deg Number);\n"
		"INSERT INTO Action (ghost.deg) VALUES (200);\n");

	TH_CHECK(msg.find("not defined in your hardware declaration section") != std::string::npos);
	TH_CHECK(markerOf(marker) == "D");	/* 回滚断电：p1 已 build，destroy 落痕 */
	TH_CHECK(!robot.registry().contains("p1"));
	TH_CHECK(robot.matrices().size() == tablesBefore);	/* 瞬态 Action 表也不入列 */
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == rowsBefore);
	TH_CHECK(!robot.declaresVariable("p1"));
	/* 活体未受损：既有 p0 照常断电（落痕与回滚那次各一次） */
	robot.deactivate();
	TH_CHECK(markerOf(marker) == "DD");
}

/* Action 表瞬态：行随硬件一起生效、执行一次即丢弃——不入结构矩阵、不注册变量。
 * 动作能引用**本批次新建**的变量，正是「Action 排在硬件装配之后」这条排序的判据。 */
TH_TEST(robot_hot_sql_action_rows_run_once_and_are_discarded) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-action-transient");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr("");	/* 空结构：整批由热插长出 */
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + marker.string() + "');\n"
		"CREATE TABLE Action (p0.deg Number);\n"
		"INSERT INTO Action (p0.deg) VALUES (200);\n");

	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(!robot.registry().contains("Action"));
	TH_CHECK(robot.matrices().size() == 1);		/* 只有 LifetimeProbe，没有 Action 表 */
	TH_CHECK(mt::findByName(robot.matrices(), "Action") == nullptr);
	TH_CHECK(!robot.declaresVariable("Action"));
	TH_CHECK(msg.find("瞬态动作已执行 1 行（未注册，已丢弃）") != std::string::npos);

	robot.deactivate();
	TH_CHECK(markerOf(marker) == "D");
}

/* 变量重名（结构里既有 / 本批次内重复）必须在申请任何资源之前拒绝 */
TH_TEST(robot_hot_sql_rejects_a_duplicate_variable_name) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-duplicate");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t namesBefore = robot.registry().byName().size();
	const std::size_t rowsBefore = mt::findByName(robot.matrices(), "LifetimeProbe")->rows();

	const std::string msg = hotSqlError(robot,
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + marker.string() + "');\n");
	TH_CHECK(msg.find("硬件变量名 'p0' 重复") != std::string::npos);
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == rowsBefore);
	TH_CHECK(markerOf(marker).empty());

	/* 批内重名同样早报（两条 INSERT 抢同一个名字） */
	const std::string msg2 = hotSqlError(robot,
		"INSERT INTO LifetimeProbe (name, path) VALUES ('q1', '" + marker.string() + "');\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('q1', '" + marker.string() + "');\n");
	TH_CHECK(msg2.find("硬件变量名 'q1' 重复") != std::string::npos);
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	robot.deactivate();
}

/* 安全边界：表名决定 modules/<表名>.lua 的解析路径，热插语句来自不可信输入
 * （REPL / 模型），故必须过 isSafeIdent；列名会进守护配置串，同理。 */
TH_TEST(robot_hot_sql_rejects_unsafe_table_and_column_names) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-ident");

	bot::MemoryReader sr("");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string badTable = hotSqlError(robot, "CREATE TABLE ../evil (name String);\n");
	TH_CHECK(badTable.find("表名") != std::string::npos);
	TH_CHECK(badTable.find("不是安全标识符") != std::string::npos);

	/* 列名带 '=' 的注入向量：裸写 a=b 在词法层就被 '=' 分隔符打散（Token =，
	 * 2026-09-23 WHERE 改造）；引号包裹的 'a=b' 仍是一个 NAME 词元，能走到
	 * isSafeIdent 检查——防线必须在这里接住它（'=' 能在守护配置串里造字段）。 */
	const std::string badCol =
		hotSqlError(robot, "CREATE TABLE Fresh (name String, 'a=b' String);\n");
	TH_CHECK(badCol.find("列名") != std::string::npos);
	TH_CHECK(badCol.find("不是安全标识符") != std::string::npos);

	TH_CHECK(robot.matrices().empty());	/* 两次都在活体之外被拒 */
	robot.deactivate();
}

/* 不存在的驱动模块：启动期要到 build 才以 loadfile 失败暴露，热插提前折成
 * 一句点名表名的人话（判据与 createLuaVars 同一来源）。 */
TH_TEST(robot_hot_sql_rejects_a_table_without_a_driver_module) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-nomodule");

	bot::MemoryReader sr("");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = hotSqlError(robot,
		"CREATE TABLE NoSuchDriver9 (name String);\n"
		"INSERT INTO NoSuchDriver9 (name) VALUES ('x0');\n");
	TH_CHECK(msg.find("模块路径下找不到它") != std::string::npos);
	TH_CHECK(msg.find("NoSuchDriver9") != std::string::npos);
	TH_CHECK(robot.matrices().empty());
	robot.deactivate();
}

/* 守护 chunk：启动期脚本可信故允许绝对路径，热插不可信——放行即等于让模型
 * 指定任意 Lua 文件当守护 chunk 加载。 */
TH_TEST(robot_hot_sql_rejects_a_daemon_chunk_outside_the_module_path) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-daemon-chunk");

	bot::MemoryReader sr("");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = hotSqlError(robot,
		"CREATE TABLE Daemon (name String, chunk String, interval String);\n"
		"INSERT INTO Daemon (name, chunk, interval) VALUES ('d9', '/tmp/evil.lua', '10');\n");
	TH_CHECK(msg.find("相对路径") != std::string::npos);
	TH_CHECK(msg.find("/tmp/evil.lua") != std::string::npos);
	TH_CHECK(robot.matrices().empty());
	robot.deactivate();
}

/* 空批次与未上电：前者是「SELECT 不该走热插」（只读查询另有出口），
 * 后者是「带电变更」这条语义的前置条件。 */
TH_TEST(robot_hot_sql_rejects_an_empty_batch_and_an_inactive_robot) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-precond");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);

	/* 未上电：结构脚本才是正确入口 */
	const std::string notActive =
		hotSqlError(robot, "INSERT INTO LifetimeProbe (name, path) VALUES ('p1', 'x');\n");
	TH_CHECK(notActive.find("需要机器人已上电常驻") != std::string::npos);
	TH_CHECK(!robot.declaresVariable("p1"));

	robot.active();
	const std::string readOnly = hotSqlError(robot, "SELECT * FROM LifetimeProbe;\n");
	TH_CHECK(readOnly.find("没有任何 CREATE/INSERT/DELETE/DROP/UPDATE 变更") != std::string::npos);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == 2);
	robot.deactivate();
}

/* 容器不变式（std::deque）白盒锚定：MetaProvider 持指向 matrices_ 元素的引用，
 * vector 的 push_back 扩容会让全部旧引用悬垂。这里直接钉住「新表入列后旧元素
 * 的地址与内容一字未动」——改成 vector 会立刻红。 */
TH_TEST(robot_hot_sql_keeps_element_references_stable_across_appends) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-sql-stable");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();

	const mt::MatrixObject* probe = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK(probe != nullptr && probe->rows() == 2);
	const std::string* header = &(*probe)(std::size_t(0), std::size_t(0));

	robot.applySql("CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n");

	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") == probe);
	TH_CHECK(probe->rows() == 2);
	TH_CHECK(&(*probe)(std::size_t(0), std::size_t(0)) == header);
	TH_CHECK(*header == "name");
	robot.deactivate();
}

/* UPDATE（2026-09-23）：解析已进方言；带电生效（断电重建）由下方
 * robot_hot_update_* 组守门，此处不再保留过渡用例。 */

/* ==================== 热删除：DELETE FROM t [WHERE ... AND ...] ====================
 * （2026-09-23）三条判据贯穿本组，与热插表组同构：
 *   ① 删除 = 真的逆序 destroy（LifetimeProbe 的 'D' 落痕是物理证据）；
 *   ② 一致性 = 结构矩阵、注册表（名字索引 + 销毁序）、MetaProvider/守护登记
 *      四者在同一次操作后互相自洽；
 *   ③ 拒绝 = 活体一字未动，且报错点名成因（引用完整性是这里的核心）。
 * 「不可逆」这条口径另有专门用例：物理断电没有回滚，中途失败如实报数。 */

/* 删既有行：变量从注册表与销毁序里消失，器件当场断电；此后 deactivate
 * 不得对它重复 destroy（它已不在销毁序里）——这正是「摘干净」的可观测判据。 */
TH_TEST(robot_hot_delete_removes_an_existing_row_and_powers_it_off) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-row");
	const fs::path marker = tmp.path() / "destroyed";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();
	TH_CHECK(robot.registry().contains("p0"));
	const std::size_t tablesBefore = robot.matrices().size();

	const std::string msg = robot.applySql("DELETE FROM LifetimeProbe WHERE name = 'p0';\n");

	/* 删除当场断电：'D' 已落下，而不是等到 deactivate */
	TH_CHECK_MSG(markerOf(marker) == "D", "实际落痕: " + markerOf(marker));
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(!robot.declaresVariable("p0"));
	TH_CHECK(robot.registry().byName().empty());
	TH_CHECK(robot.registry().sequence().empty());
	/* 表还在（删的是行不是表），只剩列头 */
	TH_CHECK(robot.matrices().size() == tablesBefore);
	const mt::MatrixObject* m = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK(m != nullptr && m->rows() == 1);
	/* 摘要如实报出删了谁 */
	TH_CHECK_MSG(msg.find("已删除硬件变量 1 个") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("p0") != std::string::npos, "实际: " + msg);

	/* 已摘除的变量不在销毁序里：deactivate 不得重复断电（落痕仍恰好 1 个 'D'） */
	robot.deactivate();
	TH_CHECK_MSG(markerOf(marker) == "D", "重复断电的落痕: " + markerOf(marker));
}

/* 多条件 AND：只有同时满足全部条件的行被删——AND 与 OR 的分界在热删除路径上
 * 尤其重要（删错行是物理不可逆的）。 */
TH_TEST(robot_hot_delete_and_conditions_select_exactly_one_row) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-and");
	const fs::path m1 = tmp.path() / "d1";
	const fs::path m2 = tmp.path() / "d2";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String, tag String);\n"
		"INSERT INTO LifetimeProbe (name, path, tag) VALUES ('p0', '" + m1.string() + "', 'a');\n"
		"INSERT INTO LifetimeProbe (name, path, tag) VALUES ('p1', '" + m2.string() + "', 'b');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	/* tag='a' 只有 p0；再加一条 name='p1' 让两个条件互斥 → 后者不存在这种行 */
	const std::string msg = robot.applySql(
		"DELETE FROM LifetimeProbe WHERE tag = 'a' AND name = 'p0';\n");
	TH_CHECK_MSG(msg.find("已删除硬件变量 1 个（p0）") != std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(robot.registry().contains("p1"));	/* 不满足全部条件的行必须留下 */
	TH_CHECK(markerOf(m1) == "D");
	TH_CHECK(markerOf(m2).empty());			/* p1 根本没被碰过 */

	/* 条件互斥（AND 语义）→ 一条都不命中 → 报错且活体不动 */
	const std::string err = hotSqlError(robot,
		"DELETE FROM LifetimeProbe WHERE tag = 'b' AND name = 'p0';\n");
	TH_CHECK_MSG(err.find("no row matches") != std::string::npos, "实际: " + err);
	TH_CHECK(robot.registry().contains("p1"));
	TH_CHECK(markerOf(m2).empty());

	robot.deactivate();
}

/* 无条件 = 删空该表（SQL 语义）：命中全部数据行，一次全删。 */
TH_TEST(robot_hot_delete_without_where_removes_every_row) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-all");
	const fs::path m1 = tmp.path() / "d1";
	const fs::path m2 = tmp.path() / "d2";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + m1.string() + "');\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + m2.string() + "');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = robot.applySql("DELETE FROM LifetimeProbe;\n");
	TH_CHECK_MSG(msg.find("已删除硬件变量 2 个") != std::string::npos, "实际: " + msg);
	TH_CHECK(robot.registry().byName().empty());
	TH_CHECK(markerOf(m1) == "D" && markerOf(m2) == "D");
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == 1);	/* 只剩列头 */

	/* 空表的无条件删除：没有可删的行 → 报错而不是静默成功 */
	const std::string err = hotSqlError(robot, "DELETE FROM LifetimeProbe;\n");
	TH_CHECK_MSG(err.find("no row matches") != std::string::npos, "实际: " + err);

	robot.deactivate();
}

/* 批内先插后删：INSERT 与 DELETE 在影子容器上按语句序交错，故后一句能命中
 * 前一句刚插的行——新表最终是**空的**，一个变量都没长出来。
 * 这条守的是「删除生效」而不是「删掉了一个变量」：那些行从未装配、从未上电，
 * 撤销范围为空，但**绝不能**跟着新表一起装配出一个已被删掉的器件。 */
TH_TEST(robot_hot_delete_can_remove_a_row_inserted_in_the_same_batch) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-same-batch");
	const fs::path marker = tmp.path() / "d1";

	bot::MemoryReader sr("CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n");
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t rowsBefore = mt::findByName(robot.matrices(), "Timer")->rows();

	const std::string msg = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p9', '" + marker.string() + "');\n"
		"DELETE FROM LifetimeProbe WHERE name = 'p9';\n");

	/* 新表建了，但行被删掉 → 0 个变量、0 行数据 */
	TH_CHECK_MSG(msg.find("新增表 1 张") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("新增硬件变量 0 个") != std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("p9"));
	const mt::MatrixObject* m = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK_MSG(m != nullptr && m->rows() == 1, "新表应只剩列头（行被删干净）");
	/* 该表的行在装配之前就被删掉了 → 从未 build → 不留断电落痕 */
	TH_CHECK_MSG(markerOf(marker).empty(), "未装配的行不该有断电落痕: " + markerOf(marker));
	/* 无关的表一字不动 */
	TH_CHECK(mt::findByName(robot.matrices(), "Timer")->rows() == rowsBefore);
	TH_CHECK(robot.registry().contains("t0"));

	robot.deactivate();
}

/* 引用完整性（本功能的安全核心）：存活结构里引用着待删变量时必须拒绝，
 * 且报错**点名**是哪张表哪一行的哪一列。三类引用各一条：
 *   a) 流列 controller/trigger（MtVars_Lua::ctrlPtrs_/trigPtrs_ 终身缓存裸指针）
 *   b) 元数据列的字面 $name 引用（MetaRef::target 在 seed 期锁定）
 *   c) 元数据列的 $(...) 表达式（标识符按名求值）
 * 三条都必须「活体一字未动」：器件不能掉电、结构不能少行。 */
TH_TEST(robot_hot_delete_rejects_a_variable_still_referenced_by_a_controller) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-refctrl");
	const fs::path mp = tmp.path() / "probe";
	const fs::path mt = tmp.path() / "timer";

	bot::MemoryReader sr(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n"
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE dummy (name String, controller String);\n"
		"INSERT INTO dummy (name, controller) VALUES ('dm', 'p0');\n");
	(void)mt;
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string err = hotSqlError(robot, "DELETE FROM LifetimeProbe WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("控制器") != std::string::npos
		|| err.find("controller") != std::string::npos, "实际: " + err);
	TH_CHECK_MSG(err.find("dummy") != std::string::npos, "报错须点名引用方所在表: " + err);
	TH_CHECK_MSG(err.find("p0") != std::string::npos, "报错须点名被引用变量: " + err);
	/* 活体一字未动 */
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(markerOf(mp).empty());
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == 2);
	robot.deactivate();
}

TH_TEST(robot_hot_delete_rejects_a_variable_still_referenced_by_a_trigger_column) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-reftrig");
	const fs::path mp = tmp.path() / "probe";

	bot::MemoryReader sr(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n"
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE dummy (name String, trigger2 String);\n"
		"INSERT INTO dummy (name, trigger2) VALUES ('dm', 'p0');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	/* trigger2 是**编号列**：判据必须走 isFlowColumn 单点，自带一份就会漏掉它
	 * 而留下悬垂指针——这条用例专门钉那个单点。 */
	const std::string err = hotSqlError(robot, "DELETE FROM LifetimeProbe WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("p0") != std::string::npos, "实际: " + err);
	TH_CHECK_MSG(err.find("trigger2") != std::string::npos, "编号流列也必须被认出: " + err);
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(markerOf(mp).empty());
	robot.deactivate();
}

TH_TEST(robot_hot_delete_rejects_a_variable_referenced_by_a_dollar_ref) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-refdollar");
	const fs::path mp = tmp.path() / "probe";

	/* Monitor 的 src='$p0'：典型的跨表元数据引用（seed 期锁定 MetaRef::target） */
	bot::MemoryReader sr(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n"
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE Monitor (name String, src String);\n"
		"INSERT INTO Monitor (name, src) VALUES ('mon0', '$p0');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string err = hotSqlError(robot, "DELETE FROM LifetimeProbe WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("$p0") != std::string::npos, "报错须回显引用原文: " + err);
	TH_CHECK_MSG(err.find("Monitor") != std::string::npos, "报错须点名引用方所在表: " + err);
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(markerOf(mp).empty());
	robot.deactivate();
}

TH_TEST(robot_hot_delete_rejects_a_variable_referenced_by_an_expression) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-refexpr");
	const fs::path mp = tmp.path() / "probe";

	/* $(v0+1) 形式：标识符经注册表按名求值，目标消失即结构已坏 */
	bot::MemoryReader sr(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n"
		"CREATE TABLE Vars (name String, value String);\n"
		"INSERT INTO Vars (name, value) VALUES ('v0', '5');\n"
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE dummy (name String, delay String);\n"
		"INSERT INTO dummy (name, delay) VALUES ('dm', '$(v0+1)');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string err = hotSqlError(robot, "DELETE FROM Vars WHERE name = 'v0';\n");
	TH_CHECK_MSG(err.find("v0") != std::string::npos, "实际: " + err);
	TH_CHECK_MSG(err.find("dummy") != std::string::npos, "报错须点名引用方所在表: " + err);
	TH_CHECK(robot.registry().contains("v0"));
	robot.deactivate();
}

/* 引用方与被引用者**一起删**时不该拒绝：引用随行一起消失，不构成悬垂。
 * 这条是上面几条拒绝用例的对照组——判据是「存活行里是否还有引用」，
 * 不是「曾经有没有引用」。 */
TH_TEST(robot_hot_delete_allows_deleting_the_referrer_together_with_its_target) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-ref-together");
	const fs::path mp = tmp.path() / "probe";

	bot::MemoryReader sr(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('timer-0');\n"
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE dummy (name String, controller String);\n"
		"INSERT INTO dummy (name, controller) VALUES ('dm', 'p0');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	/* 两行同批删：先删引用方（dummy.dm）再删目标（p0），故任何时刻存活行里
	 * 都没有悬垂引用——这正是把「幸存的引用方」而不是「全部引用方」作为
	 * 判据的意义。注意顺序无关：预检查的是**删除之后**的视图。 */
	const std::string msg = robot.applySql(
		"DELETE FROM LifetimeProbe WHERE name = 'p0';\n"
		"DELETE FROM dummy WHERE name = 'dm';\n");
	TH_CHECK_MSG(msg.find("已删除硬件变量 2 个") != std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(!robot.registry().contains("dm"));
	TH_CHECK(robot.registry().contains("timer-0"));	/* 无关的变量一字不动 */
	TH_CHECK(robot.registry().byName().size() == 1);
	TH_CHECK(markerOf(mp) == "D");
	robot.deactivate();
}

/* 守护行删除：守护线程必须**先退场**再摘除登记槽，否则它会继续往一个已
 * 断电的采样对象上写。判据是登记槽消失（daemonHolders_ 按保留项重建），
 * 且过程不挂死（屏障等待不等满超时）。 */
TH_TEST(robot_hot_delete_removes_a_daemon_row_and_stops_its_thread) {
	ModulePathGuard guard("modules:test/cpp:modules");
	TempDir tmp("hot-delete-daemon");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr(
		"CREATE TABLE Daemon (name String, chunk String, interval String);\n"
		"INSERT INTO Daemon (name, chunk, interval) VALUES ('d0', 'LifetimeTick.lua', '10');\n"
		+ probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();
	TH_CHECK(robot.registry().contains("d0"));

	const std::string msg = robot.applySql("DELETE FROM Daemon WHERE name = 'd0';\n");
	TH_CHECK_MSG(msg.find("已删除硬件变量 1 个（d0）") != std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("d0"));
	/* 守护行没了，但同表的其它行（若有）与别的表一字不动 */
	TH_CHECK(mt::findByName(robot.matrices(), "Daemon")->rows() == 1);
	TH_CHECK(robot.registry().contains("p0"));
	/* 守护退场后 `deactivate` 不得因已摘除的守护出错，也不重复断电 */
	robot.deactivate();
	TH_CHECK(markerOf(marker).find('D') != std::string::npos);
}

/* 未上电即拒：与热插表同一条前置（热删除同样是带电变更），且必须在任何
 * destroy 之前失败。 */
TH_TEST(robot_hot_delete_requires_an_active_robot) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-inactive");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	const std::string err = hotSqlError(robot, "DELETE FROM LifetimeProbe WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("需要机器人已上电常驻") != std::string::npos, "实际: " + err);
	TH_CHECK(markerOf(marker).empty());
}

/* 目标表不存在 / 结构脚本里写 DELETE：两条都要给人话，且都不碰活体。
 * 后者是 allowDelete 缺省关的守门——同一个 readSQL 服务启动路径与热插路径，
 * 缺省关保证启动路径的接受语言逐字不变。 */
TH_TEST(robot_hot_delete_rejects_unknown_table_and_script_time_delete) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-badsql");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string unknown = hotSqlError(robot, "DELETE FROM Nope WHERE name = 'x';\n");
	TH_CHECK_MSG(unknown.find("table is not defined") != std::string::npos, "实际: " + unknown);
	TH_CHECK(robot.registry().contains("p0"));
	robot.deactivate();

	/* 启动期（结构脚本）里的 DELETE：机器人还没上电，没有可变的结构 */
	bot::MemoryReader sr2(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"DELETE FROM Timer WHERE name = 't0';\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "cannot appear in a structure script",
		bot::RobotStruct(sr2));
}

/* 同一批次里既删又插：两条路径各自生效、互不串味，摘要同时报出两侧。 */
TH_TEST(robot_hot_delete_mixes_with_insert_in_one_batch) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-delete-mixed");
	const fs::path oldM = tmp.path() / "old";
	const fs::path newM = tmp.path() / "new";

	bot::MemoryReader sr(probeSql(oldM));
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = robot.applySql(
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + newM.string() + "');\n"
		"DELETE FROM LifetimeProbe WHERE name = 'p0';\n");

	/* 新增与删除是**两个维度**，必须各自如实报数：同批删一插一时净增量为 0，
	 * 用「事后总数 - 批次前总数」算就会谎报「新增 0 个」。 */
	TH_CHECK_MSG(msg.find("新增硬件变量 1 个") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("既有表追加 1 行") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("已删除硬件变量 1 个（p0）") != std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(robot.registry().contains("p1"));
	TH_CHECK(markerOf(oldM) == "D");	/* 删掉的当场断电 */
	TH_CHECK(markerOf(newM).empty());	/* 新增的带电常驻，不落痕 */
	/* 表内净行数不变（删 1 插 1），且 p1 补在被删的 p0 之后 */
	const mt::MatrixObject* m = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK(m != nullptr && m->rows() == 2);
	TH_CHECK(fx::cell(*m, 1, 0) == "p1");
	robot.deactivate();
	TH_CHECK(markerOf(newM) == "D");	/* 退出时新增的才断电 */
}

/* ==================== 整表撤下：DROP TABLE t（2026-09-23）====================
 * DROP = DELETE 的「全表行」+「摘表壳」两步。因而本组的三条判据与热删除组同构
 * （真断电 / 四视图自洽 / 拒绝时活体一字未动），另加一条**只有整表撤下才会碰到**
 * 的不变式：表壳从 matrices_（deque）上 erase 会让其后元素引用失效，存活表的
 * MetaProvider 必须被重锚定——否则后续 read/destroy 踩悬垂指针。 */

/* 撤整表：该表全部变量当场断电、注册表/销毁序清空、表壳从结构里消失；
 * 其它表一字不动。已摘除的变量不在销毁序里，deactivate 不得重复断电。 */
TH_TEST(robot_hot_drop_removes_the_whole_table_and_powers_off_all_rows) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-table");
	const fs::path m1 = tmp.path() / "d1";
	const fs::path m2 = tmp.path() / "d2";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + m1.string() + "');\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + m2.string() + "');\n"
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n");
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t tablesBefore = robot.matrices().size();

	const std::string msg = robot.applySql("DROP TABLE LifetimeProbe;\n");

	/* 当场断电：两行都落下 'D'，而不是等到 deactivate */
	TH_CHECK_MSG(markerOf(m1) == "D" && markerOf(m2) == "D",
		"实际 m1=" + markerOf(m1) + " m2=" + markerOf(m2));
	/* 表壳消失、变量与名字索引清空；无关的表与变量一字不动 */
	TH_CHECK(robot.matrices().size() == tablesBefore - 1);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") == nullptr);
	TH_CHECK(!robot.registry().contains("p0") && !robot.registry().contains("p1"));
	TH_CHECK(!robot.declaresVariable("p0"));
	TH_CHECK(robot.registry().contains("t0"));
	TH_CHECK(mt::findByName(robot.matrices(), "Timer") != nullptr);
	/* 摘要如实报出撤下的表名 */
	TH_CHECK_MSG(msg.find("已撤下表 1 张") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("LifetimeProbe") != std::string::npos, "实际: " + msg);

	/* 已摘除的变量不在销毁序里：deactivate 不得重复断电（落痕仍恰好 1 个 'D'） */
	robot.deactivate();
	TH_CHECK(markerOf(m1) == "D" && markerOf(m2) == "D");
}

/* DROP 不带 WHERE（DDL 作用于整张表）：无条件撤下全部行。空表（只有列头）
 * 撤下也是合法操作——不是「一条都没删到」，故不报错。 */
TH_TEST(robot_hot_drop_is_unconditional_and_allows_an_empty_table) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-uncond");
	const fs::path m1 = tmp.path() / "d1";
	const fs::path m2 = tmp.path() / "d2";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + m1.string() + "');\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + m2.string() + "');\n"
		/* Timer 只有列头，没有任何数据行——撤它必须照样成功 */
		"CREATE TABLE Timer (name String);\n");
	bot::RobotStruct robot(sr);
	robot.active();

	/* 空表：无条件撤下成功（不是 "no row matches" 之列） */
	const std::string emptyMsg = robot.applySql("DROP TABLE Timer;\n");
	TH_CHECK_MSG(emptyMsg.find("已撤下表 1 张") != std::string::npos, "实际: " + emptyMsg);
	TH_CHECK(mt::findByName(robot.matrices(), "Timer") == nullptr);

	/* 有数据的表：整表两行都当场断电 */
	const std::string msg = robot.applySql("DROP TABLE LifetimeProbe;\n");
	TH_CHECK_MSG(msg.find("已撤下表 1 张") != std::string::npos, "实际: " + msg);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") == nullptr);
	TH_CHECK(robot.registry().byName().empty());
	TH_CHECK(markerOf(m1) == "D" && markerOf(m2) == "D");

	robot.deactivate();
}

/* WHERE 不是 DROP 的语法：误写 WHERE 在解析期就被词元级报错拒绝，
 * 活体一字未动（这是「DDL 不筛行」的守门）。 */
TH_TEST(robot_hot_drop_rejects_a_where_clause) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-where");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string err = hotSqlError(robot, "DROP TABLE LifetimeProbe WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("Token WHERE") != std::string::npos, "实际: " + err);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") != nullptr);
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(markerOf(marker).empty());

	robot.deactivate();
}

/* 引用完整性：撤表会带走该表全部变量，故别的表若仍引用它们必须拒绝
 * （与 DELETE 同一条防线，报错点名「哪张表哪一行的哪一列引用了它」）。 */
TH_TEST(robot_hot_drop_rejects_a_variable_still_referenced_by_another_table) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-ref");
	const fs::path mp = tmp.path() / "probe";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE dummy (name String, controller String);\n"
		"INSERT INTO dummy (name, controller) VALUES ('dm', 'p0');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string err = hotSqlError(robot, "DROP TABLE LifetimeProbe;\n");
	TH_CHECK_MSG(err.find("热删除被拒绝") != std::string::npos, "实际: " + err);
	/* 活体一字未动：表在、变量在、器件未断电 */
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") != nullptr);
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(markerOf(mp).empty());

	robot.deactivate();
}

/* **整表撤下独有的不变式**：erase 表壳会让其后元素引用失效，存活表的
 * MetaProvider 必须被重锚定——否则读取存活变量的元数据会踩悬垂指针。
 * 夹具刻意把被撤的表放在 survive 表**之前**（它后面的元素才受 erase 影响），
 * 然后读存活变量的元数据：重锚定正确则返回 result=OK，悬垂则读到错位内容或崩。
 * （悬垂的最强证据在 sanitizer 构建：gate:sanitizer / gate:tsan。） */
TH_TEST(robot_hot_drop_rebases_meta_providers_of_surviving_tables) {
	ModulePathGuard guard("modules:test/cpp:modules");
	TempDir tmp("hot-drop-rebase");
	const fs::path marker = tmp.path() / "probe";

	/* dummy 在前（将被撤）、LifetimeProbe 在后（存活，其 provider 必须重锚定） */
	bot::MemoryReader sr(
		"CREATE TABLE dummy (name String);\n"
		"INSERT INTO dummy (name) VALUES ('dm');\n"
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + marker.string() + "');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	robot.applySql("DROP TABLE dummy;\n");
	TH_CHECK(mt::findByName(robot.matrices(), "dummy") == nullptr);
	TH_CHECK(robot.registry().contains("p0"));

	/* 读存活变量的元数据：走存活表（已被 rebase）的 MetaProvider */
	bot::MatrixVars_Hardware* p0 = robot.registry().find("p0");
	TH_CHECK(p0 != nullptr);
	bot::Json md = p0->read();
	TH_CHECK_MSG(md.isObj(), "存活变量读回应是结果表");
	if (md.isObj()) {
		const auto r = md.get("result");
		TH_CHECK_MSG(r.has_value() && r->getValue() == "OK",
			"存活变量读回 result 应为 OK");
	}

	robot.deactivate();
	TH_CHECK(markerOf(marker) == "D");	/* 存活表的器件照常断电 */
}

/* 同一批次内先 CREATE 再 DROP：整表从未装配、从未上电，最终什么都不长出来。 */
TH_TEST(robot_hot_drop_of_a_table_created_in_the_same_batch_leaves_nothing) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-same-batch");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr("CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p9', '" + marker.string() + "');\n"
		"DROP TABLE LifetimeProbe;\n");

	/* 表建了又被撤：一个变量都没长出来，表壳也不在结构里。摘除发生在装配之前，
	 * 故不是「新增后再删」而是「净效果为空」——摘要里连「新增表」都不出现。 */
	TH_CHECK_MSG(msg.find("新增硬件变量 0 个") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("新增表") == std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("p9"));
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") == nullptr);
	/* 从未装配 → 从未 build → 不留断电落痕 */
	TH_CHECK_MSG(markerOf(marker).empty(), "未装配的表不该有断电落痕: " + markerOf(marker));
	TH_CHECK(robot.registry().contains("t0"));

	robot.deactivate();
}

/* 目标表不存在 / 结构脚本里写 DROP：两条都要给人话，且都不碰活体。 */
TH_TEST(robot_hot_drop_rejects_script_time_and_unknown_table) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-badsql");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string unknown = hotSqlError(robot, "DROP TABLE Nope;\n");
	TH_CHECK_MSG(unknown.find("table is not defined") != std::string::npos, "实际: " + unknown);
	TH_CHECK(robot.registry().contains("p0"));
	robot.deactivate();

	/* 启动期（结构脚本）里的 DROP：机器人还没上电，没有可变的结构 */
	bot::MemoryReader sr2(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"DROP TABLE Timer;\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "cannot appear in a structure script",
		bot::RobotStruct(sr2));
}

/* 未上电即拒：与热插/热删除同一条前置，且必须在任何 destroy 之前失败。 */
TH_TEST(robot_hot_drop_requires_an_active_robot) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-inactive");
	const fs::path marker = tmp.path() / "probe";

	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	const std::string err = hotSqlError(robot, "DROP TABLE LifetimeProbe;\n");
	TH_CHECK_MSG(err.find("需要机器人已上电常驻") != std::string::npos, "实际: " + err);
	TH_CHECK(markerOf(marker).empty());
}

/* 撤表后同名重建（DROP 打开的新语义面，见 sqlparse.cpp 的说明）：撤下之后
 * CREATE 同名表应被允许（表名不再冲突），且装配出的是全新变量。 */
TH_TEST(robot_hot_drop_allows_recreating_a_table_with_the_same_name) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-drop-recreate");
	const fs::path m1 = tmp.path() / "d1";
	const fs::path m2 = tmp.path() / "d2";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + m1.string() + "');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	robot.applySql("DROP TABLE LifetimeProbe;\n");
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(markerOf(m1) == "D");

	/* 同名重建：表名不再冲突，新变量照常装配 */
	const std::string msg = robot.applySql(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + m2.string() + "');\n");
	TH_CHECK_MSG(msg.find("新增表 1 张") != std::string::npos, "实际: " + msg);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe") != nullptr);
	TH_CHECK(robot.registry().contains("p1"));
	TH_CHECK(markerOf(m2).empty());	/* 新器件带电常驻 */

	robot.deactivate();
	TH_CHECK(markerOf(m2) == "D");
}


/* ==================== 热更新：UPDATE t SET ... [WHERE ...]（2026-09-23）====================
 * 语义是**断电重建**：命中行按「DELETE（物理断电摘除）+ INSERT（改后行重新装配
 * 上电）」两条既有路径执行，不是原地改值。本组与热删除组同构的三条判据：
 *   ① 生效 = 旧行真的当场断电（探针落痕）+ 新行真的带电常驻（改后值可观测）；
 *   ② 拒绝 = 活体一字未动（守门全部在预检，报错点名成因）；
 *   ③ 先删后建的失败口径 = 已摘旧行**不复原**，错误点名丢失的变量（不谎报回滚）。
 * 另有两条方言级守门（批内独占 / 结构脚本期拒绝）钉在 readSQL。
 * 物理证据链：LifetimeProbe 的 destroy 向 data.path 追加 'D'——改 path 的更新
 * 会把「旧件的断电落痕」留在旧路径、把「新件的最终断电」留在新路径，两条路径
 * 各一个 'D' 正是「断电重建」两个阶段的直接观测。 */

/* 基本：同名行改值。重名预检的 doomed 豁免、先删后建序、摘要三段式都在此钉住。 */
TH_TEST(robot_hot_update_rewrites_a_row_by_power_cycle) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-basic");
	const fs::path oldM = tmp.path() / "old";
	const fs::path newM = tmp.path() / "new";

	bot::MemoryReader sr(probeSql(oldM));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t namesBefore = robot.registry().byName().size();

	/* 多赋值（path 与 name 各一条；name 赋原值 = 不改名，只验证多赋值通道） */
	const std::string msg = robot.applySql(
		"UPDATE LifetimeProbe SET path = '" + newM.string() + "', name = 'p0'\n"
		"WHERE name = 'p0';\n");

	/* 摘要三段式：断电重建的物理事实 = 删 1 + 增 1 + 热更新 1（三个数相互对账） */
	TH_CHECK_MSG(msg.find("已热更新 1 行（断电重建）") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("新增硬件变量 1 个") != std::string::npos, "实际: " + msg);
	TH_CHECK_MSG(msg.find("已删除硬件变量 1 个（p0）") != std::string::npos, "实际: " + msg);
	/* 同名重建：变量仍可寻址，注册表/销毁序规模不变 */
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(robot.declaresVariable("p0"));
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	TH_CHECK(robot.registry().sequence().size() == namesBefore);
	/* 矩阵行数不变（1 删 1 增），改后行的 path 已是新值 */
	const mt::MatrixObject* m = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK(m != nullptr && m->rows() == 2);
	TH_CHECK(fx::cell(*m, 1, 1) == newM.string());
	/* 先删后建的物理证据：旧件在更新**当场**断电（旧路径立即落痕），
	 * 新件带电常驻（新路径此刻无痕） */
	TH_CHECK(markerOf(oldM) == "D");
	TH_CHECK(markerOf(newM).empty());
	robot.deactivate();
	TH_CHECK(markerOf(newM) == "D");
	TH_CHECK(markerOf(oldM) == "D");	/* 旧件只断电一次，不会被二次 destroy */
}

/* WHERE 选行与无 WHERE 全表改写：只有命中行被断电重建。 */
TH_TEST(robot_hot_update_where_selects_rows_and_no_where_rewrites_all) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-where");
	const fs::path mA = tmp.path() / "a";
	const fs::path mB = tmp.path() / "b";
	const fs::path mC = tmp.path() / "c";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mA.string() + "');\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p1', '" + mB.string() + "');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = robot.applySql(
		"UPDATE LifetimeProbe SET path = '" + mC.string() + "' WHERE name = 'p1';\n");
	TH_CHECK_MSG(msg.find("已热更新 1 行") != std::string::npos, "实际: " + msg);
	/* 只有 p1 被断电重建：p1 旧件当场断电（mB 落痕），p0 的 mA 无痕 */
	TH_CHECK(markerOf(mB) == "D");
	TH_CHECK(markerOf(mA).empty());
	const mt::MatrixObject* m = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK(m != nullptr && m->rows() == 3);	/* 列头 + p0 + 改后 p1 */
	TH_CHECK(fx::cell(*m, 1, 1) == mA.string());	/* 未命中行原样 */
	TH_CHECK(fx::cell(*m, 2, 1) == mC.string());	/* 命中行已是新值 */

	/* 无 WHERE = 全部数据行（与 DELETE「无条件删全表」同口径） */
	const std::string all = robot.applySql(
		"UPDATE LifetimeProbe SET path = '" + mC.string() + "';\n");
	TH_CHECK_MSG(all.find("已热更新 2 行") != std::string::npos, "实际: " + all);
	TH_CHECK_MSG(all.find("已删除硬件变量 2 个") != std::string::npos, "实际: " + all);
	TH_CHECK(markerOf(mA) == "D");	/* p0 这次也被断电重建（旧值路径落痕） */
	robot.deactivate();
	/* mC 的三个 'D'：第一次改写的新 p1（第二次改写时被断电）+ 退出时两个新件 */
	TH_CHECK(markerOf(mC) == "DDD");
}

/* 改名：SET name = 新名。旧名随断电摘除消失，新名可寻址。 */
TH_TEST(robot_hot_update_renames_a_row_via_set_name) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-rename");
	const fs::path m = tmp.path() / "d";

	bot::MemoryReader sr(probeSql(m));
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string msg = robot.applySql(
		"UPDATE LifetimeProbe SET name = 'p9' WHERE name = 'p0';\n");
	TH_CHECK_MSG(msg.find("已热更新 1 行") != std::string::npos, "实际: " + msg);
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(!robot.declaresVariable("p0"));
	TH_CHECK(robot.registry().contains("p9"));
	const mt::MatrixObject* mm = mt::findByName(robot.matrices(), "LifetimeProbe");
	TH_CHECK(mm != nullptr && fx::cell(*mm, 1, 0) == "p9");
	/* 改名不改 path：旧件断电落痕仍在原路径 */
	TH_CHECK(markerOf(m) == "D");
	robot.deactivate();
	TH_CHECK(markerOf(m) == "DD");	/* 新件 path 未变，退出时再落一个 D */
}

/* 0 命中是响亮报错（口径同 DELETE），不是「成功但什么都没做」。 */
TH_TEST(robot_hot_update_no_match_is_a_loud_error) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-nomatch");
	const fs::path m = tmp.path() / "d";

	bot::MemoryReader sr(probeSql(m));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t namesBefore = robot.registry().byName().size();

	const std::string err = hotSqlError(robot,
		"UPDATE LifetimeProbe SET path = 'x' WHERE name = 'nope';\n");
	TH_CHECK_MSG(err.find("no row matches, nothing was updated") != std::string::npos,
		"实际: " + err);
	/* 活体一字未动 */
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == 2);
	TH_CHECK(markerOf(m).empty());
	robot.deactivate();
}

/* 被引用（controller 流列）的变量不得改写：改写 = 断电摘除，悬垂指针防线同热删除。 */
TH_TEST(robot_hot_update_refuses_a_row_still_referenced_by_a_controller) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-refctrl");
	const fs::path mp = tmp.path() / "probe";

	bot::MemoryReader sr(
		"CREATE TABLE LifetimeProbe (name String, path String);\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p0', '" + mp.string() + "');\n"
		"CREATE TABLE dummy (name String, controller String);\n"
		"INSERT INTO dummy (name, controller) VALUES ('dm', 'p0');\n");
	bot::RobotStruct robot(sr);
	robot.active();

	const std::string err = hotSqlError(robot,
		"UPDATE LifetimeProbe SET path = 'x' WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("热删除被拒绝") != std::string::npos, "实际: " + err);
	TH_CHECK_MSG(err.find("dummy") != std::string::npos, "报错须点名引用方所在表: " + err);
	TH_CHECK_MSG(err.find("p0") != std::string::npos, "报错须点名被引用变量: " + err);
	/* 活体一字未动 */
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(markerOf(mp).empty());
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == 2);
	robot.deactivate();
}

/* 改写行自己引用本批将摘除的名字（含旧名）：预检的扫描面必须覆盖**新增/改写行**
 * ——流引用与 $ 引用都是懒解析，预检不拦就是运行期的雷（先删后建下还会落进
 * 「旧件已摘、新件失败」的不可复原窗口）。 */
TH_TEST(robot_hot_update_refuses_a_rewrite_referencing_the_doomed_name) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-selfref");
	const fs::path m = tmp.path() / "d";

	bot::MemoryReader sr(probeSql(m));
	bot::RobotStruct robot(sr);
	robot.active();

	/* 改名 p0→p0x 的同时把 path 指向旧名 $p0：改后行引用本批将删的 p0 */
	const std::string err = hotSqlError(robot,
		"UPDATE LifetimeProbe SET name = 'p0x', path = '$p0' WHERE name = 'p0';\n");
	TH_CHECK_MSG(err.find("热删除被拒绝") != std::string::npos, "实际: " + err);
	TH_CHECK_MSG(err.find("$p0") != std::string::npos, "报错须点名引用形态: " + err);
	/* 活体一字未动（改名没有发生，$ 引用没有落进结构） */
	TH_CHECK(robot.registry().contains("p0"));
	TH_CHECK(!robot.registry().contains("p0x"));
	TH_CHECK(markerOf(m).empty());
	robot.deactivate();
}

/* 批内独占（readSQL 守门，四个方向）：UPDATE 与该表的其它行级语句不得同批。 */
TH_TEST(robot_hot_update_batch_exclusivity_guards) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-exclusive");
	const fs::path m = tmp.path() / "d";

	bot::MemoryReader sr(probeSql(m));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t namesBefore = robot.registry().byName().size();
	const std::size_t rowsBefore = mt::findByName(robot.matrices(), "LifetimeProbe")->rows();

	/* ① 同表先 INSERT 后 UPDATE：分解的尾块区间算式会被打乱 */
	const std::string e1 = hotSqlError(robot,
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p9', 'x');\n"
		"UPDATE LifetimeProbe SET path = 'y' WHERE name = 'p0';\n");
	TH_CHECK_MSG(e1.find("run UPDATE in a separate statement batch") != std::string::npos,
		"实际: " + e1);
	/* ② 同表先 UPDATE 后 INSERT */
	const std::string e2 = hotSqlError(robot,
		"UPDATE LifetimeProbe SET path = 'y' WHERE name = 'p0';\n"
		"INSERT INTO LifetimeProbe (name, path) VALUES ('p9', 'x');\n");
	TH_CHECK_MSG(e2.find("run INSERT in a separate statement batch") != std::string::npos,
		"实际: " + e2);
	/* ③ 同表双 UPDATE */
	const std::string e3 = hotSqlError(robot,
		"UPDATE LifetimeProbe SET path = 'y' WHERE name = 'p0';\n"
		"UPDATE LifetimeProbe SET path = 'z' WHERE name = 'p0';\n");
	TH_CHECK_MSG(e3.find("run UPDATE in a separate statement batch") != std::string::npos,
		"实际: " + e3);
	/* ④ 本批次新建的表：行从未装配，「改写」无从谈起 */
	const std::string e4 = hotSqlError(robot,
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"UPDATE Timer SET name = 't1' WHERE name = 't0';\n");
	TH_CHECK_MSG(e4.find("created earlier in this batch") != std::string::npos,
		"实际: " + e4);
	/* 活体一字未动：四批全部在预检被拒 */
	TH_CHECK(robot.registry().byName().size() == namesBefore);
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == rowsBefore);
	TH_CHECK(!robot.declaresVariable("p9"));
	TH_CHECK(!robot.declaresVariable("t0"));
	TH_CHECK(markerOf(m).empty());
	robot.deactivate();
	TH_CHECK(markerOf(m) == "D");
}

/* 方言级守门：结构脚本期拒绝（机器人尚未上电）；热插期未知表报错。 */
TH_TEST(robot_hot_update_rejects_script_time_and_unknown_table) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-badsql");
	const fs::path m = tmp.path() / "d";

	/* 启动期（结构脚本）里的 UPDATE：与 DELETE/DROP 同判据 */
	bot::MemoryReader sr2(
		"CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n"
		"UPDATE Timer SET name = 't1' WHERE name = 't0';\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "cannot appear in a structure script",
		bot::RobotStruct(sr2));

	bot::MemoryReader sr(probeSql(m));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::string unknown = hotSqlError(robot, "UPDATE Nope SET a = 1;\n");
	TH_CHECK_MSG(unknown.find("table is not defined") != std::string::npos, "实际: " + unknown);
	robot.deactivate();
}

/* 先删后建的失败口径：预检全过、删除已执行、新件装配失败（4.3 的 $ 引用校验）
 * ⇒ 新件回滚、旧件不复原，错误**点名丢失的变量**——不谎报「校验拒绝、活体未动」，
 * 也不静默假装回滚。异常类型必须是 runtime_error：成因虽是 IllegalArgumentException
 * （$ 引用不存在），但活体已经动过，paramError 口径对它是谎报。 */
TH_TEST(robot_hot_update_delete_first_failure_names_lost_variables) {
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("hot-update-lost");
	const fs::path m = tmp.path() / "d";

	bot::MemoryReader sr(probeSql(m));
	bot::RobotStruct robot(sr);
	robot.active();
	const std::size_t namesBefore = robot.registry().byName().size();

	/* $nosuch-var 不在 doomed 集（预检的引用扫描只对待删名），过了预检；
	 * 新行的 MetaProvider 在 4.3 validate 时才报「引用不存在」。 */
	std::string err;
	try {
		robot.applySql(
			"UPDATE LifetimeProbe SET path = '$nosuch-var' WHERE name = 'p0';\n");
	} catch (const std::exception& e) {
		err = e.what();
	}
	TH_CHECK_MSG(err.find("已断电摘除且未能重建") != std::string::npos, "实际: " + err);
	TH_CHECK_MSG(err.find("（p0）") != std::string::npos, "错误须点名丢失的变量: " + err);
	TH_CHECK_MSG(err.find("nosuch") != std::string::npos, "成因（$ 引用不存在）不得被掩盖: " + err);
	/* 如实：旧件已断电（落痕），新件未建成——p0 从注册表消失 */
	TH_CHECK(markerOf(m) == "D");
	TH_CHECK(!robot.registry().contains("p0"));
	TH_CHECK(robot.registry().byName().size() == namesBefore - 1);
	/* 矩阵行也回到删后状态（改后行已随回滚弹出） */
	TH_CHECK(mt::findByName(robot.matrices(), "LifetimeProbe")->rows() == 1);
	robot.deactivate();
}

/* ---- #34 条件反射（@Reflex 动作序列 + @Trigger 触发 + 临时变量绑定）----
 * 反射动作行里的 $(临时变量) 经 makeActionExprResolver 命中触发绑定的会话
 * @Alias；不绑定即报「expression references '<名>'」——成功/失败路径用
 * 同一断言锚定绑定是否生效。kBasicSql 的 relay0 是纯数据行（非硬件变量），
 * 派发期报 not defined，恰好作为「绑定已生效、走到派发」的观察点。 */
TH_TEST(robot_reflex_without_trigger_is_lazy) {
	/* @Reflex 引用未绑定变量，无 @Trigger → 反射定义惰性存在，不执行不校验 */
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Reflex\nname relay0.deg;\nblink $(undef);\n@\n");
	TH_CHECK(msg.empty());
}

TH_TEST(robot_reflex_trigger_binds_temporary_and_dispatches) {
	/* 绑定 bright=1 后 $(bright) 可解析 → 走到派发（relay0 非硬件变量报错） */
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Reflex\nname relay0.deg;\nblink $(bright);\n@\n"
		"@Trigger\ntrigger;\nblink(bright=1);\n@\n");
	TH_CHECK(msg.find("relay0") != std::string::npos);
	TH_CHECK(msg.find("not defined") != std::string::npos);
}

TH_TEST(robot_reflex_unbound_temporary_reports) {
	/* 触发没绑定 bright → $(bright) 解析失败，报表达式未定义（不落到派发） */
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Reflex\nname relay0.deg;\nblink $(bright);\n@\n"
		"@Trigger\ntrigger;\nblink(angle=1);\n@\n");
	TH_CHECK(msg.find("bright") != std::string::npos);
}

TH_TEST(robot_reflex_undefined_reflex_reports) {
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Reflex\nname relay0.deg;\nblink 1;\n@\n"
		"@Trigger\ntrigger;\nzzz(bright=1);\n@\n");
	TH_CHECK(msg.find("未定义的反射 'zzz'") != std::string::npos);
}

TH_TEST(robot_reflex_bad_arg_syntax_reports) {
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Reflex\nname relay0.deg;\nblink 1;\n@\n"
		"@Trigger\ntrigger;\nblink(bright);\n@\n");
	TH_CHECK(msg.find("k=v") != std::string::npos);
}

TH_TEST(robot_reflex_duplicate_arg_reports) {
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Reflex\nname relay0.deg;\nblink 1;\n@\n"
		"@Trigger\ntrigger;\nblink(bright=1,bright=2);\n@\n");
	TH_CHECK(msg.find("重复绑定参数 'bright'") != std::string::npos);
}

TH_TEST(robot_reflex_trigger_without_reflex_reports) {
	const std::string msg = runTexts(kBasicSql,
		"@Action\nrelay0.deg;\n@\n"
		"@Trigger\ntrigger;\nblink(bright=1);\n@\n");
	TH_CHECK(msg.find("@Reflex") != std::string::npos);
}

TH_TEST(robot_reflex_scope_does_not_leak) {
	/* 两次触发同一 run：第一次绑 bright（动作引用它，成功走完派发）；第二次
	 * 只绑 angle → $(bright) 未绑定报错 —— 证明第一次的绑定在触发后已撤销
	 * （子作用域不泄漏），也证明触发是逐行独立的。探针驱动 p0 可真实 play。 */
	ModulePathGuard guard("modules:test/cpp");
	TempDir tmp("reflex-scope");
	const fs::path marker = tmp.path() / "destroyed";
	bot::MemoryReader sr(probeSql(marker));
	bot::RobotStruct robot(sr);
	robot.active();	/* p0 探针上电注册：派发 play 需要真硬件变量在注册表 */
	bot::MemoryReader mr(
		"@Action\np0.probe;\n@\n"
		"@Reflex\nname p0.probe;\nblink $(bright);\n@\n"
		"@Trigger\ntrigger;\nblink(bright=1);\nblink(angle=1);\n@\n");
	try {
		robot.run(mr);
		TH_CHECK(false && "第二次触发应报未绑定 bright");
	} catch (const std::exception& e) {
		TH_CHECK(std::string(e.what()).find("bright") != std::string::npos);
	}
	robot.deactivate();
}
