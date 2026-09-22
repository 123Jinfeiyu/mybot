#include "agentloop.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "util.hpp"

namespace bot {
namespace agent {

namespace {

/* 多行文本统缩进，便于 stderr 里的工具日志成块可读 */
std::string indentLines(const std::string& s, std::size_t n) {
	std::string pad(n, ' ');
	std::string out;
	std::size_t pos = 0;
	while (pos < s.size()) {
		out += pad;
		std::size_t nl = s.find('\n', pos);
		if (nl == std::string::npos) {
			out += s.substr(pos);
			break;
		}
		out += s.substr(pos, nl - pos + 1);
		pos = nl + 1;
	}
	return out;
}

/* 转义日志标记（2026-09-22 安全改进）：防止工具输出含 [FAIL] 等关键字时
 * 干扰日志解析器。只转义行首的方括号标记，保留其他位置的方括号不变。 */
std::string escapeLogMarkers(const std::string& s) {
	std::string out;
	bool atLineStart = true;
	for (std::size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (atLineStart && c == '[') {
			out += "\\[";  /* 转义行首方括号 */
		} else {
			out += c;
		}
		atLineStart = (c == '\n');
	}
	return out;
}

} // namespace

AgentSession::AgentSession(ChatTransport& transport, const AgentToolRegistry& tools,
		AgentOptions opts)
	: transport_(transport), tools_(tools), opts_(std::move(opts)) {
	ChatMessage sys;
	sys.role = "system";
	sys.content = opts_.systemPrompt;
	messages_.push_back(std::move(sys));
}

void AgentSession::log(const std::string& text) const {
	std::ostream& os = opts_.log != nullptr ? *opts_.log : std::cerr;
	os << text << "\n";
}

/* ---- trimHistory：历史按字节预算裁剪（REPL 长会话）----
 * 单位是「任务块」：一条任务消息（user 且不带图片——图片消息属于它前面
 * 的任务块）直到下一条任务消息之前的全部消息。整块裁剪保证 assistant 的
 * tool_calls 与其 tool 消息、以及夹在 tool 消息之后的图片 user 消息不被
 * 拆散——半截序列发给服务端会被 400 拒绝。
 * 老块连同其中的 base64 图片一起释放：图片是历史体积主项（一张照片数百 KB），
 * 只按条数裁剪挡不住它们。 */
namespace {

/* 估算一条消息的线上体积（下界：不计 JSON 语法开销，正文/调用参数/图片/extras
 * 占绝对大头）。extras 当前只装 reasoning_content 一类文本，整体串行化计入。 */
std::size_t messageWeight(const ChatMessage& m) {
	std::size_t n = m.content.size() + m.toolCallId.size();
	for (const ToolCall& c : m.toolCalls) {
		n += c.id.size() + c.name.size() + c.arguments.size();
	}
	for (const std::string& img : m.images) { n += img.size(); }
	if (!m.extras.isNull()) { n += m.extras.toString().size(); }
	return n;
}

} // namespace

void AgentSession::trimHistory() {
	if (opts_.maxHistoryBytes == 0 || messages_.size() <= 1) { return; }

	/* 任务块起点（messages_[0] 恒为 system，跳过） */
	std::vector<std::size_t> starts;
	for (std::size_t i = 1; i < messages_.size(); ++i) {
		const ChatMessage& m = messages_[i];
		if (m.role == "user" && m.images.empty()) { starts.push_back(i); }
	}
	if (starts.empty()) { return; }	/* 防御：历史理应至少含一次任务 */

	std::vector<std::size_t> blockWeight(starts.size(), 0);
	for (std::size_t j = 0; j < starts.size(); ++j) {
		std::size_t end = (j + 1 < starts.size()) ? starts[j + 1] : messages_.size();
		for (std::size_t k = starts[j]; k < end; ++k) { blockWeight[j] += messageWeight(messages_[k]); }
	}

	/* 从最新块向旧累计；最新块无条件保留（不能把对话裁空），其余装得下就留 */
	std::size_t keep = starts.size() - 1;
	std::size_t acc = blockWeight[keep];
	while (keep > 0 && acc + blockWeight[keep - 1] <= opts_.maxHistoryBytes) {
		--keep;
		acc += blockWeight[keep];
	}
	if (keep == 0) { return; }	/* 全部塞得下，无事可做 */

	std::size_t dropped = starts[keep] - 1;	/* system 之后、最新保留块之前的条数 */
	messages_.erase(messages_.begin() + 1, messages_.begin() + starts[keep]);
	if (opts_.verbose) {
		log("[历史] 超出字节预算，已裁掉最早的 " + std::to_string(dropped)
			+ " 条消息（含其中的图片）；更早的任务上下文不再可见");
	}
}

std::string AgentSession::runTurn(const std::string& userTask) {
	trimHistory();
	/* 任务内聚合预算按任务清零（AG-4）：它约束的是「这一个任务往上下文里灌了
	 * 多少字节」，跨任务的体积由 trimHistory 的 maxHistoryBytes 管。 */
	taskToolResultBytes_ = 0;
	taskImageBytes_ = 0;
	const std::size_t base = messages_.size();

	/* 本轮里最后一个「可直接发给服务端」的边界：起点（只有这条 user 消息）算完整，
	 * 此后每当一条 assistant 的 tool_calls 拿到全部 tool 回传（连同其后的图片消息）
	 * 就推进一格。异常时只丢弃边界之后写入的部分——需要回滚的从来不是「本轮」，
	 * 而是「半截的 tool_calls 序列」（服务端会 400 拒绝那种）。 */
	std::size_t complete = base;

	ChatMessage user;
	user.role = "user";
	user.content = userTask;
	messages_.push_back(std::move(user));

	try {
		for (int turn = 0; turn < opts_.maxTurns; ++turn) {
			/* D4：SIGINT/SIGTERM 置位后不再发起新的模型/工具轮，
			 * 让主流程尽快走 deactivate 清理链 */
			if (stopRequested()) {
				throw std::runtime_error("执行被中断");
			}

			/* 取数：一次模型调用 */
			ChatResponse resp = transport_.complete(messages_, tools_.specs());

			/* 收尾：模型不再要工具 → 记下最终回答并返回 */
			if (!resp.hasToolCalls()) { return appendFinalAnswer(resp); }

			/* 模型常先给一句过渡语再发 tool_calls；过渡语只进日志，
			 * 最终回答只由「无工具调用」的那一轮决定 */
			if (opts_.verbose && !resp.content.empty()) {
				log("[模型] " + resp.content);
			}

			/* 执行：assistant(tool_calls) → 全部 tool 回传 → 图片 */
			executeToolRound(resp, turn);

			/* 到此为止 tool_calls 与 tool 回传一一对上，这段历史可以原样发送 */
			complete = messages_.size();
		}

		throw std::runtime_error("达到工具轮次上限（" + std::to_string(opts_.maxTurns)
			+ " 轮）仍未得到最终回答；可调大 --llm-max-turns，或把任务拆小");
	} catch (...) {
		/* 只裁掉半截的尾部：轮次上限这条路径走到这里时 complete 已等于当前长度，
		 * 于是本轮真实执行过的动作留在历史里——硬件状态不会因为「没拿到最终回答」
		 * 而回退，模型也不该被告知什么都没发生。 */
		if (messages_.size() > complete) { messages_.resize(complete); }
		throw;
	}
}

/* 收尾：无工具调用的那一轮。provider 附加字段（如 DeepSeek 的 reasoning_content）
 * 必须跟着这条 assistant 消息留在历史里——下一轮带 tools 的请求要把它们完整回传，
 * 否则 DeepSeek 直接 400。 */
std::string AgentSession::appendFinalAnswer(const ChatResponse& resp) {
	ChatMessage asst;
	asst.role = "assistant";
	asst.content = resp.content;
	asst.extras = resp.extras;
	messages_.push_back(std::move(asst));
	return resp.content;
}

/* 派发单个工具调用：执行 → （必要时按任务内预算降级）→ 写 tool 消息。
 * 返回该结果携带的图片 data URL（无图片时为空串）。
 *
 * 预算降级（2026-09-21 审计 AG-4）：单个任务的累计工具结果/图片字节数超过
 * AgentOptions 的上限时，**不把这一条结果放进历史**，换成一句说明回给模型。
 * 为什么是「换掉」而不是「截断」：截断工具输出会让模型看到半截内容却以为完整
 * （比没有更危险）；为什么不是「报错终止」：那是把可用性问题升级成任务失败，
 * 而模型完全有能力换一个小一点的步子重试。
 * 判定在**执行之后**做：体积要拿到结果才知道，而工具已经产生副作用（比如
 * 动作脚本已经跑了），此时谎称「没执行」会误导模型——所以文案必须如实说明
 * 「已执行，但结果体积超限」。
 * 本函数不写 assistant 消息、不动 complete 边界：调用方保证一轮里的 tool 消息
 * 与 assistant.tool_calls 一一对应。 */
std::string AgentSession::dispatchOneCall(const ToolCall& call, int turn) {
	(void)turn;
	AgentToolResult result;
	try {
		Json args = parseToolArguments(call.arguments);
		result = tools_.execute(call.name, args);
	} catch (const std::exception& e) {
		result = {std::string("工具参数错误: ") + e.what(), ""};
	}

	/* ---- 任务内预算判定（AG-4 + AG-21）---- */
	const bool textOver = opts_.maxTaskToolResultBytes > 0
		&& taskToolResultBytes_ + result.text.size() > opts_.maxTaskToolResultBytes;
	const bool imgOver = !result.imageDataUrl.empty() && opts_.maxTaskImageBytes > 0
		&& taskImageBytes_ + result.imageDataUrl.size() > opts_.maxTaskImageBytes;
	if (textOver || imgOver) {
		const std::string what = imgOver ? "图片" : "工具输出";
		const std::size_t used = imgOver ? taskImageBytes_ : taskToolResultBytes_;
		const std::size_t cap = imgOver ? opts_.maxTaskImageBytes : opts_.maxTaskToolResultBytes;
		std::string note = "本次工具结果未注入上下文：" + what + "累计体积已达任务上限（已用 "
			+ std::to_string(used) + " + 本次 " 
			+ std::to_string(imgOver ? result.imageDataUrl.size() : result.text.size())
			+ " 字节，上限 " + std::to_string(cap) + " 字节）。";
		if (imgOver) {
			/* AG-4′（第三轮审计）：这里曾原样拼进 capture_image 的成功文案，而那句
			 * 末尾是「画面已加入你的上下文」——与本条开头的「未注入上下文」直接
			 * 矛盾，而**图片确实没注入**。模型读到自相矛盾的两句，按哪句行事都错。
			 * 本项目既有的口径是「不向模型谎报状态」（见上面 AG-4 的设计注释），
			 * 所以把描述里那句成功断言剥掉，再显式说明「只把描述给你」。 */
			std::string desc = result.text;
			const std::string claim = "画面已加入你的上下文。";
			if (const std::size_t at = desc.find(claim); at != std::string::npos) {
				desc.erase(at, claim.size());
			}
			note += "已把工具的描述性文字给你（画面本身没有进上下文）：" + desc;
			/* 图片预算是**逐任务单向累加**的：一旦某张图装不下，本任务就再也塞不进
			 * 任何图片（额度只增不减）。不明说的话，模型会一直重试拍照直到轮次上限
			 * （实测：一次任务里连拒 3 次后以「达到工具轮次上限」收场——那 3 轮全
			 * 是白烧的）。这里把「本任务图片额度已尽」讲成硬事实并给出唯一出路。
			 * 判据必须与上面触发降级的判据**逐字一致**（都是「已用 + 本次 > 上限」）：
			 * 写成 used >= cap 是错的——本次 87 / 上限 100 / 已用 87 时 used < cap，
			 * 但再拍一张必然还是 87+87 > 100，额度事实上已经用尽（实测踩过，
			 * 用例 agentloop_task_budget_image_exhaustion_is_terminal_and_stated
			 * 把它钉住了）。 */
			if (used + result.imageDataUrl.size() > cap) {
				note += "本任务的图片额度已经用尽，**再调用也不会成功**；"
					"请立即改用文字结论或换一个新任务。";
			}
		}
		note += "工具本身已执行完毕（副作用已发生）。请改用更小的步子推进任务，"
			"或直接基于已有信息给出结论。";
		/* AG-21：降级说明**自己也计入预算**——否则「反复超限」可以往历史里无界
		 * 灌说明文本（每条约 300 字节 x 轮数 x 每轮调用数）。计入之后第一次降级
		 * 就把额度顶满，后续调用走同一分支且文案不变（幂等，不会雪崩）。 */
		taskToolResultBytes_ += note.size();
		result.text = std::move(note);
		result.imageDataUrl.clear();
	} else {
		taskToolResultBytes_ += result.text.size();
		taskImageBytes_ += result.imageDataUrl.size();
	}

	if (opts_.verbose) {
		/* 2026-09-22: 转义工具输出中的日志标记，防止干扰日志解析 */
		std::string shown = escapeLogMarkers(result.text);
		if (!result.imageDataUrl.empty()) {
			shown += "\n[返回图片 " + std::to_string(result.imageDataUrl.size())
				+ " 字节，已注入下一轮对话]";
		}
		log("[工具] " + call.name + "\n" + indentLines(shown, 2));
	}

	ChatMessage tool;
	tool.role = "tool";
	tool.content = result.text;
	tool.toolCallId = call.id;
	messages_.push_back(std::move(tool));
	return result.imageDataUrl;
}

/* 执行：一轮带工具调用的模型回合，按协议顺序写历史——
 * assistant(tool_calls) → 每个 tool 的回传（tool_call_id 一一对应）→ 图片消息。
 * 图片必须等**全部** tool 消息回传之后再注入：OpenAI 协议要求 assistant 的
 * tool_calls 之后紧跟对应的 tool 消息，中间插 user 消息会被判 400；tool 消息
 * 本身又只允许文本。工具参数解析失败折成 tool 回传文本，不中断本轮。 */
void AgentSession::executeToolRound(ChatResponse& resp, int turn) {
	/* 个别流式/兼容实现会漏 tool_call id：这里合成占位 id，
	 * 保证 assistant.tool_calls 与后续 tool.tool_call_id 一一对上 */
	std::vector<ToolCall>& calls = resp.toolCalls;
	for (std::size_t i = 0; i < calls.size(); ++i) {
		if (calls[i].id.empty()) {
			calls[i].id = "call_" + std::to_string(turn) + "_" + std::to_string(i);
		}
	}

	/* 单轮派发上限（审计第 4 轮）：assistant 消息里**只保留**前
	 * maxCallsPerRound 条调用，多余的折叠成一条 tool 回传说明——不派发它们
	 * 是安全底线（不给硬件一次性下发失控数量动作），但必须如实告知模型
	 * 已经被截断，否则模型以为全部执行了。折叠进 assistant.toolCalls 的
	 * 只保留将执行的部分，保证与后续 tool 消息一一对应。 */
	std::size_t excess = 0;
	if (opts_.maxCallsPerRound > 0 && calls.size() > static_cast<std::size_t>(opts_.maxCallsPerRound)) {
		excess = calls.size() - static_cast<std::size_t>(opts_.maxCallsPerRound);
		calls.resize(static_cast<std::size_t>(opts_.maxCallsPerRound));
	}

	ChatMessage asst;
	asst.role = "assistant";
	asst.content = resp.content;
	asst.toolCalls = calls;
	asst.extras = resp.extras;	/* 同上：附加字段随历史逐轮回传 */
	messages_.push_back(std::move(asst));

	std::vector<std::string> images;
	for (const auto& c : calls) {
		std::string img = dispatchOneCall(c, turn);
		if (!img.empty()) { images.push_back(std::move(img)); }
	}
	if (excess > 0) {
		/* 截断说明必须如实告知模型（与 AG-4 同口径「不谎报状态」）。挂在
		 * 最后一条已派发调用的 id 上（tool 消息需引用已存在的 tool_call_id
		 * 协议才成立）；模型主要读的是说明文本本身。 */
		ChatMessage tool;
		tool.role = "tool";
		tool.content = "本轮模型返回了 " + std::to_string(calls.size() + excess)
			+ " 条工具调用，因超过单轮上限（" + std::to_string(opts_.maxCallsPerRound)
			+ " 条）只执行了前 " + std::to_string(calls.size())
			+ " 条；其余 " + std::to_string(excess)
			+ " 条未被派发，请基于已执行的结果决定下一步";
		tool.toolCallId = calls.empty() ? std::string("call_capped") : calls.back().id;
		messages_.push_back(std::move(tool));
	}
	if (!images.empty()) {
		ChatMessage img;
		img.role = "user";
		img.content = "以上工具返回的图片如下，请结合画面内容继续。";
		img.images = std::move(images);
		messages_.push_back(std::move(img));
	}
}

} // namespace agent
} // namespace bot
