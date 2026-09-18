#include "agentdriver.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#include "luavar.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

class LuaDriverTransport : public ChatTransport {
	MatrixVars_Hardware* driver_;
	AgentDriverConfig cfg_;

public:
	LuaDriverTransport(MatrixVars_Hardware* driver, AgentDriverConfig cfg)
		: driver_(driver), cfg_(std::move(cfg)) {}

	ChatResponse complete(const std::vector<ChatMessage>& messages,
			const std::vector<ToolSpec>& tools) override {
		/* D4：Ctrl-C 后不再发起新的模型轮，让主流程尽快走 deactivate */
		if (stopRequested()) { throw std::runtime_error("请求被中断"); }

		Json req = buildDriverRequest(cfg_.model, messages, tools,
			cfg_.maxTokens, cfg_.temperature, cfg_.topP, cfg_.timeoutSec);
		if (!cfg_.endpoint.empty()) { req.put("endpoint", JsonString(cfg_.endpoint)); }

		/* playQuiet：与 play 同一路径但不回显结果表——模型响应绝不能混进
		 * stdout。驱动的 ERR 契约由内部 ensureOk 抛出（带驱动给的 message）。 */
		Json res = driver_->playQuiet(req);
		return parseDriverResponse(res);
	}
};

} // namespace

std::unique_ptr<ChatTransport> makeDriverTransport(MatrixVars_Hardware* driver,
		AgentDriverConfig cfg) {
	if (driver == nullptr) {
		throw std::invalid_argument("LLM 驱动变量为空（结构脚本未声明且未注入）");
	}
	/* 只接受 Lua 驱动：别的硬件变量没有 call 语义，playQuiet 会落到默认实现
	 * （等于 play），拿回来的表必然缺 content/tool_calls，报错离真实成因很远 */
	if (dynamic_cast<MtVars_Lua*>(driver) == nullptr) {
		throw std::invalid_argument("LLM 驱动变量必须是 Lua 驱动（modules/Llm*.lua 注册）");
	}
	return std::make_unique<LuaDriverTransport>(driver, std::move(cfg));
}

} // namespace agent
} // namespace bot
