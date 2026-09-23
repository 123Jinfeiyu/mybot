/* test_sessionfile.cpp —— 会话持久化（批次 1，2026-09-23）的离线单测
 *
 * 覆盖面：
 *   - 会话文件读写原语：编解码往返（含 tool_calls/extras）、图片写入期排除、
 *     损坏行跳过并点名、会话头必要性/版本判据、指纹差异点名、权限告警、
 *     /clear 分界标记的载入语义；
 *   - AgentSession 的 complete 边界落盘：成功任务块落盘、失败回滚不落盘、
 *     trim 后水位线同步、/clear 分界；
 *   - runAgentMode 的全链路恢复：新鲜会话落盘 → 新对象恢复 → 模型请求里
 *     看得见先前对话（与 E2E 场景 19 同判据的两份实现之一）；
 *   - buildAgentConfig 的恢复组合校验（只认命令行路径、文件必须存在）。
 * 全部离线：传输用脚本化假件，机器人用 relay-test（内存装配），不联网。
 */

#include "harness.hpp"
#include "fixtures.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agenthistory.hpp"
#include "agentloop.hpp"
#include "agentmode.hpp"
#include "agenttools.hpp"
#include "cliargs.hpp"
#include "jsonparse.hpp"
#include "llmconfig.hpp"
#include "robot.hpp"

namespace fs = std::filesystem;

using namespace bot;
using namespace bot::agent;

namespace {

/* 脚本化传输：按预设序列返回响应，并记录每次请求的消息快照
 * （与 test_agentloop/test_agentrepl 同构；独立一份，不跨文件共享夹具） */
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

ChatResponse answer(const std::string& text) {
	ChatResponse r;
	r.content = text;
	return r;
}

ChatResponse toolCallResp(const std::string& id, const std::string& name) {
	ChatResponse r;
	r.toolCalls.push_back(ToolCall{id, name, "{}"});
	return r;
}

std::string readSmallFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("测试夹具读不到: " + path); }
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

std::string slurp(const std::string& path) {
	return readSmallFile(path);
}

std::size_t lineCount(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	std::size_t n = 0;
	std::string line;
	while (std::getline(f, line)) { if (!line.empty()) { ++n; } }
	return n;
}

/* 简易对话序列：任务 → assistant(tool_calls) → tool → 图片消息 → 最终回答 */
std::vector<ChatMessage> sampleConversation() {
	std::vector<ChatMessage> msgs;
	{
		ChatMessage u;
		u.role = "user";
		u.content = "任务一：拍照并站立";
		msgs.push_back(std::move(u));
	}
	{
		ChatMessage a;
		a.role = "assistant";
		a.content = "";
		a.toolCalls.push_back(ToolCall{"call_0_0", "capture_image", "{}"});
		a.extras = JsonObject();
		a.extras.put("reasoning_content", JsonString("思考串"));
		msgs.push_back(std::move(a));
	}
	{
		ChatMessage t;
		t.role = "tool";
		t.toolCallId = "call_0_0";
		t.content = "已拍摄照片（64 字节）";
		msgs.push_back(std::move(t));
	}
	{
		ChatMessage img;
		img.role = "user";
		img.content = "以上工具返回的图片如下，请结合画面内容继续。";
		img.images.push_back("data:image/png;base64," + std::string(96, 'A'));
		msgs.push_back(std::move(img));
	}
	{
		ChatMessage a;
		a.role = "assistant";
		a.content = "已站立。";
		msgs.push_back(std::move(a));
	}
	return msgs;
}

/* 打开一个截断写的会话文件并写会话头（大多数用例的公共前置） */
std::shared_ptr<SessionFileWriter> freshWriter(const std::string& path) {
	auto w = std::make_shared<SessionFileWriter>();
	std::string err;
	if (!w->open(path, /*append=*/false, err)) { throw std::runtime_error(err); }
	w->writeHeader(SessionHead{});
	return w;
}

} // namespace

/* ---- 编解码往返：roles/content/tool_calls/extras 全部保真 ---- */
TH_TEST(sessionfile_roundtrip_messages) {
	fx::TempDir tmp("sessrt");
	const std::string path = (tmp.path() / "s.jsonl").string();

	{
		SessionHead head;
		head.sessionId = "ab12cd34";
		head.robot = "relay-test";
		head.toolsFp = "toolfp";
		head.systemFp = "sysfp";
		auto w = std::make_shared<SessionFileWriter>();
		std::string err;
		TH_CHECK(w->open(path, /*append=*/false, err));
		w->writeHeader(head);
		for (const ChatMessage& m : sampleConversation()) { TH_CHECK(w->appendMessage(m)); }
	}
	{	/* 追加路径：新进程/新写入器在既有内容后继续 */
		auto w = std::make_shared<SessionFileWriter>();
		std::string err;
		TH_CHECK(w->open(path, /*append=*/true, err));
		ChatMessage u;
		u.role = "user";
		u.content = "任务二";
		TH_CHECK(w->appendMessage(u));
	}

	LoadedSession loaded;
	std::vector<std::string> warns;
	std::string err;
	TH_CHECK(loadSessionFile(path, loaded, warns, err));
	TH_CHECK(warns.empty());
	TH_CHECK(loaded.head.robot == "relay-test");
	TH_CHECK(loaded.head.sessionId == "ab12cd34");
	TH_CHECK(loaded.messages.size() == 6);

	TH_CHECK(loaded.messages[0].role == "user");
	TH_CHECK(loaded.messages[0].content == "任务一：拍照并站立");
	TH_CHECK(loaded.messages[1].role == "assistant");
	TH_CHECK(loaded.messages[1].toolCalls.size() == 1);
	TH_CHECK(loaded.messages[1].toolCalls[0].id == "call_0_0");
	TH_CHECK(loaded.messages[1].toolCalls[0].name == "capture_image");
	TH_CHECK(loaded.messages[1].extras.isObj());
	/* 图片消息：字节不落盘，落的是占位文本（下一条用例钉死磁盘内容） */
	TH_CHECK(loaded.messages[3].role == "user");
	TH_CHECK(loaded.messages[3].images.empty());
	TH_CHECK(loaded.messages[3].content.find("未随会话恢复") != std::string::npos);
	TH_CHECK(loaded.messages[4].content == "已站立。");
	TH_CHECK(loaded.messages[5].content == "任务二");

	/* 载入序列必须过协议自检（tool_call_id 无孤儿/无重复/无漏答） */
	TH_CHECK(historyProtocolViolation(loaded.messages).empty());
}

/* ---- 图片写入期排除：磁盘上没有任何 base64/data: 字节 ---- */
TH_TEST(sessionfile_image_bytes_never_on_disk) {
	fx::TempDir tmp("sessimg");
	const std::string path = (tmp.path() / "s.jsonl").string();

	auto w = freshWriter(path);
	for (const ChatMessage& m : sampleConversation()) { TH_CHECK(w->appendMessage(m)); }

	const std::string raw = slurp(path);
	TH_CHECK(raw.find("data:") == std::string::npos);
	TH_CHECK(raw.find("base64") == std::string::npos);
	/* 占位文本带总字节数（96 画面字节 + 22 字节 data URL 前缀） */
	const std::string want = sessionImagePlaceholder(96 + 22);
	TH_CHECK(raw.find(want) != std::string::npos);
}

/* ---- 损坏行跳过并点名（崩溃残行不该抹掉完整历史） ---- */
TH_TEST(sessionfile_corrupt_lines_skipped_and_named) {
	fx::TempDir tmp("sessbad");
	const std::string path = (tmp.path() / "s.jsonl").string();
	{
		std::ofstream f(path, std::ios::binary);
		f << "{\"v\":1,\"type\":\"session_head\",\"robot\":\"r\",\"tools_fp\":\"t\","
		   "\"system_fp\":\"s\"}\n";
		f << "{\"role\":\"user\",\"content\":\"好的那一行\"}\n";
		f << "{\"role\":\"user\",\"content\":\"残行\" <<CRASH\n";	/* 非法 JSON，第 3 行 */
		f << "[1,2,3]\n";						/* 不是对象，第 4 行 */
		f << "{\"role\":\"oracle\",\"content\":\"?\"}\n";		/* 未知 role，第 5 行 */
		f << "{\"role\":\"tool\",\"content\":\"缺 tool_call_id\"}\n";	/* tool 缺 id，第 6 行 */
		f << "{\"role\":\"user\",\"content\":\"好的第二行\"}\n";
	}

	LoadedSession loaded;
	std::vector<std::string> warns;
	std::string err;
	TH_CHECK(loadSessionFile(path, loaded, warns, err));
	TH_CHECK(loaded.messages.size() == 2);
	TH_CHECK(loaded.messages[0].content == "好的那一行");
	TH_CHECK(loaded.messages[1].content == "好的第二行");
	TH_CHECK(warns.size() == 4);
	for (const std::string& w : warns) {
		TH_CHECK(w.find("已跳过该行") != std::string::npos);
	}
	/* 点名行号 */
	TH_CHECK(warns[0].find("第 3 行") != std::string::npos);
	TH_CHECK(warns[1].find("第 4 行") != std::string::npos);
	TH_CHECK(warns[2].find("第 5 行") != std::string::npos);
	TH_CHECK(warns[3].find("第 6 行") != std::string::npos);
}

/* ---- 会话头：缺失/版本不认/空文件 → 整份拒绝 ---- */
TH_TEST(sessionfile_head_required_and_version_checked) {
	fx::TempDir tmp("sesshead");
	{
		const std::string p1 = (tmp.path() / "nohead.jsonl").string();
		{
			/* 写流必须先析构（关闭/落盘）再读——同一作用域里读会看到空文件 */
			std::ofstream f(p1, std::ios::binary);
			f << "{\"role\":\"user\",\"content\":\"x\"}\n";
		}
		LoadedSession out;
		std::vector<std::string> warns;
		std::string err;
		TH_CHECK(!loadSessionFile(p1, out, warns, err));
		TH_CHECK(err.find("会话头") != std::string::npos);
	}
	{
		const std::string p2 = (tmp.path() / "v9.jsonl").string();
		{
			std::ofstream f(p2, std::ios::binary);
			f << "{\"v\":9,\"type\":\"session_head\",\"robot\":\"r\",\"tools_fp\":\"t\","
			   "\"system_fp\":\"s\"}\n";
		}
		LoadedSession out;
		std::vector<std::string> warns;
		std::string err;
		TH_CHECK(!loadSessionFile(p2, out, warns, err));
		TH_CHECK(err.find("版本") != std::string::npos);
	}
	{
		const std::string p3 = (tmp.path() / "empty.jsonl").string();
		{ std::ofstream f(p3, std::ios::binary); }
		LoadedSession out;
		std::vector<std::string> warns;
		std::string err;
		TH_CHECK(!loadSessionFile(p3, out, warns, err));
		TH_CHECK(err.find("空或缺会话头") != std::string::npos);
	}
}

/* ---- 指纹差异点名：robot / 工具集 / 系统提示词各自报清差异 ---- */
TH_TEST(sessionfile_head_mismatch_names_field) {
	SessionHead now;
	now.robot = "relay-test";
	now.toolsFp = "fp_tools_now";
	now.systemFp = "fp_sys_now";

	{
		SessionHead old = now;
		old.robot = "spider1";
		const std::string mm = sessionHeadMismatch(now, old);
		TH_CHECK(!mm.empty());
		TH_CHECK(mm.find("结构脚本") != std::string::npos);
	}
	{
		SessionHead old = now;
		old.toolsFp = "fp_tools_old";
		const std::string mm = sessionHeadMismatch(now, old);
		TH_CHECK(!mm.empty());
		TH_CHECK(mm.find("工具集") != std::string::npos);
	}
	{
		SessionHead old = now;
		old.systemFp = "fp_sys_old";
		const std::string mm = sessionHeadMismatch(now, old);
		TH_CHECK(!mm.empty());
		TH_CHECK(mm.find("系统提示词") != std::string::npos);
	}
	/* sessionId 不参与比对（恢复就是要沿用它的） */
	{
		SessionHead old = now;
		old.sessionId = "different";
		TH_CHECK(sessionHeadMismatch(now, old).empty());
	}
}

/* ---- 权限告警：宽于 600 要说话，600 不多话 ---- */
TH_TEST(sessionfile_perms_warning) {
	fx::TempDir tmp("perms");
	const std::string path = (tmp.path() / "s.jsonl").string();
	{
		std::ofstream f(path, std::ios::binary);
		f << "x\n";
	}
	fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write
		| fs::perms::group_read | fs::perms::others_read);	/* 0644 */
	const std::string warn = sessionPermsWarning(path);
	TH_CHECK(!warn.empty());
	TH_CHECK(warn.find("chmod 600") != std::string::npos);

	fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);	/* 0600 */
	TH_CHECK(sessionPermsWarning(path).empty());
}

/* ---- /clear 分界标记：载入时丢弃标记前的全部消息 ---- */
TH_TEST(sessionfile_clear_marker_drops_prior_history) {
	fx::TempDir tmp("clearmark");
	const std::string path = (tmp.path() / "s.jsonl").string();

	auto w = freshWriter(path);
	std::string err;
	for (const ChatMessage& m : sampleConversation()) { TH_CHECK(w->appendMessage(m)); }
	TH_CHECK(w->writeClearMarker());
	{
		ChatMessage u;
		u.role = "user";
		u.content = "清空后的新任务";
		TH_CHECK(w->appendMessage(u));
	}

	LoadedSession loaded;
	std::vector<std::string> warns;
	TH_CHECK(loadSessionFile(path, loaded, warns, err));
	TH_CHECK(warns.empty());
	TH_CHECK(loaded.messages.size() == 1);
	TH_CHECK(loaded.messages[0].content == "清空后的新任务");
}

/* ---- complete 边界落盘：成功块落、失败块不落 ---- */
TH_TEST(sessionfile_persists_at_complete_boundary_only) {
	fx::TempDir tmp("boundary");
	const std::string path = (tmp.path() / "s.jsonl").string();

	auto writer = freshWriter(path);

	ScriptTransport transport;
	transport.script.push_back(toolCallResp("call_0_0", "nosuchtool"));
	transport.script.push_back(answer("第一件完成"));
	transport.script.push_back(answer("第二件完成"));

	AgentOptions o;
	o.maxTurns = 4;
	o.verbose = false;
	o.systemPrompt = "测试系统提示词";
	o.sessionWriter = writer;
	AgentToolRegistry tools;
	AgentSession session(transport, tools, o);

	/* 任务一：一轮工具（未知工具也照常写 tool 消息）+ 最终回答 */
	TH_CHECK(session.runTurn("任务一") == "第一件完成");
	/* 落盘：头 + user + assistant(tool_calls) + tool + assistant = 5 行 */
	TH_CHECK(lineCount(path) == 5);

	/* 任务二成功 */
	TH_CHECK(session.runTurn("任务二") == "第二件完成");
	TH_CHECK(lineCount(path) == 7);

	/* 任务三失败，且失败发生在**完成一整轮工具之后**（先派发一条工具、再让
	 * 下一次模型调用炸掉）：这条路径下回滚边界 `complete` 已经越过那轮工具，
	 * 半截尾部就是空的——落盘与否只由「成功路径才落盘」这一条规则决定。
	 * 这正是反向验证要钉住的判据：把 persistNewMessages() 挪进 catch 分支，
	 * 文件会多出失败任务的 user 行，本用例必须转红。 */
	transport.script.push_back(toolCallResp("call_9_0", "nosuchtool"));
	bool threw = false;
	try { session.runTurn("任务三"); } catch (const std::exception&) { threw = true; }
	TH_CHECK(threw);
	TH_CHECK(lineCount(path) == 7);
	TH_CHECK(slurp(path).find("任务三") == std::string::npos);
	/* 失败任务**已完成**的那轮工具会随后续成功任务一起落盘——与内存里的历史
	 * 一致（项目口径：硬件不会倒回去，把这段抹掉就等于告诉模型什么都没发生）。
	 * 永不落盘的是**半截 tool_calls 序列**（缺 tool 回应的那些），它们已被
	 * 回滚边界裁掉。故任务四之后：7 + 任务三(3 条) + 任务四(2 条) = 12 行。 */
	transport.script.push_back(answer("第三件完成"));
	TH_CHECK(session.runTurn("任务四") == "第三件完成");
	TH_CHECK(lineCount(path) == 12);
	TH_CHECK(slurp(path).find("任务三") != std::string::npos);
	TH_CHECK(slurp(path).find("任务四") != std::string::npos);
	/* 落盘内容必须仍是服务端可接受的历史（本条与 E2E 的 python 判据同源） */
	{
		LoadedSession loaded;
		std::vector<std::string> warns;
		std::string lerr;
		TH_CHECK(loadSessionFile(path, loaded, warns, lerr));
		TH_CHECK(warns.empty());
		TH_CHECK(historyProtocolViolation(loaded.messages).empty());
	}
}

/* ---- trim 与落盘水位：老块被裁后，新块只追加一次、不重写不重复 ---- */
TH_TEST(sessionfile_trim_keeps_file_in_sync) {
	fx::TempDir tmp("trim");
	const std::string path = (tmp.path() / "s.jsonl").string();

	auto w = freshWriter(path);
	std::string err;

	const std::string big(600, 'x');
	ScriptTransport transport;
	transport.script.push_back(answer(big));
	transport.script.push_back(answer(big));
	transport.script.push_back(answer(big));

	AgentOptions o;
	o.maxTurns = 4;
	o.verbose = false;
	o.systemPrompt = "S";
	o.maxHistoryBytes = 200;	/* 一个大块就超：任务二开始前必裁任务一 */
	o.sessionWriter = w;
	AgentToolRegistry tools;
	AgentSession session(transport, tools, o);

	TH_CHECK(session.runTurn("任务一") == big);
	TH_CHECK(session.runTurn("任务二") == big);
	/* trim 无条件保留最新块：任务三开始时 [任务一, 任务二] 两块都在列，
	 * 预算 200 装不下任务一 → 裁掉它（内存侧），文件仍是 append-only 全量 */
	TH_CHECK(session.runTurn("任务三") == big);
	TH_CHECK(lineCount(path) == 7);
	TH_CHECK(slurp(path).find("任务一") != std::string::npos);
	TH_CHECK(slurp(path).find("任务三") != std::string::npos);
	TH_CHECK(session.stats().historyTrimmed);
}

/* ---- /clear 与落盘：分界标记落盘，恢复不再看见清空前的任务 ---- */
TH_TEST(sessionfile_clear_writes_marker) {
	fx::TempDir tmp("clearsess");
	const std::string path = (tmp.path() / "s.jsonl").string();

	auto w = freshWriter(path);
	std::string err;

	ScriptTransport transport;
	transport.script.push_back(answer("一"));
	transport.script.push_back(answer("二"));

	AgentOptions o;
	o.maxTurns = 4;
	o.verbose = false;
	o.systemPrompt = "S";
	o.sessionWriter = w;
	AgentToolRegistry tools;
	AgentSession session(transport, tools, o);

	TH_CHECK(session.runTurn("任务一") == "一");
	session.clearHistory();
	TH_CHECK(session.runTurn("任务二") == "二");

	LoadedSession loaded;
	std::vector<std::string> warns;
	TH_CHECK(loadSessionFile(path, loaded, warns, err));
	/* 恢复看见的是 clear 之后的记忆，不是清空前的 */
	TH_CHECK(loaded.messages.size() == 2);
	TH_CHECK(loaded.messages[0].content == "任务二");
	TH_CHECK(loaded.messages[1].content == "二");
}

/* ---- runAgentMode 全链路：新鲜会话落盘 → 新会话对象恢复 → 模型请求里
 *      看得见先前对话（与 E2E 场景 19 同判据；离线、注入传输） ---- */
TH_TEST(sessionfile_runagentmode_resume_feeds_prior_context) {
	fx::TempDir tmp("resume");
	const std::string sessPath = (tmp.path() / "s.jsonl").string();

	MemoryReader robotReader(readSmallFile("robots/relay-test.sql"));
	RobotStruct robot(robotReader);
	robot.active();

	AgentToolContext tctx;
	tctx.robot = &robot;
	tctx.scriptsDir = "scripts";
	tctx.structurePath = "robots/relay-test.sql";

	AgentModeConfig cfg;
	cfg.structurePath = "robots/relay-test.sql";
	cfg.scriptsDir = "scripts";
	cfg.llmVar = "llm0";
	cfg.verbose = false;
	cfg.sessionFile = sessPath;
	cfg.task = "第一次任务";

	{
		AgentToolRegistry tools = makeAgentTools(tctx);
		ScriptTransport transport;
		transport.script.push_back(answer("第一次回答"));
		std::istringstream in;
		std::ostringstream out, log;
		TH_CHECK(runAgentMode(cfg, robot, transport, in, out, log) == 0);
		TH_CHECK(fs::exists(sessPath));
		const std::string raw = slurp(sessPath);
		TH_CHECK(raw.find("session_head") != std::string::npos);
		TH_CHECK(raw.find("第一次任务") != std::string::npos);
		TH_CHECK(raw.find("第一次回答") != std::string::npos);
	}

	/* 恢复：全新对象（新传输/新工具表/新会话），--resume 语义 */
	cfg.task = "继续";
	cfg.resumeSession = true;
	AgentToolRegistry tools2 = makeAgentTools(tctx);
	ScriptTransport transport2;
	transport2.script.push_back(answer("第二次回答"));
	std::istringstream in2;
	std::ostringstream out2, log2;
	TH_CHECK(runAgentMode(cfg, robot, transport2, in2, out2, log2) == 0);
	TH_CHECK(transport2.requests.size() == 1);
	const std::vector<ChatMessage>& req = transport2.requests[0];
	TH_CHECK(req.size() == 4);	/* system + 任务 + 回答 + 继续任务 */
	TH_CHECK(req[0].role == "system");
	TH_CHECK(req[1].role == "user" && req[1].content == "第一次任务");
	TH_CHECK(req[2].role == "assistant" && req[2].content == "第一次回答");
	TH_CHECK(req[3].role == "user" && req[3].content == "继续");
}

/* ---- 恢复的组合校验（buildAgentConfig 第 0 步）----
 * 反向验证口径：去掉任一判据，对应用例必须转红（见 changelog 反向验证记录）。 */
TH_TEST(sessionfile_resume_requires_cli_sourced_path) {
	fx::TempDir tmp("resumecfg");
	const std::string sessPath = (tmp.path() / "s.jsonl").string();
	{ std::ofstream f(sessPath, std::ios::binary); f << "x\n"; }

	{
		/* --resume 不带 --session-file */
		CliOptions o;
		o.agent = true;
		o.robot = "robots/relay-test.sql";
		o.resume = true;
		AgentModeConfig out;
		std::string err;
		TH_CHECK(!buildAgentConfig(o, out, err));
		TH_CHECK(err.find("命令行") != std::string::npos);
	}
	{
		/* --resume + 不存在的路径 */
		CliOptions o;
		o.agent = true;
		o.robot = "robots/relay-test.sql";
		o.resume = true;
		o.sessionFile = (tmp.path() / "nope.jsonl").string();
		AgentModeConfig out;
		std::string err;
		TH_CHECK(!buildAgentConfig(o, out, err));
		TH_CHECK(err.find("不存在") != std::string::npos);
	}
	{
		/* 合法组合：第 0 步放行（注入密钥让后续阶段也能走通） */
		fx::EnvVar key("BOT4CJ_LLM_KEY", "sk-test-123");
		CliOptions o;
		o.agent = true;
		o.robot = "robots/relay-test.sql";
		o.resume = true;
		o.sessionFile = sessPath;
		o.llmConfig = "none";
		AgentModeConfig out;
		std::string err;
		TH_CHECK(buildAgentConfig(o, out, err));
		TH_CHECK(out.sessionFile == sessPath);
		TH_CHECK(out.resumeSession);
		TH_CHECK(out.sessionFileFromCli);
	}
}

/* ---- 配置文件 session_file 字段：可给（只用于写入）、未知字段仍拒绝 ---- */
TH_TEST(sessionfile_llmconfig_field) {
	fx::TempDir tmp("cfgfield");
	{
		const std::string p = (tmp.path() / "ok.json").string();
		{ std::ofstream f(p, std::ios::binary); f << "{\"session_file\":\"/tmp/x.jsonl\"}"; }
		LlmFileConfig out;
		std::string err;
		TH_CHECK(loadLlmConfigFile(p, out, err));
		TH_CHECK(out.sessionFile.has_value());
		TH_CHECK(*out.sessionFile == "/tmp/x.jsonl");
	}
	{
		const std::string p = (tmp.path() / "typo.json").string();
		{ std::ofstream f(p, std::ios::binary); f << "{\"session_fil\":\"x\"}"; }
		LlmFileConfig out;
		std::string err;
		TH_CHECK(!loadLlmConfigFile(p, out, err));
		TH_CHECK(err.find("未知字段") != std::string::npos);
	}
}
