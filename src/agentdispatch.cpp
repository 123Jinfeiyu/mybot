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
					"", false, "policy_denied: " + d.reason,
					/* executed= */ false};
			}
		}
		if (!denied) { result = tools_.execute(call.name, parsedArgs); }
		/* SAN-8②：工具层按异常类型标出的参数问题——改口径并标明"没派发"。
		 * 派发漏斗是**唯一**给模型的回执出口，口径在这里统一。 */
		if (result.paramError) {
			result.text = "工具参数错误: " + result.error;
			result.executed = false;
		}
	} catch (const std::exception& e) {
		/* 参数解析失败 = 这一次调用**从未到达工具**，副作用必然未发生（SAN-4） */
		result = AgentToolResult{std::string("工具参数错误: ") + e.what(), "", false, e.what(),
			/* executed= */ false};
	} catch (...) {
		/* A5：本机 LuaJIT 用 C++ 异常实现 luaD_throw，从没有 Lua CFRAME 的 C++
		 * 帧抛出的 Lua 错误**不是** std::exception（同判据见 main.cpp:173-176）。
		 * 只接 std::exception 的话这类抛出会穿透到 runTurn 的 catch(...)，把
		 * 整个会话结束掉——而「REPL 单轮失败不结束会话」是 runAgentMode 的承诺。
		 * 这里折成 tool 回传而不是让它上抛：那是唯一保持「本轮 tool 消息与
		 * assistant.tool_calls 一一对应」的选择（见 agentloop.cpp 的
		 * executeToolRound），也让模型看到「这一步没拿到数据」而不是「什么都没
		 * 发生」。审计照常记这条：ok=false，模型想做什么没做成同样是审计信息。
		 *
		 * executed 按 SAN-4 的口径分两种，不能一律填 false：argsOk 为假说明抛出
		 * 发生在参数解析阶段（调用从未到达工具，副作用必然未发生）；为真则抛出
		 * 来自确认程序或工具本体——Lua 动作脚本可能已经驱动了硬件才抛，此时
		 * 声称「没执行」就是谎报（那正是 SAN-4 要修的形态）。 */
		result = argsOk
			? AgentToolResult{"工具执行抛出非标准异常（Lua 错误或外来抛出），"
					"本轮结果不可用；副作用可能已发生。"
					"请先确认硬件状态，再换一种做法或改用其他工具",
					"", false, "non-std exception", /* executed= */ true}
			: AgentToolResult{std::string("工具参数错误: 非标准异常（")
					+ "参数未能解析，本次调用未派发）",
					"", false, "non-std exception", /* executed= */ false};
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
