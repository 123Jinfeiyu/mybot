/* test_mtinclude.cpp —— @Include 展开白盒单测
 *
 * 钉住 mtinclude::expandIncludes 的全部语义分支（mtmerge_core 归并内核本身
 * 由 test_mtmerge 覆盖，这里只测展开层的编排与校验）：
 *   - fast path：无 @Include 逐字不动；
 *   - @Action 相位归并（含首个 include 顶替缺省 @Action、phase "-" 继承）；
 *   - @Alias/@Vars 行叠加与同名报错；
 *   - 负路径：设备相交 / 缺 @Action / 嵌套 @Include / 路径词法 / 文件缺失 /
 *     软链拒绝 / 行数与总字节预算；
 *   - 未知矩阵透传。
 * 文件类用例走 fx::TempDir（进程级隔离，析构即清）。
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "actionsource.hpp"
#include "fixtures.hpp"
#include "harness.hpp"
#include "matrix.hpp"
#include "mtinclude.hpp"
#include "mtmerge_core.hpp"
#include "util.hpp"

namespace {

/* 在 TempDir 写一个文本文件（父目录自动创建） */
void writeFile(const fx::TempDir& dir, const std::string& name, const std::string& text) {
	std::ofstream f(dir.path() / name, std::ios::binary);
	if (!f) { throw std::runtime_error("前置条件失败：写夹具文件 " + name); }
	f << text;
}

/* 主脚本常见形态：@Action（可选）+ @Include 两行 */
std::vector<mt::MatrixObject> parseMain(const std::string& text) {
	return fx::manyMatrices(text);
}

} // namespace

/* ---------------- fast path ---------------- */

TH_TEST(mtinclude_no_include_is_noop) {
	std::string text = "@Alias\nname value;\nk 1;\n@\n\n@Action\na.x;\n1;\n@\n";
	auto mats = parseMain(text);
	std::vector<std::string> namesBefore;
	for (const auto& m : mats) { namesBefore.push_back(bot::trim(m.getName())); }
	mtinclude::expandIncludes(mats, ".");
	std::vector<std::string> namesAfter;
	for (const auto& m : mats) { namesAfter.push_back(bot::trim(m.getName())); }
	TH_CHECK(namesBefore == namesAfter);
	TH_CHECK(fx::cell(mats[0], 1, 0) == "k");
}

/* ---------------- @Action 相位归并 ---------------- */

TH_TEST(mtinclude_merges_action_with_phase) {
	fx::TempDir dir("inc_action");
	writeFile(dir, "b.mt", "@Action\nb.p;\n100;\n200;\n@\n");
	auto mats = parseMain(
		"@Action\na.x;\n1;\n2;\n@\n"
		"@Include\npath phase;\n  b.mt 1;\n@\n");
	mtinclude::expandIncludes(mats, dir.path().string());
	TH_CHECK(mats.size() == 1);	/* 展开产物只剩归并后的 @Action */
	const mt::MatrixObject& act = mats[0];
	TH_CHECK(bot::trim(act.getName()) == "Action");
	TH_CHECK(act.cols() == 2);			/* a.x ∪ b.p */
	TH_CHECK(act.rows() == 4);			/* 表头 + max(2, 2+1)=3 数据行 */
	TH_CHECK(fx::cell(act, 1, 1) == "-");	/* b 滞后 1：首行未起拍 */
	TH_CHECK(fx::cell(act, 2, 1) == "100");
	TH_CHECK(fx::cell(act, 3, 1) == "200");
	TH_CHECK(fx::cell(act, 3, 0) == "-");	/* a 耗尽补 "-" */
}

TH_TEST(mtinclude_phase_dash_inherits_previous_row) {
	fx::TempDir dir("inc_dash");
	writeFile(dir, "b.mt", "@Action\nb.p;\n100;\n@\n");
	writeFile(dir, "c.mt", "@Action\nc.q;\n7;\n@\n");
	/* 第二行 phase "-" 继承首行 2（矩阵 "-" 前向填充语义延伸到相位列） */
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath phase;\n  b.mt 2;\n  c.mt -;\n@\n");
	mtinclude::expandIncludes(mats, dir.path().string());
	const mt::MatrixObject& act = mats[0];
	TH_CHECK(act.rows() == 4);			/* max(1, 1+2)=3 数据行 */
	TH_CHECK(fx::cell(act, 3, 2) == "7");	/* c 滞后 2 → 第 3 行才出现 */
	TH_CHECK(fx::cell(act, 1, 2) == "-");
}

TH_TEST(mtinclude_first_include_becomes_action_when_main_has_none) {
	fx::TempDir dir("inc_noacc");
	writeFile(dir, "b.mt", "@Action\nb.p;\n100;\n@\n");
	writeFile(dir, "c.mt", "@Action\nc.q;\n7;\n@\n");
	/* 主脚本没有 @Action：首个 include 顶替累加器（其相位无对齐目标，
	 * 与 mtmerge --phase 作用于第 2 个及后续输入同口径） */
	auto mats = parseMain(
		"@Include\npath phase;\n  b.mt 5;\n  c.mt 0;\n@\n");
	mtinclude::expandIncludes(mats, dir.path().string());
	const mt::MatrixObject& act = mats[0];
	TH_CHECK(act.cols() == 2);
	TH_CHECK(fx::cell(act, 1, 0) == "100");	/* b 原样（相位 5 未生效） */
	TH_CHECK(fx::cell(act, 1, 1) == "7");
}

/* ---------------- @Alias / @Vars 叠加 ---------------- */

TH_TEST(mtinclude_stacks_alias_and_vars) {
	fx::TempDir dir("inc_scalar");
	writeFile(dir, "b.mt",
		"@Alias\nname value;\n  kb delay;\n@\n"
		"@Vars\nname value;\n  kv 2;\n@\n"
		"@Action\nb.p;\n100;\n@\n");
	auto mats = parseMain(
		"@Alias\nname value;\n  ka x;\n@\n"
		"@Vars\nname value;\n  kv0 1;\n@\n"
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath;\n  b.mt;\n@\n");
	mtinclude::expandIncludes(mats, dir.path().string());
	TH_CHECK(mats.size() == 3);	/* Alias + Vars + Action（include 的并入本体） */
	TH_CHECK(bot::trim(mats[0].getName()) == "Alias");
	TH_CHECK(fx::cell(mats[0], 1, 0) == "ka");
	TH_CHECK(fx::cell(mats[0], 2, 0) == "kb");
	TH_CHECK(bot::trim(mats[1].getName()) == "Vars");
	TH_CHECK(fx::cell(mats[1], 1, 0) == "kv0");
	TH_CHECK(fx::cell(mats[1], 2, 0) == "kv");
	TH_CHECK(fx::cell(mats[2], 1, 1) == "100");
}

TH_TEST(mtinclude_rejects_duplicate_alias_across_include) {
	fx::TempDir dir("inc_dup");
	writeFile(dir, "b.mt", "@Alias\nname value;\n  ka other;\n@\n@Action\nb.p;\n100;\n@\n");
	auto mats = parseMain(
		"@Alias\nname value;\n  ka x;\n@\n"
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath;\n  b.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "duplicate Alias name 'ka'",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

/* ---------------- 负路径：归并契约 ---------------- */

TH_TEST(mtinclude_rejects_device_overlap) {
	fx::TempDir dir("inc_overlap");
	writeFile(dir, "b.mt", "@Action\na.p;\n100;\n@\n");	/* 与主脚本同为 a */
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath;\n  b.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "device 'a' appears in both scripts",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

TH_TEST(mtinclude_rejects_missing_action_in_included_file) {
	fx::TempDir dir("inc_noact");
	writeFile(dir, "b.mt", "@Alias\nname value;\n  kb delay;\n@\n");
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath;\n  b.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "no @Action matrix found",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

TH_TEST(mtinclude_rejects_nested_include) {
	fx::TempDir dir("inc_nested");
	writeFile(dir, "inner.mt", "@Include\npath;\n  b.mt;\n@\n");
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath;\n  inner.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "nested @Include",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

/* ---------------- 负路径：路径词法与文件系统 ---------------- */

TH_TEST(mtinclude_path_lexical_rules) {
	fx::TempDir dir("inc_lex");
	auto main_with = [](const std::string& p) {
		return parseMain("@Action\na.x;\n1;\n@\n@Include\npath;\n  " + p + ";\n@\n");
	};
	auto abs_path = main_with("/etc/x.mt");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "absolute path rejected",
		mtinclude::expandIncludes(abs_path, dir.path().string()));
	auto traversal = main_with("../x.mt");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "must not traverse outside",
		mtinclude::expandIncludes(traversal, dir.path().string()));
	auto dot_seg = main_with("./x.mt");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "empty or '.' segment",
		mtinclude::expandIncludes(dot_seg, dir.path().string()));
	auto not_mt = main_with("x.lua");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "only accepts .mt",
		mtinclude::expandIncludes(not_mt, dir.path().string()));
}

TH_TEST(mtinclude_missing_file_reports_path) {
	fx::TempDir dir("inc_missing");
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n@Include\npath;\n  nope.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "cannot open include file",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

TH_TEST(mtinclude_rejects_symlink) {
	fx::TempDir dir("inc_symlink");
	writeFile(dir, "real.mt", "@Action\nb.p;\n100;\n@\n");
	std::error_code ec;
	std::filesystem::create_symlink("real.mt", dir.path() / "link.mt", ec);
	/* 本项目只支持 Linux，软链创建失败视为环境异常直接判红——不用「跳过
	 * 断言提前返回」的假绿形态（run_all.sh 对该形态本就判失败） */
	TH_CHECK(!ec);
	if (ec) { return; }
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n@Include\npath;\n  link.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "symbolic link",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

/* ---------------- 预算防线 ---------------- */

TH_TEST(mtinclude_row_budget_rejects_before_reading) {
	/* 33 行在读取任何文件之前就被拒（文件名故意不存在：证明检查先于 IO） */
	std::string rows;
	for (int i = 0; i < 33; ++i) { rows += "  nope" + std::to_string(i) + ".mt 0;\n"; }
	auto mats = parseMain("@Action\na.x;\n1;\n@\n@Include\npath phase;\n" + rows + "@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "exceeding the limit of 32",
		mtinclude::expandIncludes(mats, "."));
}

TH_TEST(mtinclude_total_byte_budget_rejects) {
	fx::TempDir dir("inc_budget");
	/* 两个各 ~4.5MiB 的文件：单文件不超 8MiB，合计超总预算 */
	std::string pad(64, 'x');
	{
		std::ofstream f(dir.path() / "big1.mt", std::ios::binary);
		f << "@Action\nb.p;\n";
		for (int i = 0; i < 68000; ++i) { f << "    " << pad << ";\n"; }	/* ~4.6MiB */
		f << "@\n";
	}
	{
		std::ofstream f(dir.path() / "big2.mt", std::ios::binary);
		f << "@Action\nc.q;\n";
		for (int i = 0; i < 68000; ++i) { f << "    " << pad << ";\n"; }
		f << "@\n";
	}
	auto mats = parseMain(
		"@Action\na.x;\n1;\n@\n@Include\npath;\n  big1.mt;\n  big2.mt;\n@\n");
	TH_CHECK_THROWS_MSG(bot::ScriptTooLarge, "total size of included action scripts",
		mtinclude::expandIncludes(mats, dir.path().string()));
}

/* ---------------- 列头校验与透传 ---------------- */

TH_TEST(mtinclude_include_header_rules) {
	fx::TempDir dir("inc_header");
	auto bad_col = parseMain(
		"@Action\na.x;\n1;\n@\n@Include\npath file;\n  b.mt x;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is not supported",
		mtinclude::expandIncludes(bad_col, dir.path().string()));
	auto bad_phase = parseMain(
		"@Action\na.x;\n1;\n@\n@Include\npath phase;\n  b.mt soon;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "phase is not an integer",
		mtinclude::expandIncludes(bad_phase, dir.path().string()));
}

TH_TEST(mtinclude_unknown_matrices_pass_through) {
	fx::TempDir dir("inc_unknown");
	writeFile(dir, "b.mt", "@Action\nb.p;\n100;\n@\n");
	auto mats = parseMain(
		"@Foo\nhead;\n  v;\n@\n"
		"@Action\na.x;\n1;\n@\n"
		"@Include\npath;\n  b.mt;\n@\n");
	mtinclude::expandIncludes(mats, dir.path().string());
	/* 产物：Alias/Vars（无）→ Action → 未知 @Foo */
	bool foundFoo = false;
	for (const auto& m : mats) {
		if (bot::trim(m.getName()) == "Foo") { foundFoo = true; }
	}
	TH_CHECK(foundFoo);
	TH_CHECK(mats.size() == 2);
}
