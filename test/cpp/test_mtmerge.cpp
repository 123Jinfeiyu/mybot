/* test_mtmerge.cpp —— .mt 动作脚本归并核心白盒单测（F1/N-30）
 *
 * mtmerge 此前是唯一有独立编译目标却零白盒单测的模块：归并算法、相位补 "-"、
 * 设备集相交/列头校验只能靠 shell 黑盒间接覆盖，改一行归并逻辑要等端到端
 * 合成产物被运行端另解才暴露。核心抽到 mtmerge_core 后，这里逐条钉住语义：
 *   - 列 = A 全列（原序）∪ B 全列（原序）；行相位 p>0 滞后、p<0 提前；
 *   - 较短一方耗尽后补字面 "-"（继承链自然延伸）；
 *   - 负相位部分丢弃只告警、全部丢弃抛错（N-8：拒绝静默丢动作）；
 *   - 设备集相交、列头非 <device>.<param>、@Alias/@Vars 列头/同名行冲突均报错。
 */

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "harness.hpp"
#include "matrix.hpp"
#include "mtmerge_core.hpp"
#include "util.hpp"

/* @Action 片段：两列 a.* / b.*，n 个数据行（值 = 行号） */
static mt::MatrixObject actionA(int n, const std::string& dev = "a") {
	std::string t = "@Action\n" + dev + ".x " + dev + ".y;\n";
	for (int r = 1; r <= n; ++r) {
		t += std::to_string(r * 10) + " " + std::to_string(r * 10 + 1) + ";\n";
	}
	t += "@\n";
	return fx::oneMatrix(t);
}

static mt::MatrixObject actionB(int n, const std::string& dev = "b") {
	std::string t = "@Action\n" + dev + ".p;\n";
	for (int r = 1; r <= n; ++r) { t += std::to_string(r * 100) + ";\n"; }
	t += "@\n";
	return fx::oneMatrix(t);
}

/* ---------------- mergeTwo：列拼接与相位 ---------------- */

TH_TEST(mtmerge_merge_two_concatenates_columns_and_pads_tail) {
	/* A 3 行、B 1 行、相位 0：B 耗尽后补 "-"，列序 = A 全列 ∪ B 全列 */
	mt::MatrixObject out = mtmerge::mergeTwo(actionA(3), actionB(1), 0, "a.mt", "b.mt");
	TH_CHECK(out.cols() == 3);
	TH_CHECK(fx::cell(out, 0, 0) == "a.x");
	TH_CHECK(fx::cell(out, 0, 1) == "a.y");
	TH_CHECK(fx::cell(out, 0, 2) == "b.p");
	TH_CHECK(out.rows() == 4);	/* 表头 + 3 数据行 */
	TH_CHECK(fx::cell(out, 1, 2) == "100");
	TH_CHECK(fx::cell(out, 2, 2) == "-");	/* B 已耗尽 */
	TH_CHECK(fx::cell(out, 3, 2) == "-");
	TH_CHECK(fx::cell(out, 3, 0) == "30");
}

TH_TEST(mtmerge_merge_two_positive_phase_lags_second_input) {
	/* 相位 +2：B 整体滞后 2 行，前导补 "-"，总行数 = max(2, 2+2) = 4 */
	mt::MatrixObject out = mtmerge::mergeTwo(actionA(2), actionB(2), 2, "a.mt", "b.mt");
	TH_CHECK(out.rows() == 5);
	TH_CHECK(fx::cell(out, 1, 2) == "-");
	TH_CHECK(fx::cell(out, 2, 2) == "-");
	TH_CHECK(fx::cell(out, 3, 2) == "100");
	TH_CHECK(fx::cell(out, 4, 2) == "200");
	/* A 侧后两行耗尽补 "-" */
	TH_CHECK(fx::cell(out, 3, 0) == "-");
	TH_CHECK(fx::cell(out, 4, 0) == "-");
}

TH_TEST(mtmerge_merge_two_negative_phase_advances_and_warns) {
	/* 相位 -2：B 前 2 行移出输出窗口（部分丢弃 → stderr 告警，产物仍含 B 的其余行）。
	 * next 侧行号 = r - phase = r + 2，故 r=1 取 B 的第 3 行。 */
	std::ostringstream cap;
	std::streambuf* old = std::cerr.rdbuf(cap.rdbuf());
	mt::MatrixObject out = mtmerge::mergeTwo(actionA(3), actionB(3), -2, "a.mt", "b.mt");
	std::cerr.rdbuf(old);

	TH_CHECK(out.rows() == 4);	/* max(3, 3-2) = 3 数据行 */
	TH_CHECK(fx::cell(out, 1, 2) == "300");
	TH_CHECK(fx::cell(out, 2, 2) == "-");
	TH_CHECK(fx::cell(out, 3, 2) == "-");
	const std::string warn = cap.str();
	TH_CHECK(warn.find("前 2 行被丢弃") != std::string::npos);
}

TH_TEST(mtmerge_merge_two_negative_phase_dropping_all_rows_throws) {
	/* N-8：负相位把某个输入**全部**丢弃 → 产物不含该输入任何动作，必须报错退出 2 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "drops all 2 row(s)",
		mtmerge::mergeTwo(actionA(2), actionB(2), -2, "a.mt", "b.mt"));
	/* 相位超出 B 行数同样全丢 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "drops all 2 row(s)",
		mtmerge::mergeTwo(actionA(5), actionB(2), -9, "a.mt", "b.mt"));
}

/* ---------------- checkDisjoint / 列头切分 ---------------- */

TH_TEST(mtmerge_rejects_device_present_in_both_inputs) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "device 'a' appears in both scripts",
		mtmerge::mergeTwo(actionA(1, "a"), actionB(1, "a"), 0, "a.mt", "b.mt"));
}

TH_TEST(mtmerge_rejects_multi_dot_action_column) {
	/* 列头必须恰好一个点；多点在 checkDisjoint 的切分处即报错 */
	mt::MatrixObject bad = fx::oneMatrix("@Action\na.b.c;\n1;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is not valid",
		mtmerge::mergeTwo(bad, actionB(1), 0, "a.mt", "b.mt"));
}

TH_TEST(mtmerge_rejects_column_without_dot) {
	mt::MatrixObject bad = fx::oneMatrix("@Action\nnopoint;\n1;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is not valid",
		mtmerge::mergeTwo(actionA(1), bad, 0, "a.mt", "b.mt"));
}

/* ---------------- mergeScalarRows：@Alias / @Vars 按行叠加 ---------------- */

TH_TEST(mtmerge_scalar_rows_concatenates_in_order) {
	mt::MatrixObject a = fx::oneMatrix("@Alias\nname value;\nka 1;\nkb 2;\n@\n");
	mt::MatrixObject b = fx::oneMatrix("@Alias\nname value;\nkc 3;\n@\n");
	mt::MatrixObject out = mtmerge::mergeScalarRows(a, b, "Alias", "a.mt", "b.mt");
	TH_CHECK(out.getName() == "Alias");
	TH_CHECK(out.rows() == 4);
	TH_CHECK(fx::cell(out, 1, 0) == "ka");
	TH_CHECK(fx::cell(out, 2, 0) == "kb");
	TH_CHECK(fx::cell(out, 3, 0) == "kc");
	TH_CHECK(fx::cell(out, 3, 1) == "3");
}

TH_TEST(mtmerge_scalar_rows_rejects_column_count_mismatch) {
	mt::MatrixObject a = fx::oneMatrix("@Alias\nname value;\nka 1;\n@\n");
	mt::MatrixObject b = fx::oneMatrix("@Alias\nname;\nkc;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "column count differs",
		mtmerge::mergeScalarRows(a, b, "Alias", "a.mt", "b.mt"));
}

TH_TEST(mtmerge_scalar_rows_rejects_header_mismatch) {
	mt::MatrixObject a = fx::oneMatrix("@Alias\nname value;\nka 1;\n@\n");
	mt::MatrixObject b = fx::oneMatrix("@Alias\nname alias;\nkc 3;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "header differs at column 1",
		mtmerge::mergeScalarRows(a, b, "Alias", "a.mt", "b.mt"));
}

TH_TEST(mtmerge_scalar_rows_rejects_missing_name_column) {
	/* 没有 'name' 列就无法判重（同名映射到两个不同值本是歧义） */
	mt::MatrixObject a = fx::oneMatrix("@Vars\nkey value;\nka 1;\n@\n");
	mt::MatrixObject b = fx::oneMatrix("@Vars\nkey value;\nkb 2;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "has no 'name' column",
		mtmerge::mergeScalarRows(a, b, "Vars", "a.mt", "b.mt"));
}

TH_TEST(mtmerge_scalar_rows_rejects_duplicate_name) {
	mt::MatrixObject a = fx::oneMatrix("@Alias\nname value;\nka 1;\n@\n");
	mt::MatrixObject b = fx::oneMatrix("@Alias\nname value;\nka 9;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "duplicate Alias name 'ka'",
		mtmerge::mergeScalarRows(a, b, "Alias", "a.mt", "b.mt"));
}

/* ---------------- foldNamed：同一输入内的多个同名块 ---------------- */

TH_TEST(mtmerge_fold_named_folds_multiple_blocks) {
	/* 一个 .mt 里出现两个 @Alias 块：mybot 会全部注册，mtmerge 不得只看第一个 */
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Alias\nname value;\nka 1;\n@\n"
		"@Alias\nname value;\nkb 2;\n@\n");
	auto folded = mtmerge::foldNamed(mats, "Alias", "x.mt");
	TH_CHECK(folded.has_value());
	TH_CHECK(folded->rows() == 3);
	TH_CHECK(fx::cell(*folded, 1, 0) == "ka");
	TH_CHECK(fx::cell(*folded, 2, 0) == "kb");
}

TH_TEST(mtmerge_fold_named_returns_nullopt_when_absent) {
	std::vector<mt::MatrixObject> mats = fx::manyMatrices("@Action\na.x;\n1;\n@\n");
	TH_CHECK(!mtmerge::foldNamed(mats, "Alias", "x.mt").has_value());
	TH_CHECK(!mtmerge::foldNamed(mats, "Vars", "x.mt").has_value());
}

TH_TEST(mtmerge_fold_named_rejects_duplicate_across_blocks) {
	std::vector<mt::MatrixObject> mats = fx::manyMatrices(
		"@Vars\nname value;\nka 1;\n@\n"
		"@Vars\nname value;\nka 2;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "duplicate Vars name 'ka'",
		mtmerge::foldNamed(mats, "Vars", "x.mt"));
}

/* ---------------- requireAction ---------------- */

TH_TEST(mtmerge_require_action_rejects_missing_action) {
	std::vector<mt::MatrixObject> mats = fx::manyMatrices("@Alias\nname value;\nka 1;\n@\n");
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "no @Action matrix found",
		mtmerge::requireAction(mats, "x.mt"));
}

TH_TEST(mtmerge_require_action_accepts_trimmed_name) {
	/* 矩阵名解析后可能带空白，findByName 按 trim 后比较（与运行端同一单点）。
	 * 按值取结果：GCC 的 -Wdangling-reference 启发式会对「返回引用的函数」误报，
	 * 这里本就不需要引用语义。 */
	std::vector<mt::MatrixObject> mats = fx::manyMatrices("@Action\na.x;\n1;\n@\n");
	mt::MatrixObject act = mtmerge::requireAction(mats, "x.mt");
	TH_CHECK(bot::trim(act.getName()) == "Action");
}
