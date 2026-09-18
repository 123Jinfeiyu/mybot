#pragma once
/* agentdriver.hpp —— 把一颗 Lua 协议驱动当成模型传输（ChatTransport 实现）
 *
 * 这是 C++ 与「模型调用」之间唯一的接触面：宿主只递规范化请求、只收规范化响应，
 * 不认识任何 provider 报文字段，也不链接任何 HTTP 库。
 */

#include <memory>
#include <string>

#include "agentproto.hpp"

namespace bot {

class MatrixVars_Hardware;

namespace agent {

struct AgentDriverConfig {
	std::string model;	/* 空 = 用驱动自带的缺省模型 */
	std::string endpoint;	/* 空 = 用驱动自带的缺省接入点 */
	int maxTokens = 4096;
	double temperature = 0.7;
	double topP = 0.9;
	long timeoutSec = 180;
	/* 驱动特有字段：非空即原样放进请求（如 DeepSeek 的 thinking / reasoning_effort） */
	std::string reasoningEffort;
	std::string thinking;
};

/* driver 必须是 Lua 驱动变量（modules/Llm*.lua 注册而成），否则抛
 * std::invalid_argument——别的硬件变量没有 call 语义。 */
std::unique_ptr<ChatTransport> makeDriverTransport(MatrixVars_Hardware* driver,
		AgentDriverConfig cfg);

} // namespace agent
} // namespace bot
