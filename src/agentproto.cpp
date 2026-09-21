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
		int maxTokens, double temperature, double topP, long timeoutSec,
		std::vector<const std::string*>* imageRefs) {
	Json req = JsonObject();
	req.put("model", JsonString(model));

	Json msgs = JsonArray();
	for (const auto& m : messages) { msgs.add(messageToJson(m, imageRefs)); }
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

Json messageToJson(const ChatMessage& msg, std::vector<const std::string*>* imageRefs) {
	Json m = JsonObject();
	m.put("role", JsonString(msg.role));

	/* 带图片：content 变成多模态数组（文本在前、图片在后）。
	 * provider 侧的展开由 Lua 驱动完成——这里只给结构化描述。
	 * B3：imageRefs 非空时图片以 {"__bot_img": N} 游标占位——字节本体不进
	 * 请求 Json（调用链上 jsonMergeInto 参数合并、push_lua_value 压栈都会
	 * 整树再拷），由 Lua 驱动在请求体组装期经宿主 C 函数 __bot_img(N) 取值。
	 * 游标编号与 imageRefs 的填充顺序一致（先遍历到的图片编号小）。 */
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
			if (imageRefs != nullptr) {
				imageRefs->push_back(&url);
				Json ph = JsonObject();
				ph.put("__bot_img", JsonInt(static_cast<long long>(imageRefs->size())));
				inner.put("url", ph);
			} else {
				inner.put("url", JsonString(url));
			}
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

	/* SAN-8③（2026-09-22）：被丢弃的调用要**如实报数**。
	 * 原实现丢完就当没事，整轮为空时抛「模型既没有返回文本也没有工具调用」——
	 * 而模型**确实**返回了工具调用（只是缺 function.name）。那句话是假的，
	 * 且片段给的是**规范化后**的表，看不到 provider 的原始形状。 */
	std::size_t droppedCalls = 0;

	auto tcs = res.get("tool_calls");
	if (tcs && tcs->isArr()) {
		for (const Json& v : tcs->arr) {
			if (!v.isObj()) { ++droppedCalls; continue; }
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
			/* A7：远端字段长度上限（判据单点见 agentproto.hpp 的 proto_limits）。
			 * id 超限**不能截断**——截断后它与 assistant.tool_calls 的配对就断了，
			 * 服务端 400，而这块历史已被认作"可原样发送"（同 AG-4 的失效形态）。
			 * 所以这里 fail-closed：整条响应拒绝，报错点名字段与实测字节数。 */
			if (tc.id.size() > proto_limits::kMaxToolCallIdBytes) {
				throw std::runtime_error("provider 返回的 tool_calls[].id 长 "
					+ std::to_string(tc.id.size()) + " 字节，超过上限 "
					+ std::to_string(proto_limits::kMaxToolCallIdBytes)
					+ " 字节（id 会被逐轮原样回传，且截断会破坏与 tool_calls 的配对，"
					  "故整条响应拒绝）");
			}
			if (tc.name.size() > proto_limits::kMaxToolNameBytes) {
				throw std::runtime_error("provider 返回的 tool_calls[].name 长 "
					+ std::to_string(tc.name.size()) + " 字节，超过上限 "
					+ std::to_string(proto_limits::kMaxToolNameBytes) + " 字节");
			}
			/* 无名字的调用无法派发；丢掉后若整轮为空，下面的兜底会报出来 */
			if (!tc.name.empty()) { out.toolCalls.push_back(std::move(tc)); }
			else { ++droppedCalls; }
		}
	}

	auto extras = res.get("extras");
	if (extras && extras->isObj()) { out.extras = *extras; }

	/* SAN-9：provider 用量原样透传（宿主不理解字段，只供审计取两个数） */
	auto usage = res.get("usage");
	if (usage && usage->isObj()) { out.usage = *usage; }

	if (out.content.empty() && out.toolCalls.empty()) {
		/* 丢弃数与驱动侧自报的计数一起说（驱动给的 dropped_bad_tool_calls 见
		 * modules/lib/llm_wire_openai.lua 的 SAN-6 段），让"模型说了但没派发"
		 * 这件事在报错里也看得见——不谎报状态。 */
		std::string note;
		std::size_t driverDropped = 0;
		if (out.extras.isObj()) {
			if (const Json* d = out.extras.find("dropped_bad_tool_calls");
			    d != nullptr && d->isInt()) {
				driverDropped = static_cast<std::size_t>(d->getInt());
			}
		}
		const std::size_t total = droppedCalls + driverDropped;
		if (total > 0) {
			note = "模型返回的 " + std::to_string(total)
				+ " 条工具调用全部无法派发（缺 function.name 或形态非法，已丢弃），"
				  "且没有文本内容";
		} else {
			note = "模型既没有返回文本也没有工具调用";
		}
		throw std::runtime_error(note + ": " + snippet(res.toString()));
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
