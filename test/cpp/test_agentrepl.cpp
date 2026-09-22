/* test_agentrepl.cpp —— REPL 循环本体（runAgentRepl，C3 拆出）的离线单测
 *
 * 覆盖面：内置命令派发（不触发模型）、输入超限响亮拒绝且会话继续、
 * 单轮模型失败不结束会话、/exit 与 EOF 的退出码、状态提示符格式、
 * /clear 的会话面语义。
 * I/O 全经 istringstream/ostringstream 注入；机器人用 MemoryReader 内存装配，
 * 不碰真机也不碰 HTTP。
 */

#include "harness.hpp"
#include "fixtures.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentmode.hpp"
#include "robot.hpp"	/* RobotStruct 完整类型（MemoryReader 就地装配） */

namespace fs = std::filesystem;

using namespace bot;
using namespace bot::agent;

namespace {

void writeFile(const fs::path& p, const std::string& content, fs::perms mode) {
	std::ofstream f(p, std::ios::binary);
	f << content;
	f.close();
	fs::permissions(p, mode);
}

/* 脚本化传输：按预设序列返回响应，并记录每次请求的消息快照
 * （与 test_agentloop 的 FakeTransport 同构；独立一份，避免跨文件共享夹具） */
class ScriptTransport : public ChatTransport {
public:
	std::vector<ChatResponse> script;
	std::vector<std::vector<ChatMessage>> requests;

	ChatResponse complete(const std::vector<ChatMessage>& msgs,
			const std::vector<ToolSpec>&) override {
		requests.push_back(msgs);
		if (script.empty()) { throw std::runtime_error("脚本用尽"); }
		ChatResponse r = script.front();
		script.erase(script.begin());
		return r;
	}
};

/* 第一次调用必失败、之后成功的传输（错误恢复用例） */
class FailOnceTransport : public ChatTransport {
public:
	int calls = 0;
	ChatResponse complete(const std::vector<ChatMessage>&, const std::vector<ToolSpec>&) override {
		if (++calls == 1) { throw std::runtime_error("第一次失败"); }
		ChatResponse r;
		r.content = "第二轮好了";
		return r;
	}
};

ChatResponse answer(const std::string& text) {
	ChatResponse r;
	r.content = text;
	return r;
}

AgentOptions sessionOpts() {
	AgentOptions o;
	o.maxTurns = 4;
	o.verbose = false;
	o.systemPrompt = "测试系统提示词";
	return o;
}

AgentModeConfig replCfg(const std::string& scriptsDir) {
	AgentModeConfig cfg;
	cfg.structurePath = "robots/spider1.sql";	/* 只影响提示符里的名字展示 */
	cfg.scriptsDir = scriptsDir;
	cfg.llmVar = "llm0";
	return cfg;
}

/* 读仓库内小文本文件（robots/relay-test.sql 等；测试进程 cwd = 仓库根） */
std::string readSmallFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("测试夹具读不到: " + path); }
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

/* 带硬件变量的结构：直接用仓库 relay-test（与 test_agenttools 同款离线形态）。
 * RobotStruct 不可拷贝（unique_ptr 成员），各用例就地构造。 */
#define MAKE_ROBOT() \
	MemoryReader robotReader(readSmallFile("robots/relay-test.sql")); \
	RobotStruct robot(robotReader)

} // namespace

TH_TEST(agentrepl_exit_returns_zero_without_model_call) {
	ScriptTransport transport;	/* 空脚本：任何模型调用都会炸 */
	std::istringstream in("/exit\n任务B\n");
	std::ostringstream out, log;

	MAKE_ROBOT();
	const std::string prompt = "mybot[spider1|llm0|"
		+ std::to_string(robot.registry().byName().size()) + "hw]> ";
	AgentToolRegistry tools;
	AgentSession session(transport, tools, sessionOpts());
	TH_CHECK(runAgentRepl(session, replCfg("scripts"), robot, in, out, log) == 0);
	TH_CHECK(transport.requests.empty());	/* /exit 不该触发模型 */
	/* 状态提示符：结构脚本名去路径去扩展名 + llm 变量名 + 硬件数量 */
	TH_CHECK(out.str().find(prompt) != std::string::npos);
}

TH_TEST(agentrepl_eof_returns_zero) {
	ScriptTransport transport;
	std::istringstream in("");
	std::ostringstream out, log;

	MAKE_ROBOT();
	AgentToolRegistry tools;
	AgentSession session(transport, tools, sessionOpts());
	TH_CHECK(runAgentRepl(session, replCfg("scripts"), robot, in, out, log) == 0);
	TH_CHECK(transport.requests.empty());
}

TH_TEST(agentrepl_builtin_commands_do_not_call_model) {
	fx::TempDir tmp("replscripts");
	fs::path dir = tmp.path() / "scripts";
	fs::create_directories(dir);
	writeFile(dir / "wave.mt", "# 挥手\nPRINT 1\n", fs::perms::owner_all);

	ScriptTransport transport;
	std::istringstream in("/help\n/hardware\n/scripts\n/exit\n");
	std::ostringstream out, log;

	MAKE_ROBOT();
	robot.active();	/* 硬件变量在 active() 时才注册（同 test_agenttools 的离线形态） */
	AgentToolRegistry tools;
	AgentSession session(transport, tools, sessionOpts());
	TH_CHECK(runAgentRepl(session, replCfg(dir.string()), robot, in, out, log) == 0);
	TH_CHECK(transport.requests.empty());

	const std::string& o = out.str();
	TH_CHECK(o.find("内置命令") != std::string::npos);		/* /help */
	TH_CHECK(o.find("relay0") != std::string::npos);		/* /hardware：硬件清单 */
	TH_CHECK(o.find("wave.mt") != std::string::npos
		&& o.find("挥手") != std::string::npos);			/* /scripts：路径+首行注释 */
}

TH_TEST(agentrepl_model_error_does_not_end_session) {
	FailOnceTransport transport;
	std::istringstream in("任务A\n任务B\n/exit\n");
	std::ostringstream out, log;

	MAKE_ROBOT();
	AgentToolRegistry tools;
	AgentSession session(transport, tools, sessionOpts());
	TH_CHECK(runAgentRepl(session, replCfg("scripts"), robot, in, out, log) == 0);
	/* 第一轮失败写进 log（不结束会话），第二轮照常拿到回答 */
	TH_CHECK(log.str().find("错误: 第一次失败") != std::string::npos);
	TH_CHECK(out.str().find("第二轮好了") != std::string::npos);
}

TH_TEST(agentrepl_overlong_input_rejected_session_continues) {
	ScriptTransport transport;
	transport.script.push_back(answer("好了"));
	/* 上限 +1 字节：响亮拒绝而非截断；随后正常任务照常执行 */
	std::string big(kMaxReplInputBytes + 1, 'x');
	std::istringstream in(big + "\n任务A\n/exit\n");
	std::ostringstream out, log;

	MAKE_ROBOT();
	AgentToolRegistry tools;
	AgentSession session(transport, tools, sessionOpts());
	TH_CHECK(runAgentRepl(session, replCfg("scripts"), robot, in, out, log) == 0);
	TH_CHECK(log.str().find("输入过长") != std::string::npos);
	TH_CHECK(out.str().find("好了") != std::string::npos);
	TH_CHECK(transport.requests.size() == 1);
	if (transport.requests.size() == 1) {
		TH_CHECK(transport.requests[0].back().content == "任务A");
	}
}

TH_TEST(agentrepl_clear_prints_confirmation_and_history_resets) {
	ScriptTransport transport;
	transport.script.push_back(answer("第一次"));
	transport.script.push_back(answer("第二次"));
	std::istringstream in("任务A\n/clear\n任务B\n/exit\n");
	std::ostringstream out, log;

	MAKE_ROBOT();
	AgentToolRegistry tools;
	AgentSession session(transport, tools, sessionOpts());
	TH_CHECK(runAgentRepl(session, replCfg("scripts"), robot, in, out, log) == 0);
	TH_CHECK(out.str().find("已清空对话记忆") != std::string::npos);
	TH_CHECK(transport.requests.size() == 2);
	if (transport.requests.size() == 2) {
		/* /clear 之后的请求只含 system + 新任务：旧任务对话不再可见（C1 语义经 REPL 面） */
		TH_CHECK(transport.requests[1].size() == 2);
		TH_CHECK(transport.requests[1][1].content == "任务B");
	}
}

/* ============ SAN-10：/status 是只读会话一览，且绝不触发任务 ============
 * 为什么这两条一起测：新命令最容易写错的地方是**没有在派发 runTurn 之前 continue**
 * ——那样 "/status" 会被当成任务描述发给模型（用户想看一眼状态，结果启动了
 * 一次模型调用，可能还带工具动作）。判据是 fake transport **一次都没被调用**。 */
TH_TEST(agentrepl_status_is_read_only_and_never_dispatches) {
	ScriptTransport tr;
	tr.script.push_back(answer("不该被调用"));

	AgentToolRegistry tools;	/* /status 不需要任何工具 */
	AgentOptions opts;
	opts.maxTurns = 2;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	AgentSession session(tr, tools, opts);

	AgentModeConfig cfg;
	cfg.structurePath = "robots/relay-test.sql";
	cfg.scriptsDir = "scripts";
	cfg.model = "m0";
	cfg.endpoint = "https://gateway.example/v1/chat/completions";
	cfg.auditLogPath = "audit.jsonl";	/* 只用于渲染，不需要真打开 */

	MAKE_ROBOT();
	robot.active();		/* 硬件变量在 active() 时才注册（同本文件其它用例） */

	std::istringstream in("/status\n/exit\n");
	std::ostringstream out;
	std::ostringstream log;
	TH_CHECK(runAgentRepl(session, cfg, robot, in, out, log) == 0);

	/* ① 绝不触发模型调用（这是本用例的核心判据） */
	TH_CHECK_MSG(tr.requests.empty(),
		"/status 触发了模型调用（" + std::to_string(tr.requests.size()) + " 次）");
	/* ② 渲染出关键字段 */
	const std::string text = out.str();
	TH_CHECK_MSG(text.find("会话状态") != std::string::npos, text);
	TH_CHECK_MSG(text.find("m0") != std::string::npos, "未显示模型名");
	TH_CHECK_MSG(text.find("gateway.example") != std::string::npos, "未显示接入点");
	TH_CHECK_MSG(text.find("未触发裁剪") != std::string::npos, "未显示裁剪状态");
	TH_CHECK_MSG(text.find("audit.jsonl") != std::string::npos, "未显示审计落点");
	TH_CHECK_MSG(text.find("模型 0 轮") != std::string::npos, "空会话统计应显示 0 轮");
	/* ③ /help 里也要有它（否则用户不知道这个命令存在） */
	TH_CHECK_MSG(text.find("/status") != std::string::npos, "/help 未列出 /status");

	robot.deactivate();
}

/* SAN-10 的边界：预算为 0（= 不限）时显示"不限"，不显示假的 "0 / 0"；
 * 审计未开启时显示"(未开启)"，与摘要里的 "(未配置…)" 同口径。 */
TH_TEST(agentrepl_status_shows_unlimited_and_disabled_audit_honestly) {
	ScriptTransport tr;
	AgentToolRegistry tools;
	AgentOptions opts;
	opts.maxTurns = 1;
	opts.verbose = false;
	opts.systemPrompt = "sys";
	AgentSession session(tr, tools, opts);

	AgentModeConfig cfg;
	cfg.structurePath = "robots/relay-test.sql";
	cfg.scriptsDir = "scripts";
	cfg.maxHistoryBytes = 0;		/* 不限 */
	cfg.maxTaskToolResultBytes = 0;		/* 不限 */
	cfg.maxTaskImageBytes = 0;		/* 不限 */
	/* auditLogPath 留空 = 未开启 */

	MAKE_ROBOT();
	robot.active();

	std::istringstream in("/status\n/exit\n");
	std::ostringstream out;
	std::ostringstream log;
	TH_CHECK(runAgentRepl(session, cfg, robot, in, out, log) == 0);

	const std::string text = out.str();
	TH_CHECK_MSG(text.find("不限") != std::string::npos, "预算 0 应显示「不限」: " + text);
	TH_CHECK_MSG(text.find("(未开启)") != std::string::npos, "审计未开应显示 (未开启): " + text);
	/* 不得出现 "0 / 0" 这种看起来像"额度已满"的假数字 */
	TH_CHECK_MSG(text.find("0 字节 / 0 字节") == std::string::npos,
		"预算不限时不应显示 0 / 0: " + text);

	robot.deactivate();
}
