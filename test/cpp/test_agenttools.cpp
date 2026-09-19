#include "fixtures.hpp"
#include "harness.hpp"

using fx::TempDir;

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>

#include "agenttools.hpp"
#include "jsonparse.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

using namespace bot;
using namespace bot::agent;

namespace {

std::string readAll(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("测试夹具不可读: " + path); }
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 无硬件自检机器人（relay-test）：结构脚本与动作脚本都在仓库里，
 * agent 工具的真实执行路径可以完全离线跑通 */
std::unique_ptr<RobotStruct> makeRelayRobot() {
	std::string sql = readAll("robots/relay-test.sql");
	MemoryReader reader(std::move(sql));
	auto robot = std::make_unique<RobotStruct>(reader);
	robot->active();
	return robot;
}

void writeFile(const fs::path& p, const std::string& content) {
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary);
	f << content;
}

/* 绝大多数工具只回文本；测试里只关心文本时用这个薄封装 */
std::string execText(const AgentToolRegistry& reg, const std::string& name, const Json& args) {
	return reg.execute(name, args).text;
}

} // namespace

TH_TEST(agenttools_script_catalog) {
	TempDir tmp("catalog");
	writeFile(tmp.path() / "std.mt", "# 站立动作\n@Action\na.A;\n1;\n@\n");
	writeFile(tmp.path() / "notes.txt", "not a script");
	writeFile(tmp.path() / "sub" / "walk.mt", "# 前进动作\n@Action\n");
	writeFile(tmp.path() / "empty.mt", "@Action\n");

	std::string list = describeActionScripts(tmp.path().string());
	TH_CHECK(list.find("std.mt") != std::string::npos);
	TH_CHECK(list.find("站立动作") != std::string::npos);
	TH_CHECK(list.find("sub/walk.mt") != std::string::npos);
	TH_CHECK(list.find("前进动作") != std::string::npos);
	TH_CHECK(list.find("empty.mt") != std::string::npos);	/* 无注释也列出 */
	TH_CHECK(list.find("notes.txt") == std::string::npos);

	TH_CHECK(describeActionScripts((tmp.path() / "missing").string()).find("不存在")
		!= std::string::npos);

	/* 尾斜杠的 scriptsDir 同样要能列出（相对路径按纯词法求，前缀匹配不受尾斜杠影响） */
	std::string slash = describeActionScripts(tmp.path().string() + "/");
	TH_CHECK(slash.find("std.mt") != std::string::npos);
	TH_CHECK(slash.find("站立动作") != std::string::npos);
}

TH_TEST(agenttools_script_path_guard) {
	TH_CHECK(resolveActionScriptPath("scripts", "std.mt") == "scripts/std.mt");
	TH_CHECK(resolveActionScriptPath("scripts/", "test/pwm.mt") == "scripts/test/pwm.mt");
	TH_CHECK(resolveActionScriptPath("./scripts", "std.mt") == "./scripts/std.mt");

	/* 绝对路径 / 越出脚本目录 / 非 .mt / 空段一律拒绝 */
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", "/etc/passwd"));
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", "../x.mt"));
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", "a/../../b.mt"));
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", "std.lua"));
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", ""));
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", "a//b.mt"));
	TH_CHECK_THROWS(IllegalArgumentException, resolveActionScriptPath("scripts", "."));
}

TH_TEST(agenttools_run_action_script_on_resident_robot) {
	auto robot = makeRelayRobot();

	AgentToolContext ctx;
	ctx.robot = robot.get();
	ctx.scriptsDir = "scripts";
	ctx.structurePath = "robots/relay-test.sql";
	AgentToolRegistry tools = makeAgentTools(ctx);

	std::string first = execText(tools, "run_action_script",
		parseJson("{\"script\":\"relay-test.mt\"}"));
	TH_CHECK(first.find("已执行") != std::string::npos);

	/* agent 模式的核心性质：脚本跑完硬件不释放，第二个脚本照常执行 */
	std::string second = execText(tools, "run_action_script",
		parseJson("{\"script\":\"relay-test.mt\"}"));
	TH_CHECK(second.find("已执行") != std::string::npos);

	std::string escaped = execText(tools, "run_action_script",
		parseJson("{\"script\":\"../robots/relay-test.sql\"}"));
	TH_CHECK(escaped.find("失败") != std::string::npos);
	TH_CHECK(escaped.find("不允许") != std::string::npos);

	std::string missing = execText(tools, "run_action_script",
		parseJson("{\"script\":\"no-such.mt\"}"));
	TH_CHECK(missing.find("失败") != std::string::npos);

	std::string noArg = execText(tools, "run_action_script", parseJson("{}"));
	TH_CHECK(noArg.find("失败") != std::string::npos);

	robot->deactivate();
}

TH_TEST(agenttools_run_action_script_rejects_symlink) {
	/* 中危 #5：scripts/ 里的软链曾被 std::ifstream 跟随——能写 scripts/ 的本地
	 * 进程放一条软链，就能让目录外的任意文件被当动作脚本执行，且其首行注释还会
	 * 经 list_action_scripts 回显进模型上下文。现在 open(O_NOFOLLOW) 一律拒绝。 */
	auto robot = makeRelayRobot();
	TempDir scripts("symlink");
	TempDir outside("symlink-target");
	writeFile(scripts.path() / "real.mt", "# 正常脚本\n@Action\nrelay0.test;\n      200;\n@\n");
	writeFile(outside.path() / "secret.mt", "# 目录外机密\n@Action\nrelay0.test;\n      200;\n@\n");
	fs::path link = scripts.path() / "link.mt";
	TH_CHECK(::symlink((outside.path() / "secret.mt").c_str(), link.c_str()) == 0);

	AgentToolContext ctx;
	ctx.robot = robot.get();
	ctx.scriptsDir = scripts.path().string();
	ctx.structurePath = "robots/relay-test.sql";
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* 正常脚本不受影响 */
	std::string ok = execText(tools, "run_action_script", parseJson("{\"script\":\"real.mt\"}"));
	TH_CHECK(ok.find("已执行") != std::string::npos);

	/* 软链被拒，且不泄露目标内容 */
	std::string bad = execText(tools, "run_action_script", parseJson("{\"script\":\"link.mt\"}"));
	TH_CHECK(bad.find("失败") != std::string::npos);
	TH_CHECK(bad.find("符号链接") != std::string::npos);
	TH_CHECK(bad.find("机密") == std::string::npos);

	/* 清单根本不列软链：既不给越界文件留入口，也不回显其首注释 */
	std::string list = execText(tools, "list_action_scripts", parseJson("{}"));
	TH_CHECK(list.find("link.mt") == std::string::npos);
	TH_CHECK(list.find("目录外机密") == std::string::npos);
	TH_CHECK(list.find("正常脚本") != std::string::npos);

	robot->deactivate();
}

TH_TEST(agenttools_list_hardware) {
	auto robot = makeRelayRobot();
	std::string hw = describeHardware(*robot);
	TH_CHECK(hw.find("relay0") != std::string::npos);
	TH_CHECK(hw.find("timer-0") != std::string::npos);
	TH_CHECK(hw.find("驱动") != std::string::npos);

	AgentToolContext ctx;
	ctx.robot = robot.get();
	ctx.structurePath = "robots/relay-test.sql";
	AgentToolRegistry tools = makeAgentTools(ctx);
	std::string out = execText(tools, "list_hardware", parseJson("{}"));
	TH_CHECK(out.find("relay0") != std::string::npos);
	TH_CHECK(out.find("robots/relay-test.sql") != std::string::npos);

	robot->deactivate();
}

TH_TEST(agenttools_registry_dispatch) {
	AgentToolContext ctx;
	ctx.robot = nullptr;
	ctx.allowShell = false;
	AgentToolRegistry tools = makeAgentTools(ctx);

	TH_CHECK(tools.contains("list_action_scripts"));
	TH_CHECK(tools.contains("run_action_script"));
	TH_CHECK(tools.contains("list_hardware"));
	TH_CHECK(!tools.contains("run_command"));	/* --agent-no-shell */

	std::vector<ToolSpec> specs = tools.specs();
	TH_CHECK(specs.size() == 3);
	TH_CHECK(specs[0].parameters.isObj());

	std::string unknown = execText(tools, "no_such_tool", parseJson("{}"));
	TH_CHECK(unknown.find("未知工具") != std::string::npos);
	TH_CHECK(unknown.find("list_action_scripts") != std::string::npos);
}

TH_TEST(agenttools_capture_image_via_real_camera_driver) {
	/* 用**真驱动** modules/Camera.lua 的仿真模式（src=<文件>）跑通「拍照 → 图片通道」，
	 * 不需要任何摄像头硬件；返回的 base64 必须与源文件逐字节一致 */
	TempDir tmp("cam");
	fs::path pic = tmp.path() / "pic.jpg";
	writeFile(pic, "foobar");	/* base64("foobar") == "Zm9vYmFy" */

	MemoryReader reader("CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n");
	RobotStruct robot(reader);
	robot.injectLuaDriver("Camera", "cam0");
	robot.active();

	AgentToolContext ctx;
	ctx.robot = &robot;
	ctx.camera = robot.registry().find("cam0");
	TH_CHECK(ctx.camera != nullptr);
	ctx.cameraSrc = pic.string();
	AgentToolRegistry tools = makeAgentTools(ctx);
	TH_CHECK(tools.contains("capture_image"));

	AgentToolResult res = tools.execute("capture_image", parseJson("{}"));
	TH_CHECK(res.text.find("已拍摄照片") != std::string::npos);
	TH_CHECK(res.text.find("模拟图片") != std::string::npos);
	TH_CHECK(res.text.find("6 字节") != std::string::npos);
	TH_CHECK(res.imageDataUrl == "data:image/jpeg;base64,Zm9vYmFy");

	robot.deactivate();
}

TH_TEST(agenttools_capture_image_reports_device_failure) {
	/* 设备打不开时要把驱动的 ERR 折成文本回给模型（不崩会话、也不给假图片） */
	MemoryReader reader("CREATE TABLE Timer (name String);\n"
		"INSERT INTO Timer (name) VALUES ('t0');\n");
	RobotStruct robot(reader);
	robot.injectLuaDriver("Camera", "cam0");
	robot.active();

	AgentToolContext ctx;
	ctx.robot = &robot;
	ctx.camera = robot.registry().find("cam0");
	ctx.cameraDevice = "/dev/video999";
	AgentToolRegistry tools = makeAgentTools(ctx);

	AgentToolResult res = tools.execute("capture_image", parseJson("{}"));
	TH_CHECK(res.imageDataUrl.empty());
	TH_CHECK(res.text.find("工具执行失败") != std::string::npos);
	TH_CHECK(res.text.find("/dev/video999") != std::string::npos);

	robot.deactivate();
}

TH_TEST(agenttools_no_camera_means_no_tool) {
	AgentToolContext ctx;
	ctx.robot = nullptr;
	ctx.allowShell = false;
	ctx.camera = nullptr;
	AgentToolRegistry tools = makeAgentTools(ctx);
	TH_CHECK(!tools.contains("capture_image"));
}

TH_TEST(agenttools_run_command_basic) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	std::string out = execText(tools, "run_command",
		parseJson("{\"command\":\"echo hello-agent\"}"));
	TH_CHECK(out.find("exit_code=0") != std::string::npos);
	TH_CHECK(out.find("hello-agent") != std::string::npos);

	std::string fails = execText(tools, "run_command",
		parseJson("{\"command\":\"exit 3\"}"));
	TH_CHECK(fails.find("exit_code=3") != std::string::npos);

	std::string noCmd = execText(tools, "run_command", parseJson("{}"));
	TH_CHECK(noCmd.find("失败") != std::string::npos);
}

TH_TEST(agenttools_run_command_cleans_env) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* 名字含 key/token/secret… 的环境变量必须被剔除：模型可被外部输入诱导
	 * 执行任意命令，绝不能让它读到 LLM API key */
	::setenv("BOT4CJ_TEST_SECRET_KEY", "leakme", 1);
	::setenv("BOT4CJ_TEST_PLAIN", "visible", 1);

	std::string out = execText(tools, "run_command", parseJson(
		"{\"command\":\"if printenv BOT4CJ_TEST_SECRET_KEY >/dev/null; then echo LEAKED; "
		"else echo CLEAN; fi; printenv BOT4CJ_TEST_PLAIN || echo MISSING\"}"));
	TH_CHECK(out.find("LEAKED") == std::string::npos);
	TH_CHECK(out.find("CLEAN") != std::string::npos);
	TH_CHECK(out.find("visible") != std::string::npos);

	::unsetenv("BOT4CJ_TEST_SECRET_KEY");
	::unsetenv("BOT4CJ_TEST_PLAIN");
}

TH_TEST(agenttools_run_command_stdin_is_devnull) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* fix-plan 3-1：子命令 stdin 必须指向 /dev/null——`cat` 无参读 stdin，
	 * 重定向前会一直等到超时（并与 REPL 抢输入）；重定向后立即 EOF 正常退出。 */
	std::string out = execText(tools, "run_command",
		parseJson("{\"command\":\"cat\",\"timeout_seconds\":2}"));
	TH_CHECK(out.find("exit_code=0") != std::string::npos);
	TH_CHECK(out.find("超时") == std::string::npos);
}

TH_TEST(agenttools_run_command_timeout_and_truncate) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* 超时：整进程组终止，不能挂住 agent 会话 */
	std::string slow = execText(tools, "run_command",
		parseJson("{\"command\":\"sleep 5\",\"timeout_seconds\":1}"));
	TH_CHECK(slow.find("超时") != std::string::npos);

	/* 输出截断：单个流上限 64KB */
	std::string big = execText(tools, "run_command",
		parseJson("{\"command\":\"yes a | head -c 200000\"}"));
	TH_CHECK(big.find("已截断") != std::string::npos);
}

TH_TEST(agenttools_run_command_truncation_is_per_stream) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* 上限是「每流」64KB，不是两流共享：只有触顶的那条流才应报截断。
	 * 原先用单一 truncated 标志，任一流触顶会把另一流也标成截断。 */
	std::string outOnly = execText(tools, "run_command",
		parseJson("{\"command\":\"yes a | head -c 200000\"}"));
	TH_CHECK(outOnly.find("[stdout 超过") != std::string::npos);
	TH_CHECK(outOnly.find("[stderr 超过") == std::string::npos);

	std::string errOnly = execText(tools, "run_command",
		parseJson("{\"command\":\"yes a | head -c 200000 1>&2\"}"));
	TH_CHECK(errOnly.find("[stderr 超过") != std::string::npos);
	TH_CHECK(errOnly.find("[stdout 超过") == std::string::npos);

	/* 两流都触顶：两条提示并存 */
	std::string both = execText(tools, "run_command",
		parseJson("{\"command\":\"yes a | head -c 200000; yes b | head -c 200000 1>&2\"}"));
	TH_CHECK(both.find("[stdout 超过") != std::string::npos);
	TH_CHECK(both.find("[stderr 超过") != std::string::npos);
}

TH_TEST(agenttools_run_command_rejects_out_of_range_timeout) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* P1-1 防线：模型可控的 timeout_seconds 若是 1e30 这类大浮点，直接
	 * static_cast<int> 是 float-cast-overflow UB（UBSan 实证）。必须折成
	 * 工具失败文本，且绝不能真去执行命令。 */
	std::string huge = execText(tools, "run_command",
		parseJson("{\"command\":\"echo SHOULD_NOT_RUN\",\"timeout_seconds\":1e30}"));
	TH_CHECK(huge.find("失败") != std::string::npos);
	TH_CHECK(huge.find("SHOULD_NOT_RUN") == std::string::npos);

	/* 负值不越界，按既有语义夹到下限 1 秒后照常执行 */
	std::string negative = execText(tools, "run_command",
		parseJson("{\"command\":\"echo ok-negative\",\"timeout_seconds\":-5}"));
	TH_CHECK(negative.find("exit_code=0") != std::string::npos);

	/* 区间内的小数正常取整（2.9 → 2） */
	std::string frac = execText(tools, "run_command",
		parseJson("{\"command\":\"echo ok-frac\",\"timeout_seconds\":2.9}"));
	TH_CHECK(frac.find("exit_code=0") != std::string::npos);
}

TH_TEST(agenttools_run_command_rejects_wrong_typed_timeout) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* 类型写错必须报错而非静默用缺省值：静默会让模型误以为参数已生效。
	 * 字符串/布尔/对象/数组四种错误类型都不能真去执行命令。 */
	for (const char* bad : {"\"10\"", "true", "{}", "[]"}) {
		std::string json = std::string("{\"command\":\"echo SHOULD_NOT_RUN\",\"timeout_seconds\":")
			+ bad + "}";
		std::string out = execText(tools, "run_command", parseJson(json));
		TH_CHECK(out.find("类型不符") != std::string::npos);
		TH_CHECK(out.find("SHOULD_NOT_RUN") == std::string::npos);
	}

	/* 键缺失与显式 null 都算「未提供」：走缺省超时，照常执行 */
	std::string absent = execText(tools, "run_command",
		parseJson("{\"command\":\"echo ok-absent\"}"));
	TH_CHECK(absent.find("exit_code=0") != std::string::npos);

	std::string nulled = execText(tools, "run_command",
		parseJson("{\"command\":\"echo ok-null\",\"timeout_seconds\":null}"));
	TH_CHECK(nulled.find("exit_code=0") != std::string::npos);
}

TH_TEST(agenttools_run_command_interrupts_on_stop_signal) {
	AgentToolContext ctx;
	ctx.allowShell = true;
	AgentToolRegistry tools = makeAgentTools(ctx);

	/* 停止信号（SIGINT/SIGTERM 置位全局标志）应整组终止子进程并如实标注「被中断」，
	 * 而不是等满 timeout——该分支此前无用例覆盖（0.6 改动其成因记录语义）。 */
	struct StopFlagGuard {
		StopFlagGuard() { stopRequested() = true; }
		~StopFlagGuard() { stopRequested() = false; }	/* 全局标志，勿污染后续用例 */
	} guard;

	std::string out = execText(tools, "run_command",
		parseJson("{\"command\":\"sleep 5\",\"timeout_seconds\":30}"));
	TH_CHECK(out.find("被中断") != std::string::npos);
}
