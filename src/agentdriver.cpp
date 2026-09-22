#include "agentdriver.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "hardware.hpp"
#include "llmconfig.hpp"	/* llm_limits::kRetryBackoffCapMs（退避单次上限） */
#include "luavar.hpp"	/* B3：dynamic_cast<MtVars_Lua*> 设置图片游标表 */
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

class LuaDriverTransport : public ChatTransport {
	MatrixVars_Hardware* driver_;
	AgentDriverConfig cfg_;

	/* 日志 sink：空 = stderr（正是改动前的行为）。 */
	std::ostream& log() const { return cfg_.log != nullptr ? *cfg_.log : std::cerr; }

	/* U-1：判定是否为可重试的网络错误 */
	bool isRetriableError(const std::string& msg) const {
		/* 基于 llm_http.lua 的错误消息格式判定（libcurl 错误码） */
		return msg.find("CURLE_COULDNT_CONNECT") != std::string::npos
			|| msg.find("CURLE_OPERATION_TIMEDOUT") != std::string::npos
			|| msg.find("CURLE_RECV_ERROR") != std::string::npos
			|| msg.find("CURLE_SEND_ERROR") != std::string::npos
			|| msg.find("curl error") != std::string::npos
			|| msg.find("网络") != std::string::npos;
	}

public:
	LuaDriverTransport(MatrixVars_Hardware* driver, AgentDriverConfig cfg)
		: driver_(driver), cfg_(std::move(cfg)) {}

	ChatResponse complete(const std::vector<ChatMessage>& messages,
			const std::vector<ToolSpec>& tools) override {
		/* D4：Ctrl-C 后不再发起新的模型轮，让主流程尽快走 deactivate */
		if (stopRequested()) { throw std::runtime_error("请求被中断"); }

		/* B3：图片以游标占位过 Json/Lua 边界，字节只在 Lua 组包期落值一次。
		 * imgs 指向 messages 里的字符串（调用全程存活），须在 playQuiet 全程
		 * 挂在驱动上、结束即摘（游标是组包期通道，不是常驻数据源）。 */
		std::vector<const std::string*> imgs;
		Json req = buildDriverRequest(cfg_.model, messages, tools,
			cfg_.maxTokens, cfg_.temperature, cfg_.topP, cfg_.timeoutSec, &imgs);
		if (!cfg_.endpoint.empty()) { req.put("endpoint", JsonString(cfg_.endpoint)); }
		/* 驱动特有字段：宿主不理解它们，只负责按配置透传 */
		if (!cfg_.reasoningEffort.empty()) {
			req.put("reasoning_effort", JsonString(cfg_.reasoningEffort));
		}
		if (!cfg_.thinking.empty()) { req.put("thinking", JsonString(cfg_.thinking)); }

		/* 游标表挂/摘（RAII）：只对 Lua 驱动有意义（入口已校验 kindName） */
		auto* luaDriver = dynamic_cast<MtVars_Lua*>(driver_);
		struct CursorScope {
			MtVars_Lua* d;
			~CursorScope() { if (d != nullptr) { d->setHostImages(nullptr); } }
		} cursorScope{luaDriver};
		if (luaDriver != nullptr) { luaDriver->setHostImages(&imgs); }

		/* U-1：重试循环（指数退避）。attempts 钳到 ≥1：maxRetries 是「含首次的
		 * 尝试次数」，而配置文件的 max_retries 允许 0（= 不重试）——若直接拿 0
		 * 当循环上界，就会一次都不发并落到末尾的 logic_error，「不重试」变成
		 * 「什么都不做」。 */
		const int attempts = std::max(1, cfg_.maxRetries);
		for (int attempt = 0; attempt < attempts; ++attempt) {
			try {
				/* playQuiet：与 play 同一路径但不回显结果表——模型响应绝不能混进
				 * stdout。驱动的 ERR 契约由内部 ensureOk 抛出（带驱动给的 message）。 */
				Json res = driver_->playQuiet(req);
				return parseDriverResponse(res);

			} catch (const std::exception& e) {
				std::string msg = e.what();

				/* 判定是否可重试 */
				if (!isRetriableError(msg)) {
					throw;	/* 非网络错误（如 API key 错误），直接抛出 */
				}

				/* 最后一次也失败了 */
				if (attempt == attempts - 1) {
					throw;	/* 用尽重试次数 */
				}

				/* 指数退避。单次上限：缺省区间（1s 基础 × 3 次尝试）够不到它，但
				 * 配置文件放宽 max_retries/retry_backoff_ms 后 `backoff << attempt`
				 * 会长到小时级——对「重启一下网关就好」的抖动，等待比失败更糟。 */
				const int delayMs = std::min(cfg_.retryBackoffMs * (1 << attempt),
					llm_limits::kRetryBackoffCapMs);
				log() << "[agent] 网络错误，" << delayMs << "ms 后重试（"
					<< (attempt + 1) << "/" << attempts << "）: "
					<< msg << std::endl;

				std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

				/* 二次中断检查（退避期间可能 Ctrl-C） */
				if (stopRequested()) {
					throw std::runtime_error("请求被中断");
				}
			}
		}

		/* 理论上不可达（循环内必然 throw 或 return） */
		throw std::logic_error("重试逻辑错误");
	}
};

} // namespace

std::unique_ptr<ChatTransport> makeDriverTransport(MatrixVars_Hardware* driver,
		AgentDriverConfig cfg) {
	if (driver == nullptr) {
		throw IllegalArgumentException("LLM 驱动变量为空（结构脚本未声明且未注入）");
	}
	/* 只接受 Lua 驱动：别的硬件变量没有 call 语义，playQuiet 会落到默认实现
	 * （等于 play），拿回来的表必然缺 content/tool_calls，报错离真实成因很远 */
	if (std::string_view(driver->kindName()) != "lua") {
		throw IllegalArgumentException("LLM 驱动变量必须是 Lua 驱动（modules/Llm*.lua 注册）");
	}
	return std::make_unique<LuaDriverTransport>(driver, std::move(cfg));
}

} // namespace agent
} // namespace bot
