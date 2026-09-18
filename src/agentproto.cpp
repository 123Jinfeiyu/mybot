#include "agentproto.hpp"

#include <stdexcept>
#include <utility>

#include "jsonparse.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

/* 报错里附响应片段：长报文只在开头出问题，截断避免刷屏 */
std::string snippet(const std::string& s, std::size_t n = 400) {
	return s.size() <= n ? s : s.substr(0, n) + "...(截断)";
}

} // namespace

Json buildChatRequest(const std::string& model,
		const std::vector<ChatMessage>& messages,
		const std::vector<ToolSpec>& tools,
		int maxTokens, double temperature, double topP) {
	Json body = JsonObject();
	body.put("model", JsonString(model));

	Json msgs = JsonArray();
	for (const auto& m : messages) { msgs.add(messageToJson(m)); }
	body.put("messages", msgs);

	if (!tools.empty()) {
		Json specs = JsonArray();
		for (const auto& t : tools) {
			Json fn = JsonObject();
			fn.put("name", JsonString(t.name));
			fn.put("description", JsonString(t.description));
			fn.put("parameters", t.parameters);
			Json entry = JsonObject();
			entry.put("type", JsonString("function"));
			entry.put("function", fn);
			specs.add(entry);
		}
		body.put("tools", specs);
		body.put("tool_choice", JsonString("auto"));
	}

	body.put("max_tokens", JsonInt(maxTokens));
	body.put("temperature", JsonFloat(temperature));
	body.put("top_p", JsonFloat(topP));
	body.put("stream", JsonBool(false));
	return body;
}

Json messageToJson(const ChatMessage& msg) {
	Json m = JsonObject();
	m.put("role", JsonString(msg.role));

	if (msg.role == "tool") {
		m.put("tool_call_id", JsonString(msg.toolCallId));
		m.put("content", JsonString(msg.content));
		return m;
	}

	if (!msg.toolCalls.empty()) {
		Json tcs = JsonArray();
		for (const auto& c : msg.toolCalls) {
			Json fn = JsonObject();
			fn.put("name", JsonString(c.name));
			fn.put("arguments", JsonString(c.arguments));
			Json entry = JsonObject();
			entry.put("id", JsonString(c.id));
			entry.put("type", JsonString("function"));
			entry.put("function", fn);
			tcs.add(entry);
		}
		m.put("tool_calls", tcs);
		/* 带工具调用且无过渡语时 content 给 null：个别兼容实现把空串
		 * 当成「有文本」而拒收 tool_calls 组合 */
		if (msg.content.empty()) { m.put("content", JsonNull()); }
		else { m.put("content", JsonString(msg.content)); }
		return m;
	}

	m.put("content", JsonString(msg.content));
	return m;
}

ChatResponse parseChatResponse(const std::string& body) {
	if (trim(body).empty()) {
		throw std::runtime_error("模型响应为空");
	}

	Json root = parseJson(body);
	if (!root.isObj()) {
		throw std::runtime_error("模型响应不是 JSON 对象: " + snippet(body));
	}

	auto err = root.get("error");
	if (err && !err->isNull()) {
		throw std::runtime_error("API 错误: " + err->toString());
	}

	auto choices = root.get("choices");
	if (!choices || !choices->isArr() || choices->arr.empty()) {
		throw std::runtime_error("响应中没有 choices: " + snippet(body));
	}

	auto msgOpt = choices->at(0).get("message");
	if (!msgOpt || !msgOpt->isObj()) {
		throw std::runtime_error("choices[0] 缺少 message: " + snippet(body));
	}
	const Json& msg = *msgOpt;

	ChatResponse out;
	auto content = msg.get("content");
	if (content && content->isStr()) { out.content = content->getValue(); }

	auto tcs = msg.get("tool_calls");
	if (tcs && tcs->isArr()) {
		for (const Json& v : tcs->arr) {
			if (!v.isObj()) { continue; }
			ToolCall c;
			auto id = v.get("id");
			if (id && id->isStr()) { c.id = id->getValue(); }
			auto fn = v.get("function");
			if (fn && fn->isObj()) {
				auto n = fn->get("name");
				if (n && n->isStr()) { c.name = n->getValue(); }
				auto a = fn->get("arguments");
				if (a) {
					if (a->isStr()) { c.arguments = a->getValue(); }
					else {
						/* 少数兼容实现直接回对象而非字符串：统一成本层的字符串形态 */
						c.arguments = a->toString();
					}
				}
			}
			/* 无名字的调用无法派发；丢掉比让循环拿到未知工具更干净
			 * （丢掉后若整轮为空，下面的兜底会把原始报文抛出来） */
			if (!c.name.empty()) { out.toolCalls.push_back(std::move(c)); }
		}
	}

	if (out.content.empty() && out.toolCalls.empty()) {
		throw std::runtime_error("模型既没有返回文本也没有工具调用: " + snippet(body));
	}
	return out;
}

Json parseToolArguments(const std::string& arguments) {
	std::string t = trim(arguments);
	if (t.empty()) { return JsonObject(); }
	Json v = parseJson(t);
	if (!v.isObj()) {
		throw IllegalArgumentException("工具参数不是 JSON 对象: " + snippet(arguments, 200));
	}
	return v;
}

} // namespace agent
} // namespace bot
