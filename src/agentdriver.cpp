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

/* curl 退出码 → 是否属于"重试有意义"的连接类失败。
 * 依据 libcurl 的错误分类（CURLE_* 的语义域），逐个列出而不是用区间：
 *   6  无法解析主机        —— DNS 抖动
 *   7  无法连接            —— 对端未起/网络不可达
 *   28 超时（连接或总时长） —— 现场网络慢
 *   35 SSL 连接错误        —— TLS 握手失败（含对端临时问题）
 *   52 服务器无任何返回    —— 网关抖动/被掐断
 *   55 发送数据失败        —— 上行中断
 *   56 接收数据失败        —— 下行中断
 * 其余（如 1 不支持的协议、3 URL 畸形、60 证书校验失败、67 登录失败）属
 * "配置/凭据问题"，重试不会有任何改变——**481/63 之类也不要加**：
 * 本表是"重试是否可能改变结果"的判据，不是"错误严重不严重"。 */
bool retriableCurlRc(long long rc) {
	switch (rc) {
	case 6: case 7: case 28: case 35: case 52: case 55: case 56:
		return true;
	default:
		return false;
	}
}

/* 回落判据（字段缺失时的保底，第三方驱动仍可命中）。
 * 注意它同时**保留**了旧的六个子串——不是因为我们还相信它们，而是"驱动没给
 * 结构化字段"时别无判据。生产路径的驱动现在都会给字段，这条分支只服务旧驱动。 */
bool looksRetriableByText(const std::string& msg) {
	return msg.find("CURLE_COULDNT_CONNECT") != std::string::npos
		|| msg.find("CURLE_OPERATION_TIMEDOUT") != std::string::npos
		|| msg.find("CURLE_RECV_ERROR") != std::string::npos
		|| msg.find("CURLE_SEND_ERROR") != std::string::npos
		|| msg.find("curl error") != std::string::npos
		|| msg.find("curl rc=") != std::string::npos	/* 真实形态：curl rc=N: <散文> */
		|| msg.find("网络") != std::string::npos;
}

long long clampRetryAfter(long long ms) {
	if (ms <= 0) { return 0; }
	if (ms < kRetryAfterMinMs) { return kRetryAfterMinMs; }
	if (ms > kRetryAfterMaxMs) { return kRetryAfterMaxMs; }
	return ms;
}

class LuaDriverTransport : public ChatTransport {
	MatrixVars_Hardware* driver_;
	AgentDriverConfig cfg_;

	/* 日志 sink：空 = stderr（正是改动前的行为）。 */
	std::ostream& log() const { return cfg_.log != nullptr ? *cfg_.log : std::cerr; }

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
		/* SAN-9：请求体估算字节（宿主侧的线上体积下界，与 agenthistory 的
		 * messageWeight 同口径：不含 JSON 语法开销）。只进审计，不参与任何判定。 */
		const std::size_t reqBytes = req.toString().size();

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
			bool retriable = false;
			DriverError err;
			try {
				/* playQuietRaw（SAN-3）：与 playQuiet 同一路径，但 **ERR 时返回表**
				 * 而不是抛——重试判据需要表上的结构化字段（http_status / curl_rc /
				 * retriable / retry_after_ms）。旧实现只能拿到一句人话，于是判据
				 * 退化成匹配子串，在生产错误上一次都没命中过。
				 * 语义不变：不可重试的错误照样抛出（下面按类分派）。 */
				Json res = driver_->playQuietRaw(req);

				/* Raw 模式把 ERR 表原样交回；这里按类分派：
				 *   可重试 → 由本循环的退避逻辑处理（与抛出的错误走同一条路）；
				 *   不可重试 → 折成异常抛出，文案与改动前逐字一致。 */
				const Json* rs = res.isObj() ? res.find("result") : nullptr;
				const bool isErr = rs != nullptr && rs->isStr() && rs->getValue() == "ERR";
				DriverError e = classifyDriverError(res, "LLM 驱动返回错误");
				if (isErr) {
					if (e.cls == RetryClass::No) { throw std::runtime_error(e.message); }
					err = std::move(e);
					retriable = true;
				} else {
					/* SAN-9：把本次调用的观测值带上（请求体估算字节 + 实际尝试
					 * 次数），宿主据此写审计的模型轮次记录。 */
					ChatResponse parsed = parseDriverResponse(res);
					parsed.reqBytes = reqBytes;
					parsed.attempts = attempt + 1;
					return parsed;
				}
			} catch (const std::exception& ex) {
				/* 已经判过类的可重试错误由上面的 retriable 分支处理；
				 * 这里接手的是**抛出来的**异常：可能是 playQuietRaw 的契约破坏
				 * （无 result 字段）、Lua 层错误、或后续解析失败。 */
				if (!retriable) {
					std::string msg = ex.what();
					Json empty;
					err = classifyDriverError(empty, msg);
					if (err.cls == RetryClass::No) { throw; }
				}
			}

			/* 走到这里 = 这一次尝试失败且属于可重试类 */
			if (attempt == attempts - 1) { throw std::runtime_error(err.message); }
			backoffAndLog(err, attempt, attempts);
		}

		/* 理论上不可达（循环内必然 throw 或 return） */
		throw std::logic_error("重试逻辑错误");
	}

private:
	/* 退避并打日志。退避时长：Throttled 优先采用服务端给的 Retry-After
	 * （**只有服务端说了算的等待才与限流窗口对得上**），否则用本地指数退避；
	 * 两者取大值——服务端要 30s 而本地只等 1s 等于没听。 */
	void backoffAndLog(const DriverError& err, int attempt, int attempts) {
		const int localMs = std::min(cfg_.retryBackoffMs * (1 << attempt),
			llm_limits::kRetryBackoffCapMs);
		long long delayMs = localMs;
		const char* source = "本地指数退避";
		if (err.retryAfterMs > 0 && err.retryAfterMs > delayMs) {
			delayMs = err.retryAfterMs;
			source = "服务端 Retry-After";
		}
		const char* what = err.cls == RetryClass::Throttled ? "限流" : "网络错误";
		log() << "[agent] " << what << "，" << delayMs << "ms 后重试（"
			<< (attempt + 1) << "/" << attempts << "，退避来源: " << source << "）: "
			<< err.message << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		/* 二次中断检查（退避期间可能 Ctrl-C） */
		if (stopRequested()) { throw std::runtime_error("请求被中断"); }
	}
};

} // namespace

DriverError classifyDriverError(const Json& errTable, const std::string& fallbackMessage) {
	DriverError out;
	out.message = fallbackMessage;

	if (errTable.isObj()) {
		if (const Json* m = errTable.find("message"); m != nullptr && m->isStr()) {
			out.message = m->getValue();
		}
		/* 驱动显式给出的可重试位优先（它知道 HTTP 语义） */
		bool haveVerdict = false;
		if (const Json* r = errTable.find("retriable"); r != nullptr && r->isBool()) {
			out.cls = r->getBool() ? RetryClass::Transient : RetryClass::No;
			haveVerdict = true;
		}
		if (const Json* st = errTable.find("http_status"); st != nullptr && st->isInt()) {
			const long long code = st->getInt();
			/* 429 是"限流"而不是普通瞬时错误：退避要尊重服务端的 Retry-After */
			if (code == 429) { out.cls = RetryClass::Throttled; }
			else if (code >= 500 && code <= 599) { out.cls = RetryClass::Transient; }
			else if (!haveVerdict) { out.cls = RetryClass::No; }
			haveVerdict = true;
		}
		if (const Json* ra = errTable.find("retry_after_ms"); ra != nullptr && ra->isInt()) {
			out.retryAfterMs = clampRetryAfter(ra->getInt());
		}
		if (const Json* rc = errTable.find("curl_rc"); rc != nullptr && rc->isInt()) {
			/* 传输层失败：数值退出码是判据（散文靠不住，见文件头注释） */
			out.cls = retriableCurlRc(rc->getInt()) ? RetryClass::Transient : RetryClass::No;
			haveVerdict = true;
		}
		if (haveVerdict) { return out; }
	}

	/* 字段缺失：回落到字符串判据（旧驱动/旧夹具仍可用） */
	out.cls = looksRetriableByText(out.message) ? RetryClass::Transient : RetryClass::No;
	return out;
}

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
