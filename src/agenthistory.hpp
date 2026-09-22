#pragma once
/* agenthistory.hpp —— 会话历史的所有权与字节预算裁剪（重构 A1）
 *
 * 从 AgentSession（agentloop）拆出的第一套职责：历史消息的所有权、
 * 单条消息线上体积估算（messageWeight）与按任务块的整块裁剪（trim）。
 * 拆分前这三者与任务内预算、工具派发、轮次回滚交织在 agentloop.cpp；
 * 独立后可以离线单测裁剪语义（任务块边界、最新块无条件保留等）。
 *
 * 只依赖 agentproto 的 ChatMessage，不认识传输与工具层。
 */

#include <functional>
#include <string>
#include <vector>

#include "agentproto.hpp"

namespace bot {
namespace agent {

class HistoryManager {
public:
	/* 构造即写入 system 消息（messages_[0]，恒不被裁剪） */
	explicit HistoryManager(std::string systemPrompt);

	std::vector<ChatMessage>& messages() { return messages_; }
	const std::vector<ChatMessage>& messages() const { return messages_; }

	/* ---- 历史按字节预算裁剪（REPL 长会话）----
	 * 单位是「任务块」：一条任务消息（user 且不带图片——图片消息属于它前面
	 * 的任务块）直到下一条任务消息之前的全部消息。整块裁剪保证 assistant 的
	 * tool_calls 与其 tool 消息、以及夹在 tool 消息之后的图片 user 消息不被
	 * 拆散——半截序列发给服务端会被 400 拒绝。
	 * 老块连同其中的 base64 图片一起释放：图片是历史体积主项（一张照片数百 KB），
	 * 只按条数裁剪挡不住它们。
	 * budget == 0 = 不裁剪；裁剪发生且 verbose 时经 log 打一条人话说明
	 * （文案与拆分前逐字一致）。
	 * 口径（C4，2026-09-22）：budget 记账只覆盖可裁的任务块消息，**不含**
	 * system 提示词（messages_[0]，恒保留，另有 64KiB 独立上限）与工具 schema
	 * （注册期固定）；单条体积估算（messageWeight）是线上字节的下界。 */
	void trim(std::size_t budget, bool verbose,
		const std::function<void(const std::string&)>& log);

private:
	std::vector<ChatMessage> messages_;
};

} // namespace agent
} // namespace bot
