#pragma once
/* agentdriver.hpp —— 把一颗 Lua 协议驱动当成模型传输（ChatTransport 实现）
 *
 * 这是 C++ 与「模型调用」之间唯一的接触面：宿主只递规范化请求、只收规范化响应，
 * 不认识任何 provider 报文字段，也不链接任何 HTTP 库。
 */

#include <iosfwd>
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
	/* U-1：LLM 网络错误自动重试（指数退避） */
	int maxRetries = 3;	/* 尝试次数**含首次**（故实际重试 maxRetries-1 次）。配置文件的
				 * max_retries 允许 0（= 不重试），驱动侧钳到 ≥1——「不重试」
				 * 仍是发一次，而不是一次都不发。 */
	int retryBackoffMs = 1000;	/* 退避基础时长（指数增长：1s, 2s, 4s；单次钳到
					 * llm_limits::kRetryBackoffCapMs） */
	/* 重试信息的日志 sink。空 = std::cerr（现状，行为保持）——这条路径要能被
	 * 调用方接走才算「注入」，硬编码 std::cerr 时离线用例抓不到它、宿主也没得选
	 * （C5）。它**只**是日志流：绝不写 stdout（那是模型的回答）。 */
	std::ostream* log = nullptr;
};

/* driver 必须是 Lua 驱动变量（modules/Llm*.lua 注册而成），否则抛
 * std::invalid_argument——别的硬件变量没有 call 语义。 */
std::unique_ptr<ChatTransport> makeDriverTransport(MatrixVars_Hardware* driver,
		AgentDriverConfig cfg);

} // namespace agent
} // namespace bot
