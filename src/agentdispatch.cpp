#include "agentdispatch.hpp"

#include <chrono>
#include <utility>

namespace bot {
namespace agent {

AgentToolResult DispatchGate::run(const ToolCall& call, int turn) {
	AgentToolResult result;
	Json parsedArgs;
	bool argsOk = false;
	const auto t0 = std::chrono::steady_clock::now();
	/* 拒绝的形态：被拒的调用没有副作用，回执说明「没执行、为什么」，
	 * 工具循环照常推进——模型知道后可以改道或提醒操作者。 */
	bool denied = false;
	try {
		parsedArgs = parseToolArguments(call.arguments);
		argsOk = true;
		if (policy_ && policy_->requiresConfirm(call.name)) {
			const PolicyDecision d = policy_->confirm(call.name, parsedArgs);
			if (!d.allowed) {
				denied = true;
				result = AgentToolResult{
					std::string("本次工具调用未执行：被策略拦截（--agent-policy，")
						+ d.reason + "）。请改用无需确认的工具，"
						  "或告知操作者调整策略后重试。",
					"", false, "policy_denied: " + d.reason};
			}
		}
		if (!denied) { result = tools_.execute(call.name, parsedArgs); }
	} catch (const std::exception& e) {
		result = AgentToolResult{std::string("工具参数错误: ") + e.what(), "", false, e.what()};
	}
	const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();

	/* ---- 审计轨迹：参数错误也记——「模型想做什么但没做成」同样是审计
	 * 信息（被策略拒绝的调用也走这条：ok=false + error=policy_denied）。
	 * 图片只记字节数与指纹，不落 base64；命令原文遮蔽敏感值（maskToolArgs
	 * 单点，与确认程序的输入同一份）。 ---- */
	if (audit_) {
		AuditEntry e;
		e.turn = turn;
		e.tool = call.name;
		if (argsOk && parsedArgs.isObj()) {
			e.args = maskToolArgs(call.name, parsedArgs);
		}
		e.argsRaw = call.arguments;
		e.ok = result.ok;
		e.error = result.error;
		e.ms = elapsedMs;
		e.bytes = result.text.size();
		e.imageBytes = result.imageDataUrl.size();
		if (!result.imageDataUrl.empty()) { e.imageFp = fingerprint64(result.imageDataUrl); }
		if (call.name == "run_command") { e.exitCode = extractExitCode(result.text); }
		audit_->write(e);
	}
	return result;
}

} // namespace agent
} // namespace bot
