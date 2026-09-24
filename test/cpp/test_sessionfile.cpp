/* test_sessionfile.cpp —— 会话持久化（批次 1，2026-09-23）的离线单测
 *
 * 覆盖面：
 *   - 会话文件读写原语：编解码往返（含 tool_calls/extras）、图片写入期排除、
 *     损坏行跳过并点名、会话头必要性/版本判据、指纹差异点名、权限告警、
 *     /clear 分界标记的载入语义；
 *   - buildAgentConfig 的恢复组合校验（只认命令行路径、文件必须存在）。
 * 全部离线：机器人用 relay-test（内存装配），不联网。
 *
 * 不在这里的：何时落盘、trim 后水位怎么同步、恢复后模型看得见什么——这些
 * 编排语义原先由 C++ AgentSession / runAgentMode 承担（连同 E2E 场景 19），
 * 随 d384a9b9 移除 REPL 一起消失，现由 Rust agent 侧承担（agent/src/history.rs）。
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
