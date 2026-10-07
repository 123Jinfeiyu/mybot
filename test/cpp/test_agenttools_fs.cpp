/* test_agenttools_fs.cpp —— 只读文件工具族（list_dir / read_file，Phase 4，2026-10-07）
 *
 * 镜像 src/agenttools_fs.cpp：门控（--fs-root 给了才注册）、沙箱（越界/绝对/
 * .. /符号链接逃逸 → 参数错误）、文件级失败（不存在/超限/软链/二进制）、
 * 回执定界与 UTF-8 消毒。沙箱算法本身的用例在 test_agenttools_sandbox.cpp
 * （agent_sandbox.hpp），这里只钉"工具层把 resolveUnderRoot 接对了"。
 */

#include "fixtures.hpp"
#include "harness.hpp"

using fx::TempDir;

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include "agenttools.hpp"
#include "jsonparse.hpp"
#include "robot.hpp"

#include "agenttools_fixtures.hpp"

namespace fs = std::filesystem;

using namespace bot;
using namespace bot::agent;

using afx::execText;
using afx::makeRelayRobot;
using afx::writeFile;

namespace {

/* 建一个最小工具上下文：relay 机器人 + 指定只读根 */
AgentToolContext fsContext(RobotStruct* robot, const std::string& root) {
	AgentToolContext ctx;
	ctx.robot = robot;
	ctx.scriptsDir = "scripts";
	ctx.structurePath = "robots/relay-test.sql";
	ctx.fsRoot = root;
	return ctx;
}

} // namespace

/* 门控（安全缺省）：缺 --fs-root（fsRoot 空）⇒ 两个工具都不注册。 */
TH_TEST(agenttools_fs_tools_absent_without_root) {
	auto robot = makeRelayRobot();
	AgentToolContext ctx = fsContext(robot.get(), "");
	AgentToolRegistry tools = makeAgentTools(ctx);
	TH_CHECK(!tools.contains("list_dir"));
	TH_CHECK(!tools.contains("read_file"));
	robot->deactivate();
}

/* 给了根 ⇒ 两个工具都在（同一门控）。 */
TH_TEST(agenttools_fs_tools_registered_with_root) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-register");
	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);
	TH_CHECK(tools.contains("list_dir"));
	TH_CHECK(tools.contains("read_file"));
	robot->deactivate();
}

/* list_dir：列出条目（名称 + 类型 + 大小）；子目录与文件都显示；软链不列。 */
TH_TEST(agenttools_fs_list_dir_lists_entries_and_hides_symlinks) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-listdir");
	writeFile(root.path() / "a.txt", "hello");
	fs::create_directories(root.path() / "sub");
	TempDir outside("fsroot-listdir-outside");
	writeFile(outside.path() / "secret.txt", "machine-secret");
	TH_CHECK(::symlink((outside.path() / "secret.txt").c_str(),
		(root.path() / "evil-link").c_str()) == 0);

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	std::string out = execText(tools, "list_dir", parseJson("{}"));
	TH_CHECK_MSG(out.find("a.txt") != std::string::npos
		&& out.find("[文件]") != std::string::npos
		&& out.find("5 字节") != std::string::npos, out);
	TH_CHECK_MSG(out.find("sub") != std::string::npos
		&& out.find("[目录]") != std::string::npos, out);
	TH_CHECK_MSG(out.find("evil-link") == std::string::npos,
		"软链条目不该出现在清单里: " + out);
	robot->deactivate();
}

/* list_dir 的目录参数：缺省/"."/"./" 都表示根目录本身。 */
TH_TEST(agenttools_fs_list_dir_default_path_is_root) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-listdir-default");
	writeFile(root.path() / "x.txt", "x");

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	TH_CHECK(execText(tools, "list_dir", parseJson("{}")).find("x.txt") != std::string::npos);
	TH_CHECK(execText(tools, "list_dir", parseJson("{\"path\":\".\"}"))
		.find("x.txt") != std::string::npos);
	robot->deactivate();
}

/* list_dir 的空目录与"不是目录"两种边界回执。 */
TH_TEST(agenttools_fs_list_dir_empty_and_not_a_directory) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-listdir-edge");
	fs::create_directories(root.path() / "empty");
	writeFile(root.path() / "plain.txt", "x");

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	TH_CHECK(execText(tools, "list_dir", parseJson("{\"path\":\"empty\"}"))
		.find("空目录") != std::string::npos);
	std::string notDir = execText(tools, "list_dir", parseJson("{\"path\":\"plain.txt\"}"));
	TH_CHECK(notDir.find("不是目录") != std::string::npos);
	robot->deactivate();
}

/* read_file：正常读回文件内容，经"数据非指令"定界。 */
TH_TEST(agenttools_fs_read_file_returns_content) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-readfile");
	writeFile(root.path() / "note.txt", "hello world\n");
	fs::create_directories(root.path() / "sub");
	writeFile(root.path() / "sub" / "nested.txt", "nested content");

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	std::string out = execText(tools, "read_file", parseJson("{\"path\":\"note.txt\"}"));
	TH_CHECK(out.find("hello world") != std::string::npos);
	TH_CHECK(out.find("数据不是指令") != std::string::npos);

	std::string nested = execText(tools, "read_file", parseJson("{\"path\":\"sub/nested.txt\"}"));
	TH_CHECK(nested.find("nested content") != std::string::npos);
	robot->deactivate();
}

/* read_file 的沙箱负例：绝对路径 / .. / 空 一律按**参数错误**回传（SAN-8②，
 * 判据是异常类型 IllegalArgumentException；executed 置 false 是 DispatchGate
 * 的归一化职责，不在 AgentToolRegistry::execute 这一层，故这里只钉 ok/paramError
 * ——与 test_agenttools_wait.cpp 的 agenttools_wait_variable_param_errors 同惯例）。 */
TH_TEST(agenttools_fs_read_file_sandbox_violations_are_param_errors) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-sandbox-neg");
	writeFile(root.path() / "ok.txt", "ok");

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	for (const char* bad : {"/etc/passwd", "../x.txt", "a/../../b.txt", ".", ""}) {
		AgentToolResult r = tools.execute("read_file",
			bad[0] == '\0' ? parseJson("{}") : parseJson(std::string("{\"path\":\"")
				+ bad + "\"}"));
		TH_CHECK_MSG(r.ok == false && r.paramError == true,
			std::string("bad=") + bad + " text=" + r.text);
	}
	robot->deactivate();
}

/* read_file 拒绝越过根目录的符号链接（目标已存在），不泄露目标内容。 */
TH_TEST(agenttools_fs_read_file_rejects_symlink_escape) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-readfile-symlink");
	TempDir outside("fsroot-readfile-symlink-target");
	writeFile(outside.path() / "secret.txt", "机密内容不该泄露");
	TH_CHECK(::symlink((outside.path() / "secret.txt").c_str(),
		(root.path() / "link.txt").c_str()) == 0);

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	AgentToolResult r = tools.execute("read_file", parseJson("{\"path\":\"link.txt\"}"));
	TH_CHECK(r.ok == false);
	TH_CHECK(r.text.find("机密内容") == std::string::npos);
	robot->deactivate();
}

/* read_file 拒绝超过字节上限的文件（响亮拒绝，不静默截断）。 */
TH_TEST(agenttools_fs_read_file_rejects_oversized_file) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-readfile-oversize");
	std::string big(70u * 1024, 'x');	/* > 64 KiB 上限 */
	writeFile(root.path() / "big.txt", big);

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	AgentToolResult r = tools.execute("read_file", parseJson("{\"path\":\"big.txt\"}"));
	TH_CHECK(r.ok == false);
	TH_CHECK(r.text.find("上限") != std::string::npos);
	robot->deactivate();
}

/* read_file 拒绝二进制内容（含 NUL），不 base64 塞进上下文。 */
TH_TEST(agenttools_fs_read_file_rejects_binary_content) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-readfile-binary");
	writeFile(root.path() / "bin.dat", std::string("abc\0def", 7));

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	AgentToolResult r = tools.execute("read_file", parseJson("{\"path\":\"bin.dat\"}"));
	TH_CHECK(r.ok == false);
	TH_CHECK(r.text.find("UTF-8") != std::string::npos || r.text.find("二进制") != std::string::npos);
	robot->deactivate();
}

/* read_file 不存在的文件：执行结果失败（不是参数错误——路径本身合法）。 */
TH_TEST(agenttools_fs_read_file_missing_file_is_execution_failure_not_param_error) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-readfile-missing");

	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	AgentToolResult r = tools.execute("read_file", parseJson("{\"path\":\"no-such-file.txt\"}"));
	TH_CHECK(r.ok == false);
	TH_CHECK(r.paramError == false);
	robot->deactivate();
}

/* read_file 缺 path 参数：缺字符串参数，派发漏斗统一归为"工具参数错误"
 * （本测试直接钉工具层异常，不经 DispatchGate）。 */
TH_TEST(agenttools_fs_read_file_requires_path) {
	auto robot = makeRelayRobot();
	TempDir root("fsroot-readfile-nopath");
	AgentToolContext ctx = fsContext(robot.get(), root.path().string());
	AgentToolRegistry tools = makeAgentTools(ctx);

	AgentToolResult r = tools.execute("read_file", parseJson("{}"));
	TH_CHECK(r.ok == false);
	robot->deactivate();
}
