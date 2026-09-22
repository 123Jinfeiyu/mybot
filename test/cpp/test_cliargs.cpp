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
	/* 帮助里的选项定义行（两空格 + "--"） */
	const std::set<std::string> defs = optionsWithLinePrefix(bot::helpText(), "  --");
	TH_CHECK(defs.count("--tcp") == 1);
	TH_CHECK(defs.count("--prompt-file") == 1);
	TH_CHECK(defs.count("--agent-shell") == 1);
	/* 已移除的选项不得在帮助里复活（2026-09-20 评估：--llm-key 明文 argv；
	 * --agent-shell 顶掉 --agent-no-shell，故净减 1） */
	TH_CHECK(defs.count("--llm-key") == 0);
	TH_CHECK(defs.count("--agent-no-shell") == 0);
	/* 分段帮助（2026-09-22）把 --help <topic> 四行并入帮助正文（23），审计轨迹
	 * （--audit-log，2026-09-22）再加一项（24），策略钩子（--agent-policy，
	 * 2026-09-22）再加一项（25）。计数断言是「帮助 ↔ docs 双向一致」守卫的
	 * 一部分：随选项增删同步更新。 */
	TH_CHECK(defs.size() == 25);
	TH_CHECK(defs.count("--help") == 1);
	TH_CHECK(defs.count("--audit-log") == 1);
	TH_CHECK(defs.count("--agent-policy") == 1);

	/* docs 侧：reference.md（CLI 参考）与 agent.md（agent 专属选项表）的选项表行 */
	const std::set<std::string> ref =
		optionsWithLinePrefix(readSourceFile("docs/reference.md"), "| `");
	std::set<std::string> docs = ref;
	for (const auto& o : optionsWithLinePrefix(readSourceFile("docs/agent.md"), "| `")) {
		docs.insert(o);
	}
	TH_CHECK(docs.size() >= 24);

	/* 方向一（逐文档，2026-09-20 评估收口）：帮助里的**每个**选项都必须在
	 * reference.md 有记录。此前只查「并集」，于是 reference.md 缺 --prompt-file
	 * 被 agent.md 的同名行掩盖——而 reference.md 自称收录全部可调接口。
	 * agent.md 是 agent 专属子集，不要求它覆盖全部。 */
	const std::string undocumented = firstMissing(defs, ref, {});
	TH_CHECK_MSG(undocumented.empty(),
		"帮助里的选项在 docs/reference.md 无记录（该文档自称收录全部可调接口）: " + undocumented);

	/* 方向二：docs 记录的选项都必须在帮助里有定义行——例外仅 --help（帮助自身的
	 * 开关，由惯例与错误提示覆盖）与 --robot（帮助首段与示例里已给出用法）。 */
	const std::string undef = firstMissing(docs, defs, {"--help", "--robot"});
	TH_CHECK_MSG(undef.empty(), "docs 记录了但帮助缺定义行的选项: " + undef);
}

TH_TEST(cliargs_removed_options_are_unknown_keys) {
	/* --llm-key / --agent-no-shell 已从选项表移除（2026-09-20 评估）：
	 * 残留用法必须显式报错（退出码 2），不得静默忽略成「没传」。 */
	Argv a({"mybot", "--robot=robots/test.sql", "--agent", "--llm-key=sk-x"});
	bot::ParsedArgs p = bot::parseArgs(a.argc(), a.argv());
	TH_CHECK(p.unknown.size() == 1 && p.unknown[0] == "llm-key");
	TH_CHECK(bot::validateOptions(p) == 2);

	Argv b({"mybot", "--robot=robots/test.sql", "--agent", "--agent-no-shell"});
	bot::ParsedArgs q = bot::parseArgs(b.argc(), b.argv());
	TH_CHECK(q.unknown.size() == 1 && q.unknown[0] == "agent-no-shell");
	TH_CHECK(bot::validateOptions(q) == 2);
}

/* 环境变量完整性（2026-09-20 评估）：reference.md 的环境变量表自称「全部」，
 * 但此前漏了 BOT4CJ_HAL_BACKEND / BOT4CJ_CAMERA_SRC_DIR / BOT4CJ_TEST_VIDEO。
 * 这里把该声明变成机械判据：源码/脚本里出现的 BOT4CJ_* 环境变量名必须在
 * reference.md 有记录。内嵌 Lua 重入口守卫用的是 `__BOT4CJ_*` 前缀的全局
 * （不是环境变量），扫描时按「前面不是下划线」排除掉。 */
TH_TEST(cliargs_env_vars_documented_in_reference) {
	static const char* kFiles[] = {
		"src/agentmode.cpp", "src/agenttools.cpp", "src/actionsource.cpp",
		"src/luasandbox.cpp", "src/main.cpp", "src/llmconfig.cpp",
		"modules/lib/HAL.lua", "modules/lib/virtbus.lua", "modules/Camera.lua",
		"test/lib/device_gate.sh", "scripts/agent_e2e.sh",
	};
	const std::string doc = readSourceFile("docs/reference.md");
	std::set<std::string> found;
	for (const char* rel : kFiles) {
		std::string text;
		try { text = readSourceFile(rel); } catch (const std::exception&) { continue; }
		for (std::size_t i = 0; i + 7 < text.size(); ++i) {
			if (text.compare(i, 7, "BOT4CJ_") != 0) { continue; }
			if (i > 0 && text[i - 1] == '_') { continue; }	/* __BOT4CJ_* = 守卫全局 */
			std::size_t e = i + 7;
			while (e < text.size()
					&& (std::isupper(static_cast<unsigned char>(text[e]))
						|| std::isdigit(static_cast<unsigned char>(text[e]))
						|| text[e] == '_')) {
				++e;
			}
			if (e > i + 7) { found.insert(text.substr(i, e - i)); }
		}
	}
	/* 已知被扫描到的集合至少含这 5 个（防扫描本身失效变成空集假绿） */
	TH_CHECK(found.size() >= 5);
	std::string missing;
	for (const auto& v : found) {
		if (doc.find(v) == std::string::npos) { missing += " " + v; }
	}
	TH_CHECK_MSG(missing.empty(),
		"docs/reference.md 环境变量表缺失（该文档自称收录全部）:" + missing);
}
