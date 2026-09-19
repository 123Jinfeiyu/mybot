#include "fixtures.hpp"
#include "harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "promptpack.hpp"

namespace fs = std::filesystem;

using bot::agent::kMaxPromptFileBytes;
using bot::agent::loadPromptPack;
using fx::TempDir;

namespace {

void writeFile(const fs::path& p, const std::string& content) {
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary);
	f << content;
}

} // namespace

/* 空输入不是错误：提示词包是可选项，未使用时走缺省系统提示词 */
TH_TEST(promptpack_empty_input_is_noop) {
	std::string out, err;
	TH_CHECK(loadPromptPack({}, out, err));
	TH_CHECK(out.empty());
	TH_CHECK(err.empty());
}

/* 单文件保持原文，不加 "## 文件名" 分节标题（robot_agent 同款语义） */
TH_TEST(promptpack_single_file_kept_verbatim) {
	TempDir tmp("pp1");
	fs::path f = tmp.path() / "a.md";
	writeFile(f, "\n###### 身份 ######\n你是蜘蛛机器人。\n");
	std::string out, err;
	TH_CHECK(loadPromptPack({f.string()}, out, err));
	TH_CHECK(out == "###### 身份 ######\n你是蜘蛛机器人。");	/* 原文，无分节标题 */
}

/* 显式文件按传入顺序拼接，每份前加 "## 文件名" 分节 */
TH_TEST(promptpack_explicit_files_keep_order_with_headings) {
	TempDir tmp("pp2");
	fs::path b = tmp.path() / "b.md";
	fs::path a = tmp.path() / "a.md";
	writeFile(b, "内容B\n");
	writeFile(a, "内容A\n");
	std::string out, err;
	TH_CHECK(loadPromptPack({b.string(), a.string()}, out, err));
	TH_CHECK(out == "## b.md\n\n内容B\n\n## a.md\n\n内容A");
}

/* 目录收集全部 .md 且按文件名自然排序：1- 在 2- 在 10- 之前（字典序相反） */
TH_TEST(promptpack_directory_natural_sort) {
	TempDir tmp("pp3");
	fs::path dir = tmp.path() / "pack";
	/* 故意乱序创建：目录枚举序不可靠，排序必须由实现保证 */
	writeFile(dir / "10-尾声.md", "十");
	writeFile(dir / "2-纪律.md", "二");
	writeFile(dir / "1-身份.md", "一");
	std::string out, err;
	TH_CHECK(loadPromptPack({dir.string()}, out, err));
	size_t p1 = out.find("## 1-身份.md");
	size_t p2 = out.find("## 2-纪律.md");
	size_t p10 = out.find("## 10-尾声.md");
	TH_CHECK(p1 != std::string::npos);
	TH_CHECK(p2 != std::string::npos);
	TH_CHECK(p10 != std::string::npos);
	TH_CHECK(p1 < p2 && p2 < p10);
	TH_CHECK(out.find("一") < out.find("二"));
}

/* 目录只收普通 .md：其他扩展名与子目录（递归 .md）不进包。
 * 注意 .md 被滤到只剩一份时命中「单文件不加标题」规则——这正是要钉住的行为。 */
TH_TEST(promptpack_directory_skips_non_md_and_subdirs) {
	TempDir tmp("pp4");
	fs::path dir = tmp.path() / "pack";
	writeFile(dir / "keep.md", "保留");
	writeFile(dir / "note.txt", "忽略");
	writeFile(dir / "sub" / "nested.md", "忽略");
	std::string out, err;
	TH_CHECK(loadPromptPack({dir.string()}, out, err));
	TH_CHECK(out == "保留");
	TH_CHECK(out.find("忽略") == std::string::npos);
}

/* 去首尾空白后为空的文件跳过，不产生空分节（跳过后只剩一份 → 不加标题） */
TH_TEST(promptpack_blank_files_skipped) {
	TempDir tmp("pp5");
	fs::path dir = tmp.path() / "pack";
	writeFile(dir / "a.md", "实内容\n");
	writeFile(dir / "blank.md", " \t\n");
	std::string out, err;
	TH_CHECK(loadPromptPack({dir.string()}, out, err));
	TH_CHECK(out == "实内容");
	TH_CHECK(out.find("## a.md") == std::string::npos);
}

/* 目录按输入位置原位展开，展开结果（自然排序）与其后的显式文件拼接 */
TH_TEST(promptpack_mixed_dir_and_file_input_order) {
	TempDir tmp("pp6");
	fs::path dir = tmp.path() / "pack";
	writeFile(dir / "2-b.md", "B");
	writeFile(dir / "1-a.md", "A");
	fs::path z = tmp.path() / "z.md";
	writeFile(z, "Z");
	std::string out, err;
	TH_CHECK(loadPromptPack({dir.string(), z.string()}, out, err));
	TH_CHECK(out.find("## 1-a.md") < out.find("## 2-b.md"));
	TH_CHECK(out.find("## 2-b.md") < out.find("## z.md"));
}

/* ---- 负路径：坏输入在配置组装期就报人话错误（配置错误不碰真机） ---- */

TH_TEST(promptpack_missing_path_fails) {
	TempDir tmp("pp7");
	std::string out, err;
	TH_CHECK(!loadPromptPack({(tmp.path() / "nope.md").string()}, out, err));
	TH_CHECK(err.find("不存在") != std::string::npos);
}

TH_TEST(promptpack_directory_without_md_fails) {
	TempDir tmp("pp8");
	fs::path dir = tmp.path() / "empty";
	fs::create_directories(dir);
	writeFile(dir / "x.txt", "非 md");
	std::string out, err;
	TH_CHECK(!loadPromptPack({dir.string()}, out, err));
	TH_CHECK(err.find(".md") != std::string::npos);
}

TH_TEST(promptpack_all_blank_content_fails) {
	TempDir tmp("pp9");
	fs::path dir = tmp.path() / "pack";
	writeFile(dir / "a.md", "\n\n");
	std::string out, err;
	TH_CHECK(!loadPromptPack({dir.string()}, out, err));
	TH_CHECK(err.find("内容为空") != std::string::npos);
}

/* 超字节上限响亮拒绝，不静默截断（截断过的提示词悄悄丢约束，更难查） */
TH_TEST(promptpack_oversized_file_rejected) {
	TempDir tmp("pp10");
	fs::path f = tmp.path() / "big.md";
	writeFile(f, std::string(kMaxPromptFileBytes + 1, 'x'));
	std::string out, err;
	TH_CHECK(!loadPromptPack({f.string()}, out, err));
	TH_CHECK(err.find("超过字节上限") != std::string::npos);
}
