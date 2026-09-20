/* test_cliargs.cpp —— CLI 解析 / 校验 / 帮助文本的直接锚点（本轮 M0-B1）
 *
 * 背景：parseArgs/validateOptions/print_help 原先定义在 main.cpp 里，而 main.cpp
 * 不参与 mybot_tests 链接，test/ 对这三个名字的引用为 0；~6 个校验分支只有
 * 2 个被黑盒覆盖，`--help` 更是 grep 零命中。搬进 src/cliargs.* 后在此白盒钉住：
 *   1) 未知键 / 带值键缺值 → 退出码 2（F-20 / N-4）
 *   2) agent 选项组合三条拒绝路径 → 退出码 2，合法动作模式 → 0
 *   3) 裸 --tcp 不回落端口（审计中-3）
 *   4) --help 优先级（main 在 validateOptions 之前返回 0）
 *   5) 帮助正文：关键区块存在 + 选项定义集合与 docs/ 的选项表双向一致
 *      （文档漂移从「靠人眼」变成「红」——B1 的黄金测试）
 *
 * 注意：负路径用例会让 validateOptions 按契约向 stderr 打印人话报错，
 * 单测输出里出现「未知参数：」「参数错误：」等行属**预期**，不是失败。
 */

#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "cliargs.hpp"
#include "harness.hpp"

#ifndef MYBOT_SOURCE_DIR
#define MYBOT_SOURCE_DIR "."
#endif

namespace {

/* argv 构造：parseArgs 从下标 1 开始（0 是程序名），与真实调用一致 */
struct Argv {
	std::vector<std::string> store;
	std::vector<char*> ptrs;
	explicit Argv(std::vector<std::string> a) : store(std::move(a)) {
		for (auto& s : store) { ptrs.push_back(s.data()); }
	}
	int argc() const { return static_cast<int>(ptrs.size()); }
	char** argv() { return ptrs.data(); }
};

std::string readSourceFile(const std::string& rel) {
	std::ifstream f(std::string(MYBOT_SOURCE_DIR) + "/" + rel);
	if (!f) {
		throw std::runtime_error(std::string("无法读取 ") + rel
			+ "（MYBOT_SOURCE_DIR=" MYBOT_SOURCE_DIR "）");
	}
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 从文本中抽取「以 prefix 开头的一行」里的选项名（prefix 之后取 [a-z0-9-] 连续段）。
 * prefix 自带 "--" 时（帮助正文的 "  --" 定义行）结果补回前缀；prefix 不带时
 * （docs 的 "| `" 表格行）只收以 "--" 开头的名字——环境变量行/配置字段行因此被滤掉。 */
std::set<std::string> optionsWithLinePrefix(const std::string& text, const std::string& prefix) {
	const bool prefixHasDashes =
		prefix.size() >= 2 && prefix.compare(prefix.size() - 2, 2, "--") == 0;
	std::set<std::string> out;
	std::size_t pos = 0;
	while (pos < text.size()) {
		std::size_t eol = text.find('\n', pos);
		if (eol == std::string::npos) { eol = text.size(); }
		if (text.compare(pos, prefix.size(), prefix) == 0) {
			std::size_t b = pos + prefix.size();
			std::size_t e = b;
			while (e < eol && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '-')) { ++e; }
			if (e > b) {
				std::string name = text.substr(b, e - b);
				if (prefixHasDashes) { name = "--" + name; }
				if (name.size() > 2 && name.compare(0, 2, "--") == 0) { out.insert(name); }
			}
		}
		pos = eol + 1;
	}
	return out;
}

/* 双向集合差的第一个违例者（空串 = 无违例），便于失败信息点名具体选项 */
std::string firstMissing(const std::set<std::string>& from,
		const std::set<std::string>& have, const std::set<std::string>& allowlist) {
	for (const auto& x : from) {
		if (have.count(x) == 0 && allowlist.count(x) == 0) { return x; }
	}
	return std::string();
}

} // namespace

/* ---- 1) 未知键 / 缺值：退出码 2（F-20 / N-4）---- */

TH_TEST(cliargs_unknown_key_is_rejected_with_exit_2) {
	Argv a({"mybot", "--robot=robots/test.sql", "--bogus=1", "--robotx"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.unknown.size() == 2);
	TH_CHECK(p.unknown[0] == "bogus");
	TH_CHECK(p.unknown[1] == "robotx");
	TH_CHECK(p.opts.robot.has_value());
	TH_CHECK(bot::validateOptions(p) == 2);
}

TH_TEST(cliargs_missing_value_is_rejected_with_exit_2) {
	/* `--robot robots/test.sql` 正常取值；末尾裸写 --tcp-host 判缺值 */
	Argv a({"mybot", "--robot", "robots/test.sql", "--tcp-host"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.missingValue.size() == 1);
	TH_CHECK(p.missingValue[0] == "tcp-host");
	TH_CHECK(p.opts.robot.has_value() && *p.opts.robot == "robots/test.sql");
	TH_CHECK(bot::validateOptions(p) == 2);
}

/* ---- 2) agent 选项组合三条拒绝路径 + 合法动作模式放行 ---- */

TH_TEST(cliargs_agent_only_option_without_agent_is_rejected) {
	Argv a({"mybot", "--robot=robots/test.sql", "--task", "hi"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(!p.opts.agent);
	TH_CHECK(p.opts.agentOptionsUsed);	/* agentOnly 先于缺值检查置位 */
	TH_CHECK(p.opts.agentTask == "hi");
	TH_CHECK(bot::validateOptions(p) == 2);
}

TH_TEST(cliargs_agent_with_tcp_is_rejected) {
	Argv a({"mybot", "--agent", "--robot=robots/test.sql", "--tcp=19301"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.opts.agent && p.opts.tcp);
	TH_CHECK(bot::validateOptions(p) == 2);
}

TH_TEST(cliargs_agent_without_robot_is_rejected) {
	Argv a({"mybot", "--agent", "--llm-var", "llm0"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.opts.agentOptionsUsed && !p.opts.robot.has_value());
	TH_CHECK(bot::validateOptions(p) == 2);
}

TH_TEST(cliargs_valid_action_mode_args_pass) {
	Argv a({"mybot", "--robot=robots/test.sql"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.unknown.empty() && p.missingValue.empty());
	TH_CHECK(bot::validateOptions(p) == 0);
}

/* ---- 3) 裸 --tcp 不回落端口（审计中-3）---- */

TH_TEST(cliargs_bare_tcp_flag_keeps_port_unset) {
	Argv a({"mybot", "--tcp", "--robot=robots/test.sql"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.opts.tcp);
	TH_CHECK(p.opts.tcpPort.empty());	/* 由 openActionSource 报「缺端口号」 */
	TH_CHECK(bot::validateOptions(p) == 0);
}

TH_TEST(cliargs_tcp_with_equals_and_separate_value_forms) {
	Argv eq({"mybot", "--tcp=19301", "--robot=robots/test.sql"});
	bot::ParsedArgs p1 = bot::parseArgs(eq.argc(), eq.argv());
	TH_CHECK(p1.opts.tcp && p1.opts.tcpPort == "19301");

	Argv sep({"mybot", "--tcp", "19302", "--robot=robots/test.sql"});
	bot::ParsedArgs p2 = bot::parseArgs(sep.argc(), sep.argv());
	TH_CHECK(p2.opts.tcp && p2.opts.tcpPort == "19302");
}

/* ---- 4) --help 优先级：main 在 validateOptions 之前返回 0 ---- */

TH_TEST(cliargs_help_precedes_validation_in_main) {
	/* 同时给 --help 与非法键：main 先看 help（返回 0），未知键不会被报出；
	 * 本用例把这条顺序固化为契约——若哪天把 help 检查挪到校验之后，这里要改。 */
	Argv a({"mybot", "--help", "--bogus=1"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.opts.help);
	TH_CHECK(!p.unknown.empty());		/* 解析层照样记录，只是 main 不会走到 */
}

/* ---- 5) 帮助正文：区块 + 与 docs/ 选项表双向一致（黄金测试）---- */

TH_TEST(cliargs_help_text_covers_key_sections) {
	const std::string h = bot::helpText();
	TH_CHECK(h.find("动作输入源（优先级从高到低）：") != std::string::npos);
	TH_CHECK(h.find("agent 模式（与上面的动作输入源互斥）") != std::string::npos);
	TH_CHECK(h.find("取值优先级：命令行 > 环境变量 > 配置文件 > 驱动内置缺省。") != std::string::npos);
	TH_CHECK(h.find("示例：") != std::string::npos);
	TH_CHECK(h.find("安全约束：动作脚本只能引入新的参数键") != std::string::npos);
	TH_CHECK(h.find("结构脚本与动作脚本单次输入上限均为 8 MiB。") != std::string::npos);
}

TH_TEST(cliargs_help_option_set_matches_docs) {
	/* 帮助里的选项定义行（两空格 + "--"），共 23 条 */
	const std::set<std::string> defs = optionsWithLinePrefix(bot::helpText(), "  --");
	TH_CHECK(defs.size() == 23);
	TH_CHECK(defs.count("--tcp") == 1);
	TH_CHECK(defs.count("--prompt-file") == 1);

	/* docs 侧：reference.md（CLI 参考）与 agent.md（agent 专属选项表）的选项表行 */
	std::set<std::string> docs = optionsWithLinePrefix(readSourceFile("docs/reference.md"), "| `");
	for (const auto& o : optionsWithLinePrefix(readSourceFile("docs/agent.md"), "| `")) {
		docs.insert(o);
	}
	TH_CHECK(docs.size() >= 24);

	/* 方向一：帮助里的每个选项都必须在 docs 有记录（加选项忘改文档即红） */
	const std::string undocumented = firstMissing(defs, docs, {});
	TH_CHECK_MSG(undocumented.empty(), "帮助里出现 docs 未记录的选项: " + undocumented);

	/* 方向二：docs 记录的选项都必须在帮助里有定义行——例外仅 --help（帮助自身的
	 * 开关，由惯例与错误提示覆盖）与 --robot（帮助首段与示例里已给出用法）。 */
	const std::string undef = firstMissing(docs, defs, {"--help", "--robot"});
	TH_CHECK_MSG(undef.empty(), "docs 记录了但帮助缺定义行的选项: " + undef);
}
