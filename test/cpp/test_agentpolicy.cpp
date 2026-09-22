/* test_agentpolicy.cpp —— 策略钩子（--agent-policy）的直接锚点
 *
 * 覆盖面：
 *   1) AgentPolicy::load：合法文件 + 缺省值；未知字段/类型不符/区间越界/
 *      require_confirm 与 confirm_command 互为前提等严格性拒绝
 *   2) confirm 的三条路径：放行（退出 0）/ 拒绝（退出码非 0）/ 被信号终止
 *   3) 超时两条分支：on_timeout=deny（缺省）/ allow
 *   4) 未命中 require_confirm 的工具零开销放行（连确认程序都不该被调用）
 *   5) 交给确认程序的参数已遮蔽（maskToolArgs：run_command 的敏感 KEY=VALUE）
 *
 * 确认程序是「人在环」的落点：退出码即裁决，这里的用例把它钉成契约。
 */

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "agentpolicy.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

namespace {

/* 在临时目录里写一份策略文件并加载 */
std::shared_ptr<const bot::agent::AgentPolicy> loadFromTemp(const fx::TempDir& tmp,
		const std::string& name, const std::string& json, std::string& err) {
	const std::string path = (tmp.path() / name).string();
	std::ofstream f(path);
	f << json;
	f.close();
	return bot::agent::AgentPolicy::load(path, err);
}

} // namespace

/* ---- 1) load：合法文件与缺省值 ---- */

TH_TEST(agentpolicy_load_accepts_valid_and_defaults) {
	fx::TempDir tmp("policy");
	std::string err;
	auto p = loadFromTemp(tmp, "p.json",
		"{\"require_confirm\": [\"run_command\", \"list_hardware\"],"
		" \"confirm_command\": \"/bin/true\"}", err);
	TH_CHECK_MSG(p != nullptr, err);
	TH_CHECK(p->requiresConfirm("run_command"));
	TH_CHECK(p->requiresConfirm("list_hardware"));
	TH_CHECK(!p->requiresConfirm("capture_image"));
	/* 缺省：timeout 30s、on_timeout=deny */
	TH_CHECK(p->timeoutSec() == 30);
	TH_CHECK(p->timeoutDeny());
	TH_CHECK(p->confirmCommand() == "/bin/true");
	TH_CHECK(p->requireConfirm().size() == 2);
}

/* ---- 1) load：严格性拒绝（拼错不能静默失效，缺前提不能装死） ---- */

TH_TEST(agentpolicy_load_rejects_unknown_field_and_bad_types) {
	fx::TempDir tmp("policybad");
	std::string err;
	/* 未知字段 */
	TH_CHECK(loadFromTemp(tmp, "a.json", "{\"require_konfirm\": [\"run_command\"]}", err) == nullptr);
	TH_CHECK(err.find("未知字段") != std::string::npos);
	/* require_confirm 非空但缺 confirm_command */
	TH_CHECK(loadFromTemp(tmp, "b.json", "{\"require_confirm\": [\"run_command\"]}", err) == nullptr);
	TH_CHECK(err.find("confirm_command") != std::string::npos);
	/* 反过来：confirm_command 孤儿（没有工具会走到确认） */
	TH_CHECK(loadFromTemp(tmp, "c.json", "{\"confirm_command\": \"/bin/true\"}", err) == nullptr);
	TH_CHECK(err.find("require_confirm") != std::string::npos);
	/* on_timeout 只认 deny/allow */
	TH_CHECK(loadFromTemp(tmp, "d.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"/bin/true\","
		" \"on_timeout\": \"yes\"}", err) == nullptr);
	TH_CHECK(err.find("on_timeout") != std::string::npos);
	/* timeout 越界（0 与负数会让确认语义失真） */
	TH_CHECK(loadFromTemp(tmp, "e.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"/bin/true\","
		" \"confirm_timeout_sec\": 0}", err) == nullptr);
	TH_CHECK(err.find("confirm_timeout_sec") != std::string::npos);
	/* 类型不符：require_confirm 必须是字符串数组 */
	TH_CHECK(loadFromTemp(tmp, "f.json",
		"{\"require_confirm\": [\"run_command\", 3], \"confirm_command\": \"/bin/true\"}", err)
		== nullptr);
	TH_CHECK(err.find("require_confirm") != std::string::npos);
	/* 顶层不是对象 / 不是 JSON */
	TH_CHECK(loadFromTemp(tmp, "g.json", "[\"run_command\"]", err) == nullptr);
	TH_CHECK(loadFromTemp(tmp, "h.json", "{not json", err) == nullptr);
	/* 不存在的文件 */
	bot::agent::AgentPolicy::load((tmp.path() / "nope.json").string(), err);
	TH_CHECK(err.find("策略文件") != std::string::npos);
}

/* ---- 2) confirm：放行 / 拒绝 / 信号终止 ---- */

TH_TEST(agentpolicy_confirm_allow_deny_and_signal) {
	fx::TempDir tmp("policyrun");
	std::string err;
	/* 放行：退出 0 */
	auto allow = loadFromTemp(tmp, "allow.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"/bin/true\"}", err);
	TH_CHECK_MSG(allow != nullptr, err);
	const bot::Json args = bot::JsonObject();
	const bot::agent::PolicyDecision ok = allow->confirm("run_command", args);
	TH_CHECK_MSG(ok.allowed, ok.reason);
	TH_CHECK(ok.reason.empty());

	/* 拒绝：退出码非 0，人话里带退出码 */
	auto deny = loadFromTemp(tmp, "deny.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"/bin/sh -c 'exit 3'\"}", err);
	TH_CHECK_MSG(deny != nullptr, err);
	const bot::agent::PolicyDecision no = deny->confirm("run_command", args);
	TH_CHECK(!no.allowed);
	TH_CHECK(no.reason.find("退出码 3") != std::string::npos);

	/* 被信号终止：不算放行（SIGKILL 不可捕获/不可忽略，判据决定性）。
	 * 注意 confirm_command 本就被宿主以 /bin/sh -c 包装执行，这里写裸命令，
	 * $$ 即该 shell 自己——若再写一层 /bin/sh -c，杀到的会是内层 shell，
	 * 外层反而以退出码 137 正常收场（实测踩过）。 */
	auto sig = loadFromTemp(tmp, "sig.json",
		"{\"require_confirm\": [\"run_command\"],"
		" \"confirm_command\": \"kill -KILL $$\"}", err);
	TH_CHECK_MSG(sig != nullptr, err);
	const bot::agent::PolicyDecision s = sig->confirm("run_command", args);
	TH_CHECK(!s.allowed);
	TH_CHECK(s.reason.find("信号") != std::string::npos);

	/* exec 不存在的确认程序（sh 退出 127）：拒绝而不是崩溃/放行 */
	auto missing = loadFromTemp(tmp, "missing.json",
		"{\"require_confirm\": [\"run_command\"],"
		" \"confirm_command\": \"/nonexistent-approver-cmd\"}", err);
	TH_CHECK_MSG(missing != nullptr, err);
	const bot::agent::PolicyDecision m = missing->confirm("run_command", args);
	TH_CHECK(!m.allowed);
}

/* ---- 3) confirm：超时两分支 ---- */

TH_TEST(agentpolicy_confirm_timeout_deny_and_allow) {
	fx::TempDir tmp("policyto");
	std::string err;
	bot::Json args = bot::JsonObject();
	/* deny（缺省） */
	auto deny = loadFromTemp(tmp, "deny.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"sleep 5\","
		" \"confirm_timeout_sec\": 1}", err);
	TH_CHECK_MSG(deny != nullptr, err);
	const bot::agent::PolicyDecision d = deny->confirm("run_command", args);
	TH_CHECK(!d.allowed);
	TH_CHECK(d.reason.find("超时") != std::string::npos);
	TH_CHECK(d.reason.find("deny") != std::string::npos);
	/* allow */
	auto allow = loadFromTemp(tmp, "allow.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"sleep 5\","
		" \"confirm_timeout_sec\": 1, \"on_timeout\": \"allow\"}", err);
	TH_CHECK_MSG(allow != nullptr, err);
	const bot::agent::PolicyDecision a = allow->confirm("run_command", args);
	TH_CHECK_MSG(a.allowed, a.reason);
	TH_CHECK(a.reason.find("超时") != std::string::npos);
}

/* ---- 4) 未命中的工具零开销放行 ---- */

TH_TEST(agentpolicy_unlisted_tool_passes_without_running_command) {
	fx::TempDir tmp("policyfree");
	std::string err;
	/* 确认程序故意指向不存在的路径：若未命中也去调用，这里就会拒绝 */
	auto p = loadFromTemp(tmp, "p.json",
		"{\"require_confirm\": [\"run_command\"],"
		" \"confirm_command\": \"/nonexistent-approver-cmd\"}", err);
	TH_CHECK_MSG(p != nullptr, err);
	const bot::agent::PolicyDecision d = p->confirm("list_hardware", bot::JsonObject());
	TH_CHECK_MSG(d.allowed, d.reason);
	TH_CHECK(d.reason.empty());
}

/* ---- 5) 参数遮蔽 + maskToolArgs 单点 ---- */

TH_TEST(agentpolicy_payload_masks_secrets_before_external_program) {
	fx::TempDir tmp("policymask");
	std::string err;
	/* 确认程序只认「已遮蔽」的形态：stdin 里出现裸密钥退出 1，只见到
	 * API_KEY=*** 退出 0，其他形态退出 2。写成脚本文件，避免把引号/
	 * 反斜杠经 JSON 再经 sh 传两层的转义地狱。 */
	const std::string script = (tmp.path() / "approve-mask.sh").string();
	{
		std::ofstream f(script);
		f << "#!/bin/sh\n"
		     "input=$(cat)\n"
		     "case \"$input\" in\n"
		     "  *sk-real*) exit 1 ;;\n"
		     "  *'API_KEY=***'*) exit 0 ;;\n"
		     "  *) exit 2 ;;\n"
		     "esac\n";
	}
	::chmod(script.c_str(), 0755);
	auto p = loadFromTemp(tmp, "p.json",
		"{\"require_confirm\": [\"run_command\"], \"confirm_command\": \"" + script + "\"}", err);
	TH_CHECK_MSG(p != nullptr, err);
	bot::Json args = bot::JsonObject();
	args.put("command", bot::JsonString("API_KEY=sk-real curl -s http://x"));
	const bot::agent::PolicyDecision d = p->confirm("run_command", args);
	TH_CHECK_MSG(d.allowed, d.reason);
}

TH_TEST(agentpolicy_mask_tool_args_unit) {
	/* run_command：command 字段里的敏感 KEY=VALUE 被遮蔽 */
	bot::Json args = bot::JsonObject();
	args.put("command", bot::JsonString("API_KEY=sk-real echo hi"));
	args.put("timeout_seconds", bot::JsonInt(5));
	const bot::Json masked = bot::agent::maskToolArgs("run_command", args);
	const auto cmd = masked.get("command");
	TH_CHECK(cmd && cmd->isStr());
	TH_CHECK(cmd->getValue().find("API_KEY=***") != std::string::npos);
	TH_CHECK(cmd->getValue().find("sk-real") == std::string::npos);
	/* 其他字段原样保留 */
	TH_CHECK(masked.get("timeout_seconds") && masked.get("timeout_seconds")->getInt() == 5);
	/* 非 run_command / 无 command 字段：原样返回 */
	TH_CHECK(bot::agent::maskToolArgs("list_hardware", args).toString() == args.toString());
	bot::Json plain = bot::JsonObject();
	plain.put("command", bot::JsonString("echo hello"));
	TH_CHECK(bot::agent::maskToolArgs("run_command", plain).toString() == plain.toString());
}
