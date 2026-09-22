#pragma once
/* agentproto.hpp —— agent 模式的**规范化**协议层（宿主侧）
 *
 * 分层（替换掉原先内嵌 libus_http 的 agenthttp 后）：
 *   宿主（本文件）  ：规范化消息/工具 → Json；驱动返回的 Json → 规范化响应
 *   Lua 协议驱动    ：规范化请求 → provider 报文；provider 响应 → 规范化响应
 *   Lua 传输库      ：llm_http.lua 用 ffi 直驱 libcurl
 *
 * 于是「换模型/换厂商」= 换一个 modules/Llm*.lua，宿主不需要重编、也不链接任何
 * HTTP 库；provider 特有字段（如 DeepSeek 的 reasoning_content）经 extras
 * 原样逐轮带回，由驱动决定怎么用。
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

/* 规范化对话消息：
 *   role=system/user  → content
 *   role=assistant    → content（可为空）+ toolCalls + extras
 *   role=tool         → toolCallId + content
 * extras 是 provider 特有的附加字段（如 DeepSeek 的 reasoning_content），
 * 宿主不理解也不修改，只负责逐轮原样带回给驱动。 */
struct ChatMessage {
	std::string role;
	std::string content;
	std::vector<ToolCall> toolCalls;
	std::string toolCallId;
	Json extras;	/* 对象；空表示无 */
	/* 多模态图片（data URL）。非空时该消息的 content 在线上会变成
	 * [{type:"text"},{type:"image_url"}] 数组——OpenAI 协议里 tool 消息只允许
	 * 文本，所以图片必须由工具循环另起一条 user 消息注入（见 agentloop）。 */
	std::vector<std::string> images;
};

/* 暴露给模型的工具声明（parameters 为 JSON Schema object） */
struct ToolSpec {
	std::string name;
	std::string description;
	Json parameters;
};

/* 一轮模型响应：最终文本，或一组工具调用（模型常先给一句过渡语再发调用）。 */
struct ChatResponse {
	std::string content;
	std::vector<ToolCall> toolCalls;
	Json extras;	/* 对象；空表示无 */
	/* SAN-9（2026-09-22）：provider 给的 token 用量，**原样透传**。
	 * 宿主不理解它（各家字段名可能不同），只在审计里挑 prompt_tokens /
	 * completion_tokens 记账——**只记 provider 说的数，不做任何推算或计费估算**。
	 * 缺省空 = provider 没给。 */
	Json usage;
	/* SAN-9：本次模型调用的请求体估算字节与**实际尝试次数**（>1 = 发生过重试）。
	 * 传输层填，宿主只用于审计记录；离线夹具不填（0 表示未统计）。 */
	std::size_t reqBytes = 0;
	int attempts = 0;
	bool hasToolCalls() const { return !toolCalls.empty(); }
};

/* 传输抽象：complete() = 一轮模型调用。失败抛 std::exception。 */
class ChatTransport {
public:
	virtual ~ChatTransport() = default;
	virtual ChatResponse complete(const std::vector<ChatMessage>& messages,
			const std::vector<ToolSpec>& tools) = 0;
};

/* 规范化请求（宿主 → 驱动）：驱动据此生成 provider 报文。
 * tools 为空时不发 tools 字段（部分兼容实现拒绝空数组）。
 * imageRefs（可空，B3）：非空时消息里的图片以 {"__bot_img": N} 游标占位穿过
 * Json 与 Lua 边界（N 从 1 起，指向该向量里第 N 个字符串），图片字节只在
 * 请求体组装期经宿主 C 函数落值一次——不再为同一 payload 在 req 树、
 * 参数合并、Lua 压栈各拷一份。向量指针须在驱动 playQuiet 全程存活。 */
Json buildDriverRequest(const std::string& model,
		const std::vector<ChatMessage>& messages,
		const std::vector<ToolSpec>& tools,
		int maxTokens, double temperature, double topP, long timeoutSec,
		std::vector<const std::string*>* imageRefs = nullptr);

/* 单条消息 → 规范化 JSON（imageRefs 语义同 buildDriverRequest；空 = 图片
 * 原样拷入，供离线单测/不经过 Lua 边界的调用面） */
Json messageToJson(const ChatMessage& msg, std::vector<const std::string*>* imageRefs = nullptr);

/* 解析驱动返回的规范化响应（{result="OK", content, tool_calls, extras}）。
 * result 非 "OK"、或既无文本也无工具调用一律抛 std::runtime_error。 */
ChatResponse parseDriverResponse(const Json& res);

/* 解析 tool_call.arguments（模型给的 JSON 字符串）。
 * 空串 → 空对象；非对象或非法 JSON → 抛 IllegalArgumentException
 * （由工具循环折成参数错误回传模型，不打断会话）。 */
Json parseToolArguments(const std::string& arguments);

} // namespace agent
} // namespace bot
