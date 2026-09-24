#pragma once
/* agentproto.hpp —— agent 工具后端与 Rust agent 之间的规范化数据结构（C++ 侧）
 *
 * 沿革：这里曾有宿主 ⇄ Lua 协议驱动的整条规范化协议（ChatTransport /
 * buildDriverRequest / parseDriverResponse）——模型调用由 modules/Llm*.lua 承担。
 * agent 循环迁 Rust 后（agent/src，模型调用直连 provider），传输层随
 * agentdriver 一起删除；本文件保留的是仍然存活的两侧共享结构：
 *   * ToolCall / parseToolArguments —— MCP 后端帧协议的工具调用面
 *     （mcpbackend 组装 → DispatchGate 派发 → agenttools 执行）；
 *   * ChatMessage / ToolSpec —— 会话文件读写原语（agenthistory，批次 D 的
 *     语义参考）与工具声明（AgentToolRegistry::specs）。
 */

#include <string>
#include <vector>

#include "json.hpp"

namespace bot {
namespace agent {

/* 一次工具调用。arguments 保留模型给出的原始 JSON 字符串（不在此处解析：
 * 解析失败要作为「工具参数错误」回传给模型，而不是在协议层打断整轮对话）。 */
struct ToolCall {
	std::string id;
	std::string name;
	std::string arguments;
};

/* 规范化对话消息（会话文件 agenthistory 的落盘单位；字段语义见
 * encodeSessionMessage）：
 *   role=system/user  → content
 *   role=assistant    → content（可为空）+ toolCalls + extras
 *   role=tool         → toolCallId + content
 * extras 是 provider 特有的附加字段，宿主不理解也不修改。 */
struct ChatMessage {
	std::string role;
	std::string content;
	std::vector<ToolCall> toolCalls;
	std::string toolCallId;
	Json extras;	/* 对象；空表示无 */
	/* 多模态图片（data URL）。会话文件落盘时以占位文本替代（字节不落盘）。 */
	std::vector<std::string> images;
};

/* 暴露给模型的工具声明（parameters 为 JSON Schema object）。
 * AgentToolRegistry::specs 产出，MCP 后端的 tool.list 与会话文件的
 * 工具集指纹（sessionToolsFingerprint）共用。 */
struct ToolSpec {
	std::string name;
	std::string description;
	Json parameters;
};

/* 解析 tool_call.arguments（模型给的 JSON 字符串）。
 * 空串 → 空对象；非对象或非法 JSON → 抛 IllegalArgumentException
 * （由派发漏斗折成参数错误回传模型，不打断会话）。 */
Json parseToolArguments(const std::string& arguments);

} // namespace agent
} // namespace bot
