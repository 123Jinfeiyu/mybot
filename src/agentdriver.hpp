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

/* ---- 错误的可重试分类（SAN-3，2026-09-22）----
 * 动机：重试循环此前靠**匹配人话子串**判"能不能重试"（`CURLE_*` / `curl error` /
 * `网络`），而生产路径产出的错误长这样：`curl rc=7: Could not connect to server`、
 * `HTTP 503: {...}`——六个子串一个都不匹配（只有测试夹具有意产出匹配串），
 * 于是 `max_retries` / `retry_backoff_ms` 在所有真实 HTTP/curl 失败上都不生效，
 * 且没有任何用例会发现。实测（2026-09-22）：503/429/真实超时/连接被拒
 * **各只发 1 次请求**。
 *
 * 判据改为**结构化字段优先**：驱动在 ERR 表上给出 http_status / curl_rc /
 * retriable / retry_after_ms（见 modules/lib/llm_wire_openai.lua），宿主据此判类；
 * 字段缺失时**回落**到旧的字符串判据（保底，第三方驱动仍可用）。
 * 纯函数、不碰网络与硬件，因此可以离线穷举。 */
enum class RetryClass {
	No,		/* 别再试了：4xx（429 除外）、配额耗尽、请求本身有问题 */
	Transient,	/* 稍后再试：连接类 curl 错误、5xx */
	Throttled,	/* 被限流：429 —— 退避优先采用服务端给的 Retry-After */
};

struct DriverError {
	RetryClass cls = RetryClass::No;
	long long retryAfterMs = 0;	/* >0 = 服务端明确要求等这么久（已钳制） */
	std::string message;		/* 原样保留：这是进日志与异常的人话 */
};

/* 把驱动返回的 ERR 表折成可判类的错误。fallbackMessage 是字段缺失时的兜底文案
 * （调用方传 playQuiet 抛出的异常文本）。 */
DriverError classifyDriverError(const Json& errTable, const std::string& fallbackMessage);

/* 服务端 Retry-After 的钳制区间（llm_limits 之外的第二个落点，因为它是
 * **协议**约束而非配置面）：下限 1s（短于 1s 的退避对限流无意义），
 * 上限 5min（坏网关回一个天文数字不能把 agent 挂死一天）。
 * 区间与理由的单点在本文件；llmconfig 的退避上限（kRetryBackoffCapMs=60s）
 * 管的是**本地指数退避**，两者刻意不同。 */
inline constexpr long long kRetryAfterMinMs = 1000;
inline constexpr long long kRetryAfterMaxMs = 5 * 60 * 1000;

} // namespace agent
} // namespace bot
