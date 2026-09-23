#include "harness.hpp"

#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentdriver.hpp"
#include "agentloop.hpp"
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

TH_TEST(agentdriver_retry_goes_to_injected_log_sink) {
	ModulePathGuard guard("modules:test/cpp");
	auto robot = makeRobot(kTimerRobotSql);
	robot->injectLuaDriver("AgentFakeDriver", "llm0");
	robot->active();

	/* C5：重试信息原先硬编码 std::cerr（离线用例抓不到、宿主没得选），
	 * 现在经 AgentDriverConfig::log 注入；缺省仍是 stderr（行为保持）。 */
	std::ostringstream sink;
	AgentDriverConfig cfg;
	cfg.model = "netfail-once";
	cfg.maxRetries = 3;
	cfg.retryBackoffMs = 0;		/* 用例不该真等退避 */
	cfg.log = &sink;
	auto transport = makeDriverTransport(robot->registry().find("llm0"), cfg);

	/* 首次网络类失败被重试，第二次成功（重试不改语义，只是多试一次） */
	const ChatResponse resp = transport->complete({msg("user", "x")}, {});
	TH_CHECK_MSG(resp.content.find("msgs=1") != std::string::npos, resp.content);
	TH_CHECK_MSG(sink.str().find("网络错误") != std::string::npos, sink.str());
	TH_CHECK_MSG(sink.str().find("1/3") != std::string::npos, sink.str());

	/* maxRetries = 0（配置文件的「不重试」）必须仍然**发一次**：旧循环直接拿它
	 * 当上界，会一次都不发并落到 logic_error("重试逻辑错误")——那是「什么都不做」，
	 * 不是「不重试」。断言抛的是驱动给的成因，且没有重试日志。 */
	std::ostringstream sink2;
	AgentDriverConfig once;
	once.model = "netfail-always";
	once.maxRetries = 0;
	once.retryBackoffMs = 0;
	once.log = &sink2;
	auto noRetry = makeDriverTransport(robot->registry().find("llm0"), once);
	TH_CHECK_THROWS_MSG(std::runtime_error, "curl error",
		noRetry->complete({msg("user", "x")}, {}));
	TH_CHECK_MSG(sink2.str().empty(), sink2.str());

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
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "必须是 Lua 驱动",
		makeDriverTransport(robot->registry().find("A"), cfg));

	robot->deactivate();
}

TH_TEST(agentdriver_null_driver_rejected) {
	AgentDriverConfig cfg;
	TH_CHECK_THROWS(IllegalArgumentException, makeDriverTransport(nullptr, cfg));
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

/* ============ SAN-3：错误的可重试分类（纯函数，离线穷举） ============
 * 这是"重试到底认不认得出真实错误"的判据单点。旧实现按人话子串判定，而生产
 * 路径的错误形态（curl rc=N: <散文>、HTTP 503: {...}）一个子串都不匹配——
 * 实测 503/429/真实超时/连接被拒各只发 1 次请求（见
 * .tmp/AGENT_NEXT_PLAN_20260922.md 的 SAN-3）。 */
namespace {

Json errTable(const std::string& json) { return parseJson(json); }

} // namespace

TH_TEST(agentdriver_classify_http_status) {
	/* 429 = 限流（退避要尊重服务端的 Retry-After） */
	{
		DriverError e = classifyDriverError(
			errTable("{\"result\":\"ERR\",\"message\":\"HTTP 429\",\"http_status\":429,"
				 "\"retriable\":true,\"retry_after_ms\":2000}"),
			"fallback");
		TH_CHECK(e.cls == RetryClass::Throttled);
		TH_CHECK(e.retryAfterMs == 2000);
		TH_CHECK(e.message == "HTTP 429");
	}
	/* 5xx = 瞬时（重试有意义） */
	for (long long code : {500LL, 502LL, 503LL, 504LL}) {
		DriverError e = classifyDriverError(
			errTable("{\"result\":\"ERR\",\"message\":\"HTTP " + std::to_string(code)
				+ "\",\"http_status\":" + std::to_string(code) + ",\"retriable\":true}"),
			"fallback");
		TH_CHECK_MSG(e.cls == RetryClass::Transient, std::to_string(code));
	}
	/* 4xx（429 除外）= 别再试了：重试不会改变结果，只烧配额 */
	for (long long code : {400LL, 401LL, 403LL, 404LL, 422LL}) {
		DriverError e = classifyDriverError(
			errTable("{\"result\":\"ERR\",\"message\":\"HTTP " + std::to_string(code)
				+ "\",\"http_status\":" + std::to_string(code) + ",\"retriable\":false}"),
			"fallback");
		TH_CHECK_MSG(e.cls == RetryClass::No, std::to_string(code));
	}
}

TH_TEST(agentdriver_classify_curl_rc) {
	/* 连接类退出码可重试（含真实形态 curl rc=7 / rc=28） */
	for (long long rc : {6LL, 7LL, 28LL, 35LL, 52LL, 55LL, 56LL}) {
		DriverError e = classifyDriverError(
			errTable("{\"result\":\"ERR\",\"message\":\"curl rc=" + std::to_string(rc)
				+ ": whatever\",\"curl_rc\":" + std::to_string(rc) + "}"),
			"fallback");
		TH_CHECK_MSG(e.cls == RetryClass::Transient, std::to_string(rc));
	}
	/* 配置/凭据类退出码**不**可重试 */
	for (long long rc : {1LL, 3LL, 60LL, 67LL}) {
		DriverError e = classifyDriverError(
			errTable("{\"result\":\"ERR\",\"message\":\"curl rc=" + std::to_string(rc)
				+ ": whatever\",\"curl_rc\":" + std::to_string(rc) + "}"),
			"fallback");
		TH_CHECK_MSG(e.cls == RetryClass::No, std::to_string(rc));
	}
}

TH_TEST(agentdriver_classify_retry_after_clamping) {
	const auto after = [](long long ms) {
		return classifyDriverError(
			errTable("{\"result\":\"ERR\",\"message\":\"HTTP 429\",\"http_status\":429,"
				 "\"retry_after_ms\":" + std::to_string(ms) + "}"),
			"f").retryAfterMs;
	};
	TH_CHECK(after(0) == 0);				/* 0 = 没给 */
	TH_CHECK(after(-5) == 0);				/* 负数当没给 */
	TH_CHECK(after(1) == kRetryAfterMinMs);			/* 太短对限流无意义 */
	TH_CHECK(after(2000) == 2000);				/* 区间内原样 */
	TH_CHECK(after(86400LL * 1000) == kRetryAfterMaxMs);	/* 坏网关不能挂死 agent */
}

TH_TEST(agentdriver_classify_falls_back_to_text_for_legacy_drivers) {
	/* 字段缺失时回落字符串判据：第三方/旧驱动仍可用（并且现在认得真实形态
	 * "curl rc=" ——旧表里没有它，正是重试失效的直接原因）。 */
	{
		Json t = errTable("{\"result\":\"ERR\",\"message\":\"curl rc=7: Could not connect\"}");
		TH_CHECK(classifyDriverError(t, "x").cls == RetryClass::Transient);
	}
	{
		Json t = errTable("{\"result\":\"ERR\",\"message\":\"curl error 7: 网络不可达\"}");
		TH_CHECK(classifyDriverError(t, "x").cls == RetryClass::Transient);
	}
	{
		Json t = errTable("{\"result\":\"ERR\",\"message\":\"API key 无效\"}");
		TH_CHECK(classifyDriverError(t, "x").cls == RetryClass::No);
	}
	/* 空表：用调用方给的兜底文案判定 */
	Json empty;
	TH_CHECK(classifyDriverError(empty, "curl rc=28: Timeout was reached").cls
		== RetryClass::Transient);
	TH_CHECK(classifyDriverError(empty, "模型既没有返回文本也没有工具调用").cls
		== RetryClass::No);
}

/* ============ SAN-6：漏 id 的网关下，跨轮 id 必须唯一 ============
 * 缺陷形态（已复现）：驱动曾自己合成 `tc.id or ("call_" .. i)`，而宿主**故意**
 * 把合成方案设计成轮次相关的（agentloop 的 call_<turn>_<i>，注释里写明就是为
 * "个别流式/兼容实现会漏 tool_call id"准备的）。驱动先合成 call_1 把宿主那条
 * 路径遮住，于是**每轮都从 call_1 重来**——第二轮的历史里出现两个同 id，真机
 * 400 answered more than once，且那条坏消息落在不可回滚的历史里。
 * 驱动改为透传（空串）后，宿主接手合成，跨轮唯一。
 *
 * 覆盖边界（如实声明）：本用例走的是 AgentFakeDriver 夹具（不经 Lua wire 层），
 * 所以它钉的是**宿主的合成方案**；"驱动不再合成 call_<i>"这一半由
 * test/test_llm_drivers.lua 的 SAN-6 用例钉（那边实测过反向验证：把驱动改回
 * tc.id or ("call_" .. i) → 两条 Lua 断言转红）。两半合起来才是完整判据。 */
TH_TEST(agentdriver_missing_ids_are_synthesized_by_host_per_turn) {
	ModulePathGuard mpath("./modules:./test/cpp");
	auto robot = makeRobot(kTimerRobotSql);
	robot->injectLuaDriver("AgentFakeDriver", "llm0");
	robot->active();

	MatrixVars_Hardware* drv = robot->registry().find("llm0");
	TH_CHECK(drv != nullptr);
	AgentDriverConfig cfg;
	cfg.model = "noid";
	auto transport = makeDriverTransport(drv, cfg);

	AgentToolRegistry tools;
	tools.add(AgentTool{"list_hardware", "只读清单", parseJson("{\"type\":\"object\"}"),
		[](const Json&) -> AgentToolResult { return {"(清单)", ""}; }});

	AgentOptions opts;
	opts.maxTurns = 5;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	AgentSession session(*transport, tools, opts);
	/* 夹具前两轮各回一条**无 id** 的工具调用，第三轮收尾 → 两轮 tool 回传 */
	TH_CHECK(session.runTurn("任务") == "收尾");

	/* 两轮各一条 tool 消息：id 必须**不同**（call_0_0 与 call_1_0），
	 * 且整段历史必须能被服务端接受（historyProtocolViolation 判据②）。 */
	const auto& h = session.history();
	std::vector<std::string> toolIds;
	for (const ChatMessage& m : h) {
		if (m.role == "tool") { toolIds.push_back(m.toolCallId); }
	}
	TH_CHECK_MSG(toolIds.size() == 2,
		"应有两轮各自的一条 tool 回传，实际 " + std::to_string(toolIds.size()));
	TH_CHECK_MSG(toolIds[0] != toolIds[1],
		"跨轮 id 必须不同，实际都是 '" + toolIds[0] + "'");
	TH_CHECK_MSG(toolIds[0] == "call_0_0" && toolIds[1] == "call_1_0",
		"宿主合成方案应为 call_<turn>_<i>，实际: " + toolIds[0] + " / " + toolIds[1]);
	TH_CHECK_MSG(historyProtocolViolation(h).empty(), historyProtocolViolation(h));

	robot->deactivate();
}
