/* test_agentaudit.cpp —— 审计轨迹（--audit-log）的直接锚点
 *
 * 覆盖面：
 *   1) maskSecretsInText：敏感 KEY=VALUE 遮蔽 / 非敏感名不误伤 / 普通命令原样
 *   2) extractExitCode：run_command 回执前缀解析与非法形态拒绝
 *   3) fingerprint64：确定性 + 输入不同指纹不同
 *   4) AuditLog：JSON Lines 落盘形态（ts/session/tool/ok/ms/bytes 齐备）、
 *      图片只记体积与指纹（base64 不落盘）、坏路径上电前失败
 *
 * 审计是「事后复盘」的证据链，这里的断言就是它的最低可信判据：
 * 行必须是合法 JSON、字段齐备、绝不含图片数据本体。
 */

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "agentaudit.hpp"
#include "fixtures.hpp"
#include "harness.hpp"
#include "jsonparse.hpp"

namespace {

std::string slurp(const std::string& path) {
	std::ifstream f(path);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

/* ---- maskSecretsInText ---- */

TH_TEST(agentaudit_mask_shadows_sensitive_assignment) {
	const std::string out = bot::agent::maskSecretsInText(
		"BOT4CJ_LLM_KEY=sk-abc123 curl -s http://x");
	TH_CHECK(out.find("BOT4CJ_LLM_KEY=***") != std::string::npos);
	TH_CHECK(out.find("sk-abc123") == std::string::npos);
	/* 连字符复合名（api-key）按整词切词同样命中 */
	const std::string out2 = bot::agent::maskSecretsInText("api-key=verysecret run");
	TH_CHECK(out2.find("api-key=***") != std::string::npos);
	TH_CHECK(out2.find("verysecret") == std::string::npos);
	/* Authorization 头形态（遮蔽专用词：不进环境清洗名单，只对文本有意义） */
	const std::string out3 = bot::agent::maskSecretsInText(
		"curl -H \"Authorization: Bearer sk-tok\" http://x");
	TH_CHECK(out3.find("sk-tok") == std::string::npos);
}

TH_TEST(agentaudit_mask_spares_unrelated_names_and_plain_text) {
	/* KEYSTONE/MONKEY 不含敏感整词，不得误伤（与环境清洗同一判据） */
	TH_CHECK(bot::agent::maskSecretsInText("KEYSTONE=abc MONKEY=x echo hi")
		== "KEYSTONE=abc MONKEY=x echo hi");
	/* 值边界：引号与空白结束值，之后的文本保持原样 */
	const std::string out = bot::agent::maskSecretsInText("token=\"abc def\" tail");
	TH_CHECK(out.find("token=***") != std::string::npos);
	TH_CHECK(out.find(" tail") != std::string::npos);
	/* 没有赋值形态的普通命令逐字保留 */
	const std::string plain = "systemctl status mybot --no-pager";
	TH_CHECK(bot::agent::maskSecretsInText(plain) == plain);
	/* 标识符后不是 '='（如命令 flag -H）不算「名字=值」 */
	TH_CHECK(bot::agent::maskSecretsInText("curl -H x") == "curl -H x");
}

/* ---- extractExitCode ---- */

TH_TEST(agentaudit_extract_exit_code_parses_run_receipt_prefix) {
	TH_CHECK(bot::agent::extractExitCode("exit_code=0\nstdout:\nx") == std::optional<long long>(0));
	TH_CHECK(bot::agent::extractExitCode("exit_code=127\nstdout:\n(空)")
		== std::optional<long long>(127));
	TH_CHECK(bot::agent::extractExitCode("exit_code=-1\nstdout:\n(空)")
		== std::optional<long long>(-1));
	/* 非 run_command 回执形态（其他工具的成功文案/任意文本）一律拒绝 */
	TH_CHECK(!bot::agent::extractExitCode("已执行动作脚本 std.mt（126 字节）").has_value());
	TH_CHECK(!bot::agent::extractExitCode("").has_value());
	TH_CHECK(!bot::agent::extractExitCode("exit_code=abc").has_value());
	/* 越界值（>255）不是合法退出码，拒绝 */
	TH_CHECK(!bot::agent::extractExitCode("exit_code=999999").has_value());
}

/* ---- fingerprint64 ---- */

TH_TEST(agentaudit_fingerprint_is_deterministic_and_discriminating) {
	const std::string a = bot::agent::fingerprint64("data:image/jpeg;base64,AAAA");
	const std::string b = bot::agent::fingerprint64("data:image/jpeg;base64,AAAA");
	const std::string c = bot::agent::fingerprint64("data:image/jpeg;base64,AAAB");
	TH_CHECK(a == b);			/* 同输入同指纹（对账靠它） */
	TH_CHECK(a != c);			/* 不同输入不同指纹 */
	TH_CHECK(a.size() == 16);		/* 64 bit → 16 hex 字符 */
}

/* ---- AuditLog ---- */

TH_TEST(agentaudit_writes_jsonl_with_core_fields_and_no_image_payload) {
	fx::TempDir tmp("audit");
	const std::string path = (tmp.path() / "audit.jsonl").string();
	bot::agent::AuditLog log;
	std::string err;
	TH_CHECK_MSG(log.open(path, err), err);

	bot::agent::AuditEntry e;
	e.turn = 2;
	e.tool = "capture_image";
	e.ok = true;
	e.ms = 43;
	e.bytes = 64;
	e.imageBytes = 1024;
	e.imageFp = bot::agent::fingerprint64("data:image/jpeg;base64,/9j/xxxx");
	log.write(e);
	log.write(e);	/* 第二条：同会话 id 应一致 */

	const std::string text = slurp(path);
	std::istringstream ss(text);
	std::string line;
	int lines = 0;
	std::string firstSession;
	while (std::getline(ss, line)) {
		if (line.empty()) { continue; }
		++lines;
		/* 每行都必须是合法 JSON 且核心字段齐备——审计行不可解析即失去证据价值 */
		const bot::Json j = bot::parseJson(line);
		TH_CHECK_MSG(j.isObj(), "第 " + std::to_string(lines) + " 行不是 JSON 对象");
		TH_CHECK(j.get("ts") && j.get("ts")->isStr() && !j.get("ts")->getValue().empty());
		TH_CHECK(j.get("tool") && j.get("tool")->getValue() == "capture_image");
		TH_CHECK(j.get("turn") && j.get("turn")->getInt() == 2);
		TH_CHECK(j.get("ok") && j.get("ok")->getBool());
		TH_CHECK(j.get("ms") && j.get("ms")->getInt() == 43);
		TH_CHECK(j.get("bytes") && j.get("bytes")->getInt() == 64);
		TH_CHECK(j.get("image_bytes") && j.get("image_bytes")->getInt() == 1024);
		TH_CHECK(j.get("image_fp") && j.get("image_fp")->isStr());
		if (firstSession.empty()) { firstSession = j.get("session")->getValue(); }
		else { TH_CHECK(j.get("session")->getValue() == firstSession); }
		/* 图片数据本体绝不落盘：这是审计模块的隐私与体积红线 */
		TH_CHECK(line.find("base64") == std::string::npos);
	}
	TH_CHECK(lines == 2);
}

TH_TEST(agentaudit_logs_failures_and_masks_run_command_args) {
	fx::TempDir tmp("auditfail");
	const std::string path = (tmp.path() / "audit.jsonl").string();
	bot::agent::AuditLog log;
	std::string err;
	TH_CHECK_MSG(log.open(path, err), err);

	bot::Json args = bot::JsonObject();
	/* 命令原文的遮蔽发生在派发点（agentloop::dispatchOneCall），记账层只负责
	 * 如实落盘——这里按派发点产出的形态给参（遮蔽本身由上面的用例钉住）。 */
	args.put("command", bot::JsonString(
		bot::agent::maskSecretsInText("BOT4CJ_LLM_KEY=sk-real curl http://x")));
	args.put("timeout_seconds", bot::JsonInt(10));
	bot::agent::AuditEntry e;
	e.turn = 0;
	e.tool = "run_command";
	e.args = args;
	e.ok = false;
	e.error = "命令执行失败: fork 失败";
	e.ms = 5;
	e.bytes = 40;
	e.exitCode = 1;
	log.write(e);

	const bot::Json j = bot::parseJson(slurp(path));
	TH_CHECK(j.isObj());
	/* 失败也要记（ok=false + error），且命令里的敏感值已被遮蔽 */
	TH_CHECK(j.get("ok") && !j.get("ok")->getBool());
	TH_CHECK(j.get("error") && j.get("error")->getValue().find("fork") != std::string::npos);
	TH_CHECK(j.get("exit_code") && j.get("exit_code")->getInt() == 1);
	const auto cmd = j.get("args");
	TH_CHECK(cmd && cmd->isObj() && cmd->get("command"));
	TH_CHECK(cmd->get("command")->getValue().find("BOT4CJ_LLM_KEY=***") != std::string::npos);
	TH_CHECK(cmd->get("command")->getValue().find("sk-real") == std::string::npos);
	/* 非敏感参数原样保留 */
	TH_CHECK(cmd->get("timeout_seconds") && cmd->get("timeout_seconds")->getInt() == 10);
}

TH_TEST(agentaudit_open_bad_path_fails_before_power_on) {
	bot::agent::AuditLog log;
	std::string err;
	TH_CHECK(!log.open("/nonexistent-dir-for-audit/audit.jsonl", err));
	TH_CHECK(!err.empty());		/* 人话报错（上电前失败，配置错误不碰真机） */
	TH_CHECK(!log.enabled());	/* 打开失败不得进入半开状态 */
}

TH_TEST(agentaudit_stderr_sink_is_enabled_and_session_is_stable) {
	bot::agent::AuditLog log;
	std::string err;
	TH_CHECK_MSG(log.open("-", err), err);
	TH_CHECK(log.enabled());
	TH_CHECK(log.sessionId().size() == 8);
	/* 双开拒绝：一个会话一个落点，静默换文件会让证据链断成两截 */
	TH_CHECK(!log.open("/tmp/another-audit.jsonl", err));
	TH_CHECK(!err.empty());
}

/* ============ SAN-9：会话边界与模型轮次记录 ============
 * 缺口（读码 + 实测）：审计此前**只记工具调用**，于是"模型说了什么、花了多久、
 * 这一轮有没有真的发出请求"完全不可查；一次在模型调用阶段失败的会话在日志里
 * **一条记录都没有**——而那恰恰是最需要复盘的时候。
 * 判据（三条一起）：① 纯文本回答的任务（不调任何工具）也必须留下 model 记录；
 * ② 会话头尾各一条 session 记录；③ 既有工具行的字段**一个不少**（加法演进）。 */
TH_TEST(agentaudit_model_and_session_records_are_additive) {
	using namespace bot;
	using namespace bot::agent;
	fx::TempDir tmp("audit-session");
	const std::string path = (tmp.path() / "audit.jsonl").string();

	AuditLog log;
	std::string err;
	TH_CHECK(log.open(path, err));

	/* ① 会话头（start）——由外壳写，这里直接调底层 write 验证形态 */
	{
		AuditEntry e;
		e.kind = "session";
		e.status = "start";
		e.driver = "LlmOpenAI";
		e.model = "m0";
		/* 接入点带 userinfo + query：**必须**在落盘前被剥掉 */
		e.endpoint = "https://user:tok@gateway.example/v1/chat/completions?key=SECRET";
		e.apiKeySource = "env";
		log.write(e);
	}
	/* ② 模型轮次（含 provider 给的用量） */
	{
		AuditEntry e;
		e.kind = "model";
		e.turn = 0;
		e.ok = true;
		e.status = "ok";
		e.ms = 42;
		e.reqBytes = 1024;
		e.respBytes = 128;
		e.attempts = 2;		/* 发生过重试 */
		e.promptTokens = 1234;
		e.completionTokens = 56;
		log.write(e);
	}
	/* ③ 既有工具行（字段一个不少） */
	{
		AuditEntry e;
		e.kind = "tool";
		e.turn = 0;
		e.tool = "list_hardware";
		e.ok = true;
		e.ms = 3;
		e.bytes = 64;
		log.write(e);
	}
	/* ④ 会话尾（error） */
	{
		AuditEntry e;
		e.kind = "session";
		e.status = "error";
		e.error = "请求已取消（宿主收到停止信号）";
		log.write(e);
	}

	const std::string raw = slurp(path);
	/* 密钥与凭据绝不落盘（顺带钉住 endpoint 的剥 query/userinfo 行为） */
	TH_CHECK_MSG(raw.find("SECRET") == std::string::npos, "query 进了审计日志");
	TH_CHECK_MSG(raw.find("user:tok") == std::string::npos, "URL userinfo 进了审计日志");
	TH_CHECK_MSG(raw.find("gateway.example") != std::string::npos,
		"接入点的 host 应当保留（否则复盘时看不出连的哪）");

	std::istringstream ss(raw);
	std::string line;
	int nSession = 0, nModel = 0, nTool = 0;
	while (std::getline(ss, line)) {
		if (line.empty()) { continue; }
		Json d = parseJson(line);
		const Json* kind = d.find("kind");
		TH_CHECK(kind != nullptr && kind->isStr());	/* 每行都有 kind */
		const std::string k = kind->getValue();
		if (k == "session") {
			++nSession;
			TH_CHECK(d.find("status") != nullptr);
			/* 会话记录**不带**工具字段（避免 fake 语义：tool 为空串） */
			TH_CHECK(d.find("tool") == nullptr);
			/* start 记录带驱动/模型/接入点/密钥来源；end 记录只带 status
			 * （收尾不需要重复配置，重复反而让日志变长） */
			const Json* st = d.find("status");
			if (st != nullptr && st->isStr() && st->getValue() == "start") {
				TH_CHECK(d.find("driver") != nullptr);
				TH_CHECK(d.find("endpoint") != nullptr);
				TH_CHECK(d.find("api_key_source") != nullptr);
			}
		} else if (k == "model") {
			++nModel;
			TH_CHECK(d.find("ms") != nullptr);
			TH_CHECK(d.find("attempts") != nullptr);
			TH_CHECK(d.find("prompt_tokens") != nullptr);
			TH_CHECK(d.find("completion_tokens") != nullptr);
		} else if (k == "tool") {
			++nTool;
			/* 既有工具行字段一个不少 */
			TH_CHECK(d.find("turn") != nullptr);
			TH_CHECK(d.find("tool") != nullptr);
			TH_CHECK(d.find("ok") != nullptr);
			TH_CHECK(d.find("ms") != nullptr);
			TH_CHECK(d.find("bytes") != nullptr);
		} else {
			TH_CHECK_MSG(false, "未知 kind: " + k);
		}
	}
	TH_CHECK(nSession == 2);
	TH_CHECK(nModel == 1);
	TH_CHECK(nTool == 1);
}

/* SAN-9 兼容性钉子：老消费方按工具字段过滤，结果必须与"没有新记录类型"时一致。
 * 判据是**过滤器语义不变**：`tool == "x"` 只会命中工具行，不会误命中 session/model
 * 记录（它们的 tool 字段根本不出现）。 */
TH_TEST(agentaudit_legacy_tool_filter_still_selects_only_tool_rows) {
	using namespace bot;
	using namespace bot::agent;
	fx::TempDir tmp("audit-compat");
	const std::string path = (tmp.path() / "audit.jsonl").string();
	AuditLog log;
	std::string err;
	TH_CHECK(log.open(path, err));

	AuditEntry s;
	s.kind = "session";
	s.status = "start";
	log.write(s);
	AuditEntry m;
	m.kind = "model";
	m.turn = 0;
	m.ok = true;
	log.write(m);
	AuditEntry t;
	t.kind = "tool";
	t.tool = "run_command";
	t.exitCode = 0;
	log.write(t);

	/* 模拟老消费方的过滤表达式语义：只看带 tool 字段且值匹配的行 */
	std::istringstream ss(slurp(path));
	std::string line;
	int matched = 0, parsed = 0;
	while (std::getline(ss, line)) {
		if (line.empty()) { continue; }
		Json d = parseJson(line);
		++parsed;
		const Json* tool = d.find("tool");
		if (tool != nullptr && tool->isStr() && tool->getValue() == "run_command") { ++matched; }
	}
	TH_CHECK(parsed == 3);
	TH_CHECK_MSG(matched == 1, "老过滤表达式只应命中那一条工具行，实际 " + std::to_string(matched));
}
