#pragma once
/* agenthttp.hpp —— 基于 libus_http 的 OpenAI 兼容传输实现
 *
 * 这是 agent 模式里唯一依赖第三方 HTTP 库的文件（libus_http → libcurl +
 * OpenSSL）。构建期通过 MYBOT_HAVE_AGENT 开关控制：
 *   - 找到 libus_http：编译真实实现，--agent 可用；
 *   - 找不到：编译一个「明确报不支持」的桩，主程序其余部分照常构建与守门。
 * 这样 CI 基线（debian:bookworm 无 libus_http）不会因为 agent 模式而整体失败。
 */

#include <cstddef>
#include <memory>
#include <string>

#include "agentproto.hpp"

namespace bot {
namespace agent {

struct HttpTransportConfig {
	std::string endpoint;
	std::string apiKey;
	std::string model;
	int maxTokens = 4096;
	double temperature = 0.7;
	double topP = 0.9;
	long timeoutSec = 180;
	long connectTimeoutSec = 15;
	std::size_t maxResponseBytes = 8u << 20;	/* 与脚本输入上限同量级 */
	int maxAttempts = 3;				/* 网络错误/408/429/5xx 的退避重试上限 */
};

/* 构造 HTTP 传输。未启用 agent 支持的构建里调用即抛 std::runtime_error。 */
std::unique_ptr<ChatTransport> makeHttpTransport(const HttpTransportConfig& cfg);

} // namespace agent
} // namespace bot
