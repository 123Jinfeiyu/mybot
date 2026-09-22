#include "agenthistory.hpp"

namespace bot {
namespace agent {

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

HistoryManager::HistoryManager(std::string systemPrompt) {
	ChatMessage sys;
	sys.role = "system";
	sys.content = std::move(systemPrompt);
	messages_.push_back(std::move(sys));
}

void HistoryManager::trim(std::size_t budget, bool verbose,
		const std::function<void(const std::string&)>& log) {
	if (budget == 0 || messages_.size() <= 1) { return; }

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
	while (keep > 0 && acc + blockWeight[keep - 1] <= budget) {
		--keep;
		acc += blockWeight[keep];
	}
	if (keep == 0) { return; }	/* 全部塞得下，无事可做 */

	std::size_t dropped = starts[keep] - 1;	/* system 之后、最新保留块之前的条数 */
	messages_.erase(messages_.begin() + 1, messages_.begin() + starts[keep]);
	if (verbose && log) {
		log("[历史] 超出字节预算，已裁掉最早的 " + std::to_string(dropped)
			+ " 条消息（含其中的图片）；更早的任务上下文不再可见");
	}
}

} // namespace agent
} // namespace bot
