#include "harness.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentdriver.hpp"
#include "agentproto.hpp"
#include "jsonparse.hpp"
#include "robot.hpp"
#include "util.hpp"

using namespace bot;
using namespace bot::agent;

namespace {

/* 让驱动解析路径能同时看到真模块树与测试夹具目录（modules 在前，
 * 与生产语义一致：同名以 modules 为准）。退出时还原，避免影响其它用例。 */
class ModulePathGuard {
	std::string old_;
	bool had_ = false;

public:
	explicit ModulePathGuard(const std::string& path) {
		const char* v = std::getenv("BOT4CJ_MODULE_PATH");
		had_ = v != nullptr;
		if (had_) { old_ = v; }
		::setenv("BOT4CJ_MODULE_PATH", path.c_str(), 1);
	}
	~ModulePathGuard() {
		if (had_) { ::setenv("BOT4CJ_MODULE_PATH", old_.c_str(), 1); }
		else { ::unsetenv("BOT4CJ_MODULE_PATH"); }
	}
	ModulePathGuard(const ModulePathGuard&) = delete;
	ModulePathGuard& operator=(const ModulePathGuard&) = delete;
};

constexpr const char* kTimerRobotSql =
	"CREATE TABLE Timer (name String);\n"
	"INSERT INTO Timer (name) VALUES ('t0');\n";

/* 只构造+解析：**不** active()。agent 模式的注入必须发生在 active() 之前，
 * 故调用方要显式控制「先注入、后装配」的顺序。 */
std::unique_ptr<RobotStruct> makeRobot(const std::string& sql) {
	MemoryReader reader(sql);
	return std::make_unique<RobotStruct>(reader);
}

ChatMessage msg(const std::string& role, const std::string& content) {
	ChatMessage m;
	m.role = role;
	m.content = content;
	return m;
}

} // namespace

TH_TEST(agentdriver_inject_then_transport_end_to_end) {
	ModulePathGuard guard("modules:test/cpp");
	auto robot = makeRobot(kTimerRobotSql);

	/* 结构脚本没声明 LLM 驱动 → 宿主按 --llm-driver 注入 */
	TH_CHECK(!robot->declaresVariable("llm0"));
	robot->injectLuaDriver("AgentFakeDriver", "llm0");
	TH_CHECK(robot->declaresVariable("llm0"));

	robot->active();	/* 注入必须在 active() 之前，变量是在那时装配的 */
	MatrixVars_Hardware* drv = robot->registry().find("llm0");
	TH_CHECK(drv != nullptr);

	AgentDriverConfig cfg;
	cfg.model = "m0";
	cfg.endpoint = "http://example.invalid/v1";
	cfg.maxTokens = 321;
	auto transport = makeDriverTransport(drv, cfg);

	std::vector<ChatMessage> messages{msg("system", "系统"), msg("user", "任务")};
	std::vector<ToolSpec> tools{
		ToolSpec{"list_hardware", "清单", parseJson("{\"type\":\"object\"}")}};

	ChatResponse resp = transport->complete(messages, tools);
	/* 驱动把「它收到的规范化请求」回显出来：证明组包、投递、解析三段都通了 */
	TH_CHECK(resp.content == "msgs=2 tools=1 model=m0 endpoint=http://example.invalid/v1 first=系统");
	TH_CHECK(resp.hasToolCalls());
	TH_CHECK(resp.toolCalls.size() == 1);
	TH_CHECK(resp.toolCalls[0].id == "c1");
	TH_CHECK(resp.toolCalls[0].name == "list_hardware");
	TH_CHECK(resp.extras.isObj());
	TH_CHECK(resp.extras.get("reasoning_content")->getValue() == "fake-think");

	robot->deactivate();
}

TH_TEST(agentdriver_driver_err_becomes_exception) {
	ModulePathGuard guard("modules:test/cpp");
	auto robot = makeRobot(kTimerRobotSql);
	robot->injectLuaDriver("AgentFakeDriver", "llm0");
	robot->active();

	AgentDriverConfig cfg;
	cfg.model = "fail";	/* 夹具靠 model 触发 ERR 契约 */
	auto transport = makeDriverTransport(robot->registry().find("llm0"), cfg);

	/* 驱动的 ERR 必须带着它给的 message 冒到宿主，而不是静默变成空回答 */
	TH_CHECK_THROWS_MSG(std::runtime_error, "假驱动故意失败",
		transport->complete({msg("user", "x")}, {}));

	robot->deactivate();
}

TH_TEST(agentdriver_rejects_non_lua_variable) {
	ModulePathGuard guard("modules:test/cpp");
	/* Alias 是标量硬件变量，没有 call 语义：绑成模型传输必须被拒绝 */
	auto robot = makeRobot(std::string(kTimerRobotSql)
		+ "CREATE TABLE Alias (name String, alias String);\n"
		+ "INSERT INTO Alias (name, alias) VALUES ('A', 'angle');\n");
	robot->active();

	AgentDriverConfig cfg;
	TH_CHECK_THROWS_MSG(std::invalid_argument, "必须是 Lua 驱动",
		makeDriverTransport(robot->registry().find("A"), cfg));

	robot->deactivate();
}

TH_TEST(agentdriver_null_driver_rejected) {
	AgentDriverConfig cfg;
	TH_CHECK_THROWS(std::invalid_argument, makeDriverTransport(nullptr, cfg));
}

TH_TEST(agentdriver_inject_rejects_unsafe_names) {
	MemoryReader reader(kTimerRobotSql);
	RobotStruct robot(reader);

	/* 名字会被拼进合成 SQL，且表名还会当 modules/<表名>.lua 解析——
	 * 放行引号/分号等于让命令行参数改写结构脚本 */
	TH_CHECK_THROWS(IllegalArgumentException, robot.injectLuaDriver("Bad;DROP TABLE X", "v"));
	TH_CHECK_THROWS(IllegalArgumentException, robot.injectLuaDriver("Ok", "v';DROP"));
	TH_CHECK_THROWS(IllegalArgumentException, robot.injectLuaDriver("", "v"));
	TH_CHECK_THROWS(IllegalArgumentException, robot.injectLuaDriver("1Bad", "v"));
	TH_CHECK(!robot.declaresVariable("v"));
}

TH_TEST(agentdriver_declared_variable_is_not_reinjected) {
	/* 结构脚本已声明同名变量时，宿主不应再注入（declaresVariable 是关键判据） */
	ModulePathGuard guard("modules:test/cpp");
	auto robot = makeRobot(std::string(kTimerRobotSql)
		+ "CREATE TABLE AgentFakeDriver (name String);\n"
		+ "INSERT INTO AgentFakeDriver (name) VALUES ('llm0');\n");
	robot->active();
	TH_CHECK(robot->declaresVariable("llm0"));
	TH_CHECK(robot->registry().find("llm0") != nullptr);
	robot->deactivate();
}
