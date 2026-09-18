#include "agentproto.hpp"

#include <stdexcept>
#include <utility>

#include "jsonparse.hpp"
#include "util.hpp"

namespace bot {
namespace agent {

namespace {

/* 报错里附片段：长报文只在开头出问题，截断避免刷屏 */
std::string snippet(const std::string& s, std::size_t n = 400) {
	return s.size() <= n ? s : s.substr(0, n) + "...(截断)";
}

/* 规范化工具声明：**扁平**的 {name, description, parameters}。
 * 包成 provider 的 {"type":"function","function":{...}} 是 Lua 驱动的职责
 * （见 modules/lib/llm_wire_openai.lua）。此处若提前包一层，驱动会再包一层，
 * 结果 function 里空无一物 —— 模型一个工具都看不到，而工具循环表面上还在跑。 */
Json toolSpecJson(const ToolSpec& t) {
	Json fn = JsonObject();
	fn.put("name", JsonString(t.name));
	fn.put("description", JsonString(t.description));
	fn.put("parameters", t.parameters);
	return fn;
}

} // namespace

Json buildDriverRequest(const std::string& model,
		const std::vector<ChatMessage>& messages,
		const std::vector<ToolSpec>& tools,
		int maxTokens, double temperature, double topP, long timeoutSec) {
	Json req = JsonObject();
	req.put("model", JsonString(model));

	Json msgs = JsonArray();
	for (const auto& m : messages) { msgs.add(messageToJson(m)); }
	req.put("messages", msgs);

	if (!tools.empty()) {
		Json specs = JsonArray();
		for (const auto& t : tools) { specs.add(toolSpecJson(t)); }
		req.put("tools", specs);
		/* 协议侧的 tool_choice 由驱动生成（OpenAI 系为 "auto"） */
	}

	req.put("max_tokens", JsonInt(maxTokens));
	req.put("temperature", JsonFloat(temperature));
	req.put("top_p", JsonFloat(topP));
	req.put("timeout", JsonInt(timeoutSec));
	return req;
}

Json messageToJson(const ChatMessage& msg) {
	Json m = JsonObject();
	m.put("role", JsonString(msg.role));

	/* 带图片：content 变成多模态数组（文本在前、图片在后）。
	 * provider 侧的展开由 Lua 驱动完成——这里只给结构化描述。 */
	if (!msg.images.empty()) {
		Json parts = JsonArray();
		Json text = JsonObject();
		text.put("type", JsonString("text"));
		text.put("text", JsonString(msg.content));
		parts.add(text);
		for (const auto& url : msg.images) {
			Json img = JsonObject();
			img.put("type", JsonString("image_url"));
			Json inner = JsonObject();
			inner.put("url", JsonString(url));
			img.put("image_url", inner);
			parts.add(img);
		}
		m.put("content", parts);
		if (msg.extras.isObj() && msg.extras.objSize() > 0) { m.put("extras", msg.extras); }
		return m;
	}

	if (msg.role == "tool") {
		m.put("tool_call_id", JsonString(msg.toolCallId));
		m.put("content", JsonString(msg.content));
		return m;
	}

	if (!msg.toolCalls.empty()) {
		Json tcs = JsonArray();
		for (const auto& c : msg.toolCalls) {
			Json entry = JsonObject();
			entry.put("id", JsonString(c.id));
			entry.put("name", JsonString(c.name));
			entry.put("arguments", JsonString(c.arguments));
			tcs.add(entry);
		}
		m.put("tool_calls", tcs);
	}
	/* content 一律给字符串（空串表示无过渡语）：具体协议里「带 tool_calls 时
	 * content 该是 null 还是省略」由驱动决定，宿主不越界。 */
	m.put("content", JsonString(msg.content));

	if (msg.extras.isObj() && msg.extras.objSize() > 0) {
		m.put("extras", msg.extras);
	}
	return m;
}

ChatResponse parseDriverResponse(const Json& res) {
	if (!res.isObj()) {
		throw std::runtime_error("LLM 驱动返回的不是表: " + snippet(res.toString()));
	}

	auto result = res.get("result");
	std::string status = (result && result->isStr()) ? result->getValue() : "";
	if (status != "OK") {
		std::string msg = "LLM 驱动返回错误";
		auto m = res.get("message");
		if (m && m->isStr()) { msg += ": " + m->getValue(); }
		else { msg += ": " + snippet(res.toString()); }
		throw std::runtime_error(msg);
	}

	ChatResponse out;
	auto content = res.get("content");
	if (content && content->isStr()) { out.content = content->getValue(); }

	auto tcs = res.get("tool_calls");
	if (tcs && tcs->isArr()) {
		for (const Json& v : tcs->arr) {
			if (!v.isObj()) { continue; }
			ToolCall tc;
			auto id = v.get("id");
			if (id && id->isStr()) { tc.id = id->getValue(); }
			auto name = v.get("name");
			if (name && name->isStr()) { tc.name = name->getValue(); }
			auto args = v.get("arguments");
			if (args) {
				if (args->isStr()) { tc.arguments = args->getValue(); }
				else { tc.arguments = args->toString(); }
			}
			/* 无名字的调用无法派发；丢掉后若整轮为空，下面的兜底会报出来 */
			if (!tc.name.empty()) { out.toolCalls.push_back(std::move(tc)); }
		}
	}

	auto extras = res.get("extras");
	if (extras && extras->isObj()) { out.extras = *extras; }

	if (out.content.empty() && out.toolCalls.empty()) {
		throw std::runtime_error("模型既没有返回文本也没有工具调用: " + snippet(res.toString()));
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
