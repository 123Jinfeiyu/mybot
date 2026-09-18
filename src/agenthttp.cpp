#include "agenthttp.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "json.hpp"
#include "util.hpp"

#ifdef MYBOT_HAVE_AGENT
#include <us/HttpClient.hpp>
#endif

namespace bot {
namespace agent {

#ifdef MYBOT_HAVE_AGENT

namespace {

std::string snippet(const std::string& s, std::size_t n = 400) {
	return s.size() <= n ? s : s.substr(0, n) + "...(截断)";
}

/* 可重试的 HTTP 状态：请求超时、限流、服务端/网关错误；其余 4xx 重试无意义 */
bool retryableStatus(long code) {
	return code == 408 || code == 429 || (code >= 500 && code <= 599);
}

/* 退避等待：分片睡眠，Ctrl-C（stopRequested）时立即放弃等待 */
void backoff(int attempt, const std::string& why) {
	int seconds = 2 * attempt;
	std::cerr << "[agent][重试] " << why << "，" << seconds << "s 后进行第 "
		<< (attempt + 1) << " 次尝试" << std::endl;
	for (int i = 0; i < seconds * 10; ++i) {
		if (stopRequested()) { throw std::runtime_error("请求被中断"); }
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

class HttpTransport : public ChatTransport {
	HttpTransportConfig cfg_;

public:
	explicit HttpTransport(HttpTransportConfig cfg) : cfg_(std::move(cfg)) {}

	ChatResponse complete(const std::vector<ChatMessage>& messages,
			const std::vector<ToolSpec>& tools) override {
		Json body = buildChatRequest(cfg_.model, messages, tools,
			cfg_.maxTokens, cfg_.temperature, cfg_.topP);
		std::string payload = body.toString();

		for (int attempt = 1;; ++attempt) {
			std::string raw;
			long code = 0;
			try {
				pmc::net::HttpClient client;
				client.set_header("Authorization: Bearer " + cfg_.apiKey);
				client.set_header("Content-Type: application/json");
				client.set_timeout(cfg_.timeoutSec);
				client.set_connect_timeout(cfg_.connectTimeoutSec);
				client.set_max_response_size(cfg_.maxResponseBytes);
				client.post_stream_sync(cfg_.endpoint, payload,
					[&raw](const std::string& chunk) { raw += chunk; });
				code = client.get_response_code();
			} catch (const std::exception& e) {
				if (attempt >= cfg_.maxAttempts) {
					throw std::runtime_error(std::string("请求模型失败: ") + e.what());
				}
				backoff(attempt, std::string("网络错误: ") + e.what());
				continue;
			}

			if (code != 200) {
				if (retryableStatus(code) && attempt < cfg_.maxAttempts) {
					backoff(attempt, "HTTP " + std::to_string(code));
					continue;
				}
				throw std::runtime_error("模型接口返回 HTTP " + std::to_string(code)
					+ ": " + snippet(raw));
			}

			return parseChatResponse(raw);
		}
	}
};

} // namespace

std::unique_ptr<ChatTransport> makeHttpTransport(const HttpTransportConfig& cfg) {
	return std::make_unique<HttpTransport>(cfg);
}

#else

std::unique_ptr<ChatTransport> makeHttpTransport(const HttpTransportConfig&) {
	throw std::runtime_error(
		"本构建未启用 agent 模式：未找到 libus_http（us/HttpClient.hpp 与 libus_httpclient.so）。"
		"安装 libus_http 后重新执行 cmake 构建即可；也可以用 -DMYBOT_AGENT=OFF 显式关闭。");
}

#endif

} // namespace agent
} // namespace bot
