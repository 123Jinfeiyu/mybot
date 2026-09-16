/* test_hardware.cpp —— 硬件变量基类 + 元数据收集（MetaProvider）
 *
 * 这两块是「结构脚本 → Lua 驱动入参」的唯一通路，此前只被 E2E 间接覆盖：
 *   parseMtDouble  动作矩阵单元格的类型解析（含 "-" 继承与首行占位符语义）
 *   ensureOk       halt-on-error 契约（result=="ERR" 即抛，trim 后比较）
 *   MetaProvider   字面列首采冻结 / "-" 列跳过 / name+controller 跳过 / $ 引用实时快照
 *   C1 防线        $ 引用成环时深度计数兜底，清晰报错而非栈溢出
 */

#include <map>
#include <memory>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "harness.hpp"
#include "hardware.hpp"
#include "json.hpp"
#include "matrix.hpp"
#include "robot.hpp"
#include "util.hpp"

using bot::Json;
using bot::JsonArray;
using bot::JsonBool;
using bot::JsonFloat;
using bot::JsonInt;
using bot::JsonNull;
using bot::JsonObject;
using bot::JsonString;
using bot::MatrixVars_Hardware;
using bot::MetaProvider;

/* ---------------- 基类生命周期 ---------------- */

TH_TEST(hardware_base_class_is_not_copyable) {
	/* 子类持有 lua_State* / 共享槽等裸句柄，隐式拷贝即双重释放 */
	TH_CHECK(!std::is_copy_constructible<MatrixVars_Hardware>::value);
	TH_CHECK(!std::is_copy_assignable<MatrixVars_Hardware>::value);
}

TH_TEST(hardware_build_destroy_are_optional_hooks) {
	int built = 0;
	int freed = 0;
	MatrixVars_Hardware bare([](const Json&) { return JsonObject(); });
	bare.build();
	bare.destroy();
	TH_CHECK(built == 0 && freed == 0);

	MatrixVars_Hardware hooked([](const Json&) { return JsonObject(); },
		[&built] { ++built; }, [&freed] { ++freed; });
	hooked.build();
	/* build/destroy 自身不做幂等保护：调用一次就转发一次，
	 * 「只 build 一次」由 RobotStruct::active() 那一层保证。 */
	hooked.build();
	hooked.destroy();
	TH_CHECK(built == 2 && freed == 1);
}

TH_TEST(hardware_read_defaults_to_play_with_empty_object) {
	/* P4/M1：read() 缺省实现 = play({})，驱动侧无需为只读快照另写入口 */
	Json seen = JsonNull();
	MatrixVars_Hardware v([&seen](const Json& in) {
		seen = in;
		Json out = JsonObject();
		out.put("count", JsonInt(3));
		return out;
	});
	Json snap = v.read();
	TH_CHECK(seen.isObj());
	TH_CHECK(seen.objSize() == 0);
	TH_CHECK(snap.get("count")->getInt() == 3);
}

/* ---------------- parseMtDouble ---------------- */

TH_TEST(parseMtDouble_reads_numeric_cells) {
	mt::MatrixObject m = fx::oneMatrix("@Action\nrelay0.deg;\n200;\n-3.5;\n1e2;\n@\n");
	TH_CHECK(bot::MatrixVars_Hardware::parseMtDouble(m, 1, 0) == 200.0);
	TH_CHECK(bot::MatrixVars_Hardware::parseMtDouble(m, 2, 0) == -3.5);
	TH_CHECK(bot::MatrixVars_Hardware::parseMtDouble(m, 3, 0) == 100.0);
}

TH_TEST(parseMtDouble_inherits_dash_placeholder) {
	mt::MatrixObject m = fx::oneMatrix("@Action\nrelay0.deg;\n200;\n-;\n-;\n@\n");
	/* 连续 "-" 逐行继承同一个值（前向填充，非递归） */
	TH_CHECK(bot::MatrixVars_Hardware::parseMtDouble(m, 2, 0) == 200.0);
	TH_CHECK(bot::MatrixVars_Hardware::parseMtDouble(m, 3, 0) == 200.0);
}

TH_TEST(parseMtDouble_first_row_dash_is_zero) {
	/* 首数据行无可继承 → 0.0（沿用 Cangjie 语义：动作矩阵首行留空即零位） */
	mt::MatrixObject m = fx::oneMatrix("@Action\nrelay0.deg;\n-;\n@\n");
	TH_CHECK(bot::MatrixVars_Hardware::parseMtDouble(m, 1, 0) == 0.0);
}

TH_TEST(parseMtDouble_rejects_non_numeric_cell) {
	/* 报错必须带上出错单元格原文，否则用户无法在网格里定位 */
	mt::MatrixObject m = fx::oneMatrix("@Action\nrelay0.deg;\nabc;\n@\n");
	TH_CHECK_THROWS_MSG(std::invalid_argument, "abc",
		bot::MatrixVars_Hardware::parseMtDouble(m, 1, 0));
	/* 尾部残留字符同样算非法（"12abc" 不能悄悄读成 12） */
	mt::MatrixObject m2 = fx::oneMatrix("@Action\nrelay0.deg;\n12abc;\n@\n");
	TH_CHECK_THROWS_MSG(std::invalid_argument, "12abc",
		bot::MatrixVars_Hardware::parseMtDouble(m2, 1, 0));
}

/* ---------------- ensureOk ---------------- */

TH_TEST(ensureOk_passes_through_non_error_values) {
	Json scalar = JsonInt(7);
	TH_CHECK(MatrixVars_Hardware::ensureOk(scalar).getInt() == 7);

	Json ok = JsonObject();
	ok.put("result", JsonString("OK"));
	ok.put("value", JsonFloat(1.25));
	TH_CHECK(MatrixVars_Hardware::ensureOk(ok, "play x").get("value")->getFloat() == 1.25);

	Json plain = JsonObject();
	TH_CHECK(MatrixVars_Hardware::ensureOk(plain).objSize() == 0);
}

TH_TEST(ensureOk_throws_on_err_contract) {
	Json err = JsonObject();
	err.put("result", JsonString("ERR"));
	err.put("message", JsonString("i2c nack"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "i2c nack",
		MatrixVars_Hardware::ensureOk(err, "play relay0"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "play relay0",
		MatrixVars_Hardware::ensureOk(err, "play relay0"));

	/* 驱动常把 result 写成 "ERR " 之类，trim 后仍须判失败 */
	Json padded = JsonObject();
	padded.put("result", JsonString("  ERR\t"));
	TH_CHECK_THROWS_MSG(std::runtime_error, "Hardware call failed",
		MatrixVars_Hardware::ensureOk(padded));
}

TH_TEST(ensureOk_does_not_mistake_substring_for_error) {
	Json j = JsonObject();
	j.put("result", JsonString("OK"));
	j.put("message", JsonString("device reported ERR internally"));
	TH_CHECK(MatrixVars_Hardware::ensureOk(j).isObj());
}

/* ---------------- jsonMergeInto ---------------- */

TH_TEST(json_merge_into_overwrites_in_place) {
	Json dst = JsonObject();
	dst.put("a", JsonInt(1));
	dst.put("b", JsonInt(2));
	Json src = JsonObject();
	src.put("b", JsonString("B"));
	src.put("c", JsonInt(3));
	bot::jsonMergeInto(dst, src);
	/* 浅合并：同名覆盖且保持原位置，新键追加在尾部 */
	TH_CHECK(dst.toString() == R"({"a":1,"b":"B","c":3})");
}

/* ---------------- MetaProvider ---------------- */

TH_TEST(metaprovider_skips_name_controller_and_dash_columns) {
	mt::MatrixObject m = fx::oneMatrix(
		"@Relay\nname controller note servo;\nr0 t0 hello -;\n@\n");
	std::map<std::string, MatrixVars_Hardware*> vars;
	MetaProvider prov(m, 1, m.indexOfCol(0, "controller"), &vars);
	Json meta = prov.collect();
	TH_CHECK(meta.toString() == R"({"note":"hello"})");
}

TH_TEST(metaprovider_freezes_literal_columns_after_first_collect) {
	/* P8/M2：字面列首采即冻结，之后每次只是重放缓存（避免逐帧重扫矩阵） */
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname note;\nr0 hello;\n@\n");
	std::map<std::string, MatrixVars_Hardware*> vars;
	MetaProvider prov(m, 1, -1, &vars);
	TH_CHECK(prov.collect().toString() == R"({"note":"hello"})");
	TH_CHECK(prov.collect().toString() == R"({"note":"hello"})");
}

TH_TEST(metaprovider_merges_live_snapshot_of_reference_column) {
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname src;\nr0 $t0;\n@\n");
	int reads = 0;
	MatrixVars_Hardware t0([&reads](const Json&) {
		++reads;
		Json snap = JsonObject();
		snap.put("count", JsonInt(reads));
		return snap;
	});
	std::map<std::string, MatrixVars_Hardware*> vars;
	vars["t0"] = &t0;
	MetaProvider prov(m, 1, -1, &vars);

	Json first = prov.collect();
	Json second = prov.collect();
	/* 引用列不入冻结缓存：每次 collect 都取实时快照 */
	TH_CHECK(reads == 2);
	TH_CHECK(first.get("count")->getInt() == 1);
	TH_CHECK(second.get("count")->getInt() == 2);
}

TH_TEST(metaprovider_unknown_reference_reports_clearly) {
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname src;\nr0 $ghost;\n@\n");
	std::map<std::string, MatrixVars_Hardware*> vars;
	MetaProvider prov(m, 1, -1, &vars);
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is not defined", prov.collect());
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "$ghost", prov.collect());
}

TH_TEST(metaprovider_scalar_reference_is_rejected) {
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname src;\nr0 $alias0;\n@\n");
	MatrixVars_Hardware alias0([](const Json&) { return JsonString("timer-0"); });
	std::map<std::string, MatrixVars_Hardware*> vars;
	vars["alias0"] = &alias0;
	MetaProvider prov(m, 1, -1, &vars);
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "non-object", prov.collect());
}

TH_TEST(metaprovider_circular_reference_hits_depth_guard) {
	/* C1 防线：$ 引用成环（这里经一个回入 collect() 的变量构造自引用）
	 * 必须被线程局部深度计数拦下并说人话，而不是无限互递归直到 SIGSEGV。 */
	mt::MatrixObject m = fx::oneMatrix("@Relay\nname src;\nr0 $loop;\n@\n");
	std::map<std::string, MatrixVars_Hardware*> vars;
	std::function<Json()> reenter;
	MatrixVars_Hardware loop([&reenter](const Json&) { return reenter(); });
	vars["loop"] = &loop;
	MetaProvider prov(m, 1, -1, &vars);
	reenter = [&prov]() { return prov.collect(); };

	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "circular", prov.collect());
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "reference chain exceeds",
		prov.collect());
	/* 深度计数是 thread_local + RAII 递减：抛异常后必须已归零，
	 * 否则后续任何一次 collect() 都会被误判成环。 */
	mt::MatrixObject clean = fx::oneMatrix("@Relay\nname note;\nr0 hello;\n@\n");
	MetaProvider other(clean, 1, -1, &vars);
	TH_CHECK(other.collect().toString() == R"({"note":"hello"})");
}

TH_TEST(alias_matrix_requires_name_and_alias_columns) {
	mt::MatrixObject noName = fx::oneMatrix("@Alias\nalias;\nx;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "no 'name' column",
		bot::MtVars_Alias::Factory(noName, 1));

	mt::MatrixObject noAlias = fx::oneMatrix("@Alias\nname;\nx;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "no 'alias' column",
		bot::MtVars_Alias::Factory(noAlias, 1));
}

TH_TEST(alias_matrix_reads_its_target_column) {
	mt::MatrixObject m = fx::oneMatrix("@Alias\nname alias;\nspeed duty;\n@\n");
	std::unique_ptr<bot::MtVars_Alias> alias = bot::MtVars_Alias::Factory(m, 1);
	/* 别名变量 read() 返回目标列名字符串（供 @Action 列名二次解析） */
	TH_CHECK(alias->read().getValue() == "duty");
	/* "-" 继承：第二行未写 alias 时沿用上一行 */
	mt::MatrixObject m2 = fx::oneMatrix("@Alias\nname alias;\nspeed duty;\nfast -;\n@\n");
	std::unique_ptr<bot::MtVars_Alias> alias2 = bot::MtVars_Alias::Factory(m2, 2);
	TH_CHECK(alias2->read().getValue() == "duty");
}
