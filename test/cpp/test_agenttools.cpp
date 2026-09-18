#include "harness.hpp"

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

class TempDir {
	fs::path dir_;

public:
	explicit TempDir(const std::string& tag) {
		dir_ = fs::temp_directory_path()
			/ ("mybot_agent_" + tag + "_" + std::to_string(::getpid()));
		std::error_code ec;
		fs::remove_all(dir_, ec);
		fs::create_directories(dir_);
	}
	~TempDir() {
		std::error_code ec;
		fs::remove_all(dir_, ec);
	}
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
	const fs::path& path() const { return dir_; }
};

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
