#pragma once
/* agentproto.hpp —— agent 模式协议层：OpenAI 兼容消息/工具调用编解码 + 传输抽象
 *
 * 分层目的（与内置守门的关系）：本层只做「值 ↔ JSON」的纯转换，不碰 HTTP，
 * 因此可以随测试二进制一起编译、离线单测；真正发出网络请求的实现见
 * agenthttp.cpp（依赖 libus_http，构建期可选）。工具循环 agentloop 只认识
 * 这里的 ChatTransport 抽象，测试可注入脚本化传输，无需网络与 API key。
 */

#include <string>
#include <vector>

#include "json.hpp"

namespace bot {
namespace agent {

/* 一次工具调用。arguments 保留模型给出的原始 JSON 字符串
 * （不在此处解析：解析失败要作为「工具参数错误」回传给模型，而不是
 * 在协议层把整轮对话打断）。 */
struct ToolCall {
	std::string id;
	std::string name;
	std::string arguments;
};

/* 对话消息（OpenAI 兼容子集）：
 *   role=system/user  → content
 *   role=assistant    → content（可为空）+ toolCalls
 *   role=tool         → toolCallId + content（工具结果） */
struct ChatMessage {
	std::string role;
	std::string content;
	std::vector<ToolCall> toolCalls;
	std::string toolCallId;
};

/* 暴露给模型的工具声明（parameters 为 JSON Schema object） */
struct ToolSpec {
	std::string name;
	std::string description;
	Json parameters;
};

/* 一轮模型响应：最终文本，或一组工具调用（模型常先给一句过渡语再发调用，
 * 故两者可同时存在）。 */
struct ChatResponse {
	std::string content;
	std::vector<ToolCall> toolCalls;
	bool hasToolCalls() const { return !toolCalls.empty(); }
};

/* 传输抽象：complete() = 一轮模型调用。失败抛 std::exception（子类给出人话报错）。 */
class ChatTransport {
public:
	virtual ~ChatTransport() = default;
	virtual ChatResponse complete(const std::vector<ChatMessage>& messages,
			const std::vector<ToolSpec>& tools) = 0;
};

/* 组装非流式请求体（stream=false；agent 模式的工具循环按整轮取响应，
 * 不需要 SSE 增量协议）。tools 为空时不发 tools/tool_choice 字段
 * （部分兼容实现拒绝空数组）。 */
Json buildChatRequest(const std::string& model,
		const std::vector<ChatMessage>& messages,
		const std::vector<ToolSpec>& tools,
		int maxTokens, double temperature, double topP);

/* 单条消息 → JSON（tool 消息用 tool_call_id 对齐 assistant 的 tool_calls） */
Json messageToJson(const ChatMessage& msg);

/* 解析响应体：choices[0].message → content / tool_calls。
 * 结构不符、含 error 字段、既无文本也无工具调用一律抛 std::runtime_error
 * （带响应片段，便于定位是网关报错还是模型抽风）。 */
ChatResponse parseChatResponse(const std::string& body);

/* 解析 tool_call.arguments（模型给的 JSON 字符串）。
 * 空串 → 空对象；非对象或非法 JSON → 抛 IllegalArgumentException
 * （由工具循环折成参数错误回传模型，不打断会话）。 */
Json parseToolArguments(const std::string& arguments);

} // namespace agent
} // namespace bot
