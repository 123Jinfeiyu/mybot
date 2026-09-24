/* test_mcpbackend.cpp —— MCP 后端（--mcp-backend，方案 F / 拓扑 F3）单测
 *
 * 覆盖**纯协议件**（帧编码、UTF-8 校验、路由、能力门控、预算再强制、错误脱敏）
 * 与 SO_PEERCRED 校验——都不依赖真实硬件或真实 socket 连接。socket 收发全链路
 * （readFrame/sendFrame 的 deadline recv、8 MiB 上限拒收）由 scripts/mcp_e2e.sh
 * 端到端覆盖（M6），因为那两个函数是 mcpbackend.cpp 的文件私有件。
 *
 * 工具派发用两条路：① 手搓注册表 + 假工具（隔离协议逻辑，无需 robot）；
 * ② relay-test 虚拟机器人 + makeAgentTools（验证与 agent 模式同源的能力门控与
 * 真实只读工具派发）。 */

#include "fixtures.hpp"
#include "harness.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agenttools.hpp"
#include "jsonparse.hpp"
#include "mcpbackend.hpp"
#include "robot.hpp"
#include "util.hpp"

using namespace bot;
using namespace bot::agent;

namespace {

std::string readAll(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("测试夹具不可读: " + path); }
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 无硬件自检机器人（relay-test）：与 test_agenttools.cpp 同款夹具 */
std::unique_ptr<RobotStruct> makeRelayRobot() {
	std::string sql = readAll("robots/relay-test.sql");
	MemoryReader reader(std::move(sql));
	auto robot = std::make_unique<RobotStruct>(reader);
	robot->active();
	return robot;
}

/* 一个只回固定文本的假工具（隔离协议逻辑，不碰硬件） */
AgentTool echoTool() {
	AgentTool t;
	t.name = "echo";
	t.description = "回显工具（测试用）";
	t.parameters = JsonObject();
	t.execute = [](const Json&) -> AgentToolResult { return AgentToolResult{"echoed", ""}; };
	return t;
}

/* 记录派发次数的假工具（验证预算超限时**不**派发） */
AgentTool countingTool(int& counter) {
	AgentTool t;
	t.name = "count";
	t.description = "计数工具（测试用）";
	t.parameters = JsonObject();
	t.execute = [&counter](const Json&) -> AgentToolResult {
		++counter;
		return AgentToolResult{"counted", ""};
	};
	return t;
}

bool respOk(const Json& r) {
	const Json* s = r.find("success");
	return s != nullptr && s->isBool() && s->getBool();
}

std::string respError(const Json& r) {
	const Json* e = r.find("error");
	return (e != nullptr && e->isStr()) ? e->getValue() : std::string();
}

const Json* respData(const Json& r) { return r.find("data"); }

bool toolsHas(const Json& toolsArr, const std::string& name) {
	for (std::size_t i = 0; i < toolsArr.size(); ++i) {
		const Json* n = toolsArr.at(i).find("name");
		if (n != nullptr && n->isStr() && n->getValue() == name) { return true; }
	}
	return false;
}

McpBackend makeBackend(AgentToolRegistry reg, long callBudget = 0, long wallClock = 0) {
	return McpBackend(std::move(reg), nullptr, nullptr, callBudget, wallClock);
}

} // namespace

/* ---- 帧编码：4 字节 big-endian 长度 + 载荷（与 pymsg/gomsg/libmsg 字节兼容）---- */
TH_TEST(mcpbackend_frame_encode_is_4byte_be_plus_payload) {
	const std::string f = mcpEncodeFrame("abc");
	TH_CHECK(f.size() == 7);
	TH_CHECK(static_cast<unsigned char>(f[0]) == 0u);
	TH_CHECK(static_cast<unsigned char>(f[1]) == 0u);
	TH_CHECK(static_cast<unsigned char>(f[2]) == 0u);
	TH_CHECK(static_cast<unsigned char>(f[3]) == 3u);
	TH_CHECK(f.compare(4, 3, "abc") == 0);

	/* 空载荷合法（libmsg 口径）：只有 4 字节头，长度 0 */
	const std::string e = mcpEncodeFrame("");
	TH_CHECK(e.size() == kMcpHeaderSize);
	TH_CHECK(static_cast<unsigned char>(e[3]) == 0u);

	/* 300 = 0x0000012C：验证多字节 BE 编码正确 */
	const std::string big = mcpEncodeFrame(std::string(300, 'x'));
	TH_CHECK(big.size() == 304);
	TH_CHECK(static_cast<unsigned char>(big[2]) == 0x01u);
	TH_CHECK(static_cast<unsigned char>(big[3]) == 0x2Cu);
}

/* ---- 严格 UTF-8 校验：拒 overlong / 代理区 / 截断 / 超范围 ---- */
TH_TEST(mcpbackend_valid_utf8_accepts_and_rejects) {
	TH_CHECK(mcpValidUtf8("hello"));			/* ASCII */
	TH_CHECK(mcpValidUtf8("\xe4\xb8\xad"));		/* 中（3 字节）*/
	TH_CHECK(mcpValidUtf8("\xf0\x9f\x98\x80"));	/* 😀（4 字节）*/
	TH_CHECK(mcpValidUtf8(""));			/* 空串合法 */

	TH_CHECK(!mcpValidUtf8("\xc0\x80"));		/* overlong 编码的 NUL */
	TH_CHECK(!mcpValidUtf8("\xed\xa0\x80"));	/* 代理区 D800 */
	TH_CHECK(!mcpValidUtf8("\xe4\xb8"));		/* 3 字节序列被截断 */
	TH_CHECK(!mcpValidUtf8("\x80"));		/* 续字节打头 */
	TH_CHECK(!mcpValidUtf8("\xf5\x80\x80\x80"));	/* > U+10FFFF */
}

/* ---- ping 路由 ---- */
TH_TEST(mcpbackend_ping_route) {
	McpBackend b = makeBackend(AgentToolRegistry{});
	const Json r = parseJson(b.handleFrame(R"({"_route":"ping"})"));
	TH_CHECK(respOk(r));
	const Json* data = respData(r);
	TH_CHECK(data != nullptr);
	const Json* pong = data->find("pong");
	TH_CHECK(pong != nullptr && pong->isBool() && pong->getBool());
}

/* ---- 非法 JSON / 顶层非对象 → 固定串，从不回显输入 ---- */
TH_TEST(mcpbackend_invalid_json_returns_fixed_string_without_echo) {
	McpBackend b = makeBackend(AgentToolRegistry{});

	const Json r1 = parseJson(b.handleFrame("this is not json"));
	TH_CHECK(!respOk(r1));
	TH_CHECK(respError(r1) == "invalid JSON request");
	TH_CHECK(respError(r1).find("not json") == std::string::npos);	/* 不回显 */

	/* 合法 JSON 但顶层是数组：与 C++ RpcServer 同口径判为非法请求 */
	const Json r2 = parseJson(b.handleFrame("[1,2]"));
	TH_CHECK(!respOk(r2));
	TH_CHECK(respError(r2) == "invalid JSON request");
}

/* ---- 缺失 / 非串 / 未知 _route ---- */
TH_TEST(mcpbackend_route_errors) {
	McpBackend b = makeBackend(AgentToolRegistry{});

	const Json missing = parseJson(b.handleFrame("{}"));
	TH_CHECK(!respOk(missing));
	TH_CHECK(respError(missing) == "missing _route");

	const Json notStr = parseJson(b.handleFrame(R"({"_route":42})"));
	TH_CHECK(!respOk(notStr));
	TH_CHECK(respError(notStr) == "_route must be a string");

	const Json unknown = parseJson(b.handleFrame(R"({"_route":"nope"})"));
	TH_CHECK(!respOk(unknown));
	TH_CHECK(respError(unknown).find("unknown route: nope") != std::string::npos);
}

/* ---- tool.list 反映注册表（含 inputSchema）---- */
TH_TEST(mcpbackend_tool_list_reflects_registry) {
	AgentToolRegistry reg;
	reg.add(echoTool());
	McpBackend b = makeBackend(std::move(reg));

	const Json r = parseJson(b.handleFrame(R"({"_route":"tool.list"})"));
	TH_CHECK(respOk(r));
	const Json* data = respData(r);
	TH_CHECK(data != nullptr);
	const Json* tools = data->find("tools");
	TH_CHECK(tools != nullptr && tools->isArr());
	TH_CHECK(tools->size() == 1);
	TH_CHECK(toolsHas(*tools, "echo"));
	/* MCP 的 inputSchema 直传工具的 parameters（JSON Schema object） */
	const Json* schema = tools->at(0).find("inputSchema");
	TH_CHECK(schema != nullptr && schema->isObj());
}

/* ---- tool.call 穿过 DispatchGate 派发 ---- */
TH_TEST(mcpbackend_tool_call_dispatches_through_gate) {
	AgentToolRegistry reg;
	reg.add(echoTool());
	McpBackend b = makeBackend(std::move(reg));

	const Json r = parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"echo","arguments":{}})"));
	TH_CHECK(respOk(r));	/* 协议层成功 */
	const Json* data = respData(r);
	TH_CHECK(data != nullptr);
	TH_CHECK(data->find("text")->getValue() == "echoed");
	TH_CHECK(data->find("ok")->getBool());
	TH_CHECK(data->find("executed")->getBool());
}

/* ---- 未知工具：协议成功，但 data.ok=false、executed=false（副作用未发生）---- */
TH_TEST(mcpbackend_unknown_tool_reports_not_executed) {
	AgentToolRegistry reg;
	reg.add(echoTool());
	McpBackend b = makeBackend(std::move(reg));

	const Json r = parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"ghost","arguments":{}})"));
	TH_CHECK(respOk(r));
	const Json* data = respData(r);
	TH_CHECK(data != nullptr);
	TH_CHECK(!data->find("ok")->getBool());
	TH_CHECK(!data->find("executed")->getBool());
	TH_CHECK(data->find("text")->getValue().find("未知工具") != std::string::npos);
}

/* ---- tool.call 参数形态错误 ---- */
TH_TEST(mcpbackend_tool_call_argument_shape_errors) {
	AgentToolRegistry reg;
	reg.add(echoTool());
	McpBackend b = makeBackend(std::move(reg));

	const Json noName = parseJson(b.handleFrame(R"({"_route":"tool.call","arguments":{}})"));
	TH_CHECK(!respOk(noName));
	TH_CHECK(respError(noName).find("name") != std::string::npos);

	const Json badArgs = parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"echo","arguments":5})"));
	TH_CHECK(!respOk(badArgs));
	TH_CHECK(respError(badArgs).find("arguments") != std::string::npos);
}

/* ---- 预算再强制：超限拒绝且**不派发**（无副作用，评估 §3.2）---- */
TH_TEST(mcpbackend_call_budget_reenforced_without_dispatch) {
	int counter = 0;
	AgentToolRegistry reg;
	reg.add(countingTool(counter));
	McpBackend b = makeBackend(std::move(reg), /*callBudget=*/1, /*wallClock=*/0);

	const Json first = parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"count","arguments":{}})"));
	TH_CHECK(respOk(first));
	TH_CHECK(counter == 1);

	const Json second = parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"count","arguments":{}})"));
	TH_CHECK(!respOk(second));
	TH_CHECK(respError(second).find("上限") != std::string::npos);
	TH_CHECK(counter == 1);	/* 第二次**没有**派发：计数不变 */
}

/* ---- beginConnection 重置每连接预算 ---- */
TH_TEST(mcpbackend_begin_connection_resets_budget) {
	int counter = 0;
	AgentToolRegistry reg;
	reg.add(countingTool(counter));
	McpBackend b = makeBackend(std::move(reg), /*callBudget=*/1, /*wallClock=*/0);

	TH_CHECK(respOk(parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"count","arguments":{}})"))));
	TH_CHECK(!respOk(parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"count","arguments":{}})"))));

	b.beginConnection();	/* 新连接：预算恢复 */
	TH_CHECK(respOk(parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"count","arguments":{}})"))));
	TH_CHECK(counter == 2);
}

/* ---- 能力门控体现在 tool.list：未开 --agent-shell 则 run_command 不出现 ---- */
TH_TEST(mcpbackend_capability_gating_in_tool_list) {
	auto robot = makeRelayRobot();

	AgentToolContext off;
	off.robot = robot.get();
	off.scriptsDir = "scripts";
	off.allowShell = false;
	McpBackend bOff = makeBackend(makeAgentTools(off));
	const Json rOff = parseJson(bOff.handleFrame(R"({"_route":"tool.list"})"));
	const Json* toolsOff = respData(rOff)->find("tools");
	TH_CHECK(toolsOff != nullptr);
	TH_CHECK(toolsHas(*toolsOff, "run_action_script"));	/* 缺省工具在 */
	TH_CHECK(!toolsHas(*toolsOff, "run_command"));		/* 未开 shell：不宣告 */

	AgentToolContext on;
	on.robot = robot.get();
	on.scriptsDir = "scripts";
	on.allowShell = true;
	McpBackend bOn = makeBackend(makeAgentTools(on));
	const Json rOn = parseJson(bOn.handleFrame(R"({"_route":"tool.list"})"));
	const Json* toolsOn = respData(rOn)->find("tools");
	TH_CHECK(toolsOn != nullptr);
	TH_CHECK(toolsHas(*toolsOn, "run_command"));		/* 开了 shell：宣告 */
}

/* ---- 真实只读工具派发（relay 虚拟机器人，无硬件副作用）---- */
TH_TEST(mcpbackend_tool_call_real_robot_readonly) {
	auto robot = makeRelayRobot();
	AgentToolContext ctx;
	ctx.robot = robot.get();
	ctx.scriptsDir = "scripts";
	McpBackend b = makeBackend(makeAgentTools(ctx));

	const Json r = parseJson(
		b.handleFrame(R"({"_route":"tool.call","name":"list_hardware","arguments":{}})"));
	TH_CHECK(respOk(r));
	const Json* data = respData(r);
	TH_CHECK(data != nullptr);
	TH_CHECK(data->find("ok")->getBool());
	TH_CHECK(!data->find("text")->getValue().empty());
}

/* ---- SO_PEERCRED：同进程 socketpair 匹配；非 socket fd 读取失败即拒 ---- */
TH_TEST(mcpbackend_peercred_matches_self_and_rejects_non_socket) {
	std::ostringstream log;

	int fds[2] = {-1, -1};
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		TH_SKIP("socketpair 不可用");
	}
	/* 同进程两端：uid/gid 必然一致 → 放行 */
	TH_CHECK(mcpCheckPeerCred(fds[0], log));
	::close(fds[0]);
	::close(fds[1]);

	/* 非 socket fd：getsockopt(SO_PEERCRED) 失败 → 拒（覆盖失败分支）*/
	const int fd = ::open("/dev/null", O_RDONLY);
	TH_CHECK(fd >= 0);
	TH_CHECK(!mcpCheckPeerCred(fd, log));
	::close(fd);
}
