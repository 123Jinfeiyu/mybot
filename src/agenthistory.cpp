#include "agenthistory.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "agentaudit.hpp"	/* fingerprint64 / auditTimestamp（指纹与时间戳单点） */
#include "jsonparse.hpp"	/* parseJson */

namespace bot {
namespace agent {

namespace {

/* 估算一条消息的线上体积（下界，C4 口径）。不计：role 字符串、JSON 语法开销
 * （键名/引号/分隔符）、非 ASCII 的 \uXXXX 转义膨胀（CJK 约 2×、emoji 至 3×——
 * 转义行为取决于传输层序列化器，宿主侧不下断言）；正文/调用参数/图片/extras
 * 占绝对大头，作为裁剪判据方向正确、量级够用。extras 当前只装
 * reasoning_content 一类文本，整体串行化计入。
 * 另注意（C4）：trim 的预算记账**不含** messages_[0]（system 提示词）与工具
 * schema——前者恒不被裁剪、另有 64KiB 独立上限（agentprompt
 * kMaxSystemPromptBytes），后者注册期固定、逐请求重复发送，计入只会产生
 * 「历史没满就裁历史」的假超限。线上请求体真实总量 ≈ 预算记账 + system + schema。 */
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

std::size_t HistoryManager::accountedBytes() const {
	std::size_t n = 0;
	/* 与 trim 的记账一致：跳过 messages_[0]（system 提示词，恒保留） */
	for (std::size_t i = 1; i < messages_.size(); ++i) { n += messageWeight(messages_[i]); }
	return n;
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

/* ---- 会话持久化（批次 1，2026-09-23）---- */

std::string sessionImagePlaceholder(std::size_t totalImageBytes) {
	return "[图片 " + std::to_string(totalImageBytes) + " 字节，未随会话恢复]";
}

std::string sessionToolsFingerprint(const std::vector<ToolSpec>& specs) {
	std::string joined;
	for (const ToolSpec& s : specs) {
		if (!joined.empty()) { joined += ","; }
		joined += s.name;
	}
	return fingerprint64(joined);
}

std::string encodeSessionMessage(const ChatMessage& m) {
	/* system 不落盘：恢复时由当前进程重建并经指纹核对，落盘反而会让
	 * 「提示词换了没有」失去单一事实来源（会话头里的 systemFp） */
	if (m.role == "system") { return ""; }
	Json line = JsonObject();
	line.put("role", JsonString(m.role));
	if (!m.images.empty()) {
		/* 图片写入期排除：字节不落盘，落盘占位文本（恢复后模型据此知道
		 * 画面不在，与「图片默认不恢复」的操作者口径一致） */
		std::size_t total = 0;
		for (const std::string& img : m.images) { total += img.size(); }
		line.put("content", JsonString(sessionImagePlaceholder(total)));
		line.put("images_omitted", JsonInt(static_cast<std::int64_t>(m.images.size())));
	} else {
		line.put("content", JsonString(m.content));
	}
	if (m.role == "assistant") {
		if (!m.toolCalls.empty()) {
			Json calls = JsonArray();
			for (const ToolCall& c : m.toolCalls) {
				Json call = JsonObject();
				call.put("id", JsonString(c.id));
				call.put("name", JsonString(c.name));
				call.put("arguments", JsonString(c.arguments));
				calls.add(call);
			}
			line.put("tool_calls", calls);
		}
		/* provider 附加字段（DeepSeek 的 reasoning_content 等）随历史逐轮回传，
		 * 恢复后若丢失会话照样 400——必须原样落盘、原样读回 */
		if (m.extras.isObj() && m.extras.objSize() > 0) { line.put("extras", m.extras); }
	}
	if (m.role == "tool") {
		line.put("tool_call_id", JsonString(m.toolCallId));
	}
	return line.toString();
}

SessionFileWriter::~SessionFileWriter() {
	if (file_ != nullptr) { std::fclose(file_); }
}

bool SessionFileWriter::open(const std::string& path, bool append, std::string& err) {
	if (file_ != nullptr) {
		err = "会话文件已打开: " + path;
		return false;
	}
	/* append=true 只在恢复路径用：文件已在 buildAgentConfig 的存在性校验里
	 * 确认过，这里打不开是运行期 IO 错误，照实报。 */
	std::FILE* f = std::fopen(path.c_str(), append ? "a" : "w");
	if (f == nullptr) {
		err = "无法打开会话文件 '" + path + "'（errno=" + std::to_string(errno)
			+ "）；请检查目录是否存在、是否有写权限";
		return false;
	}
	file_ = f;
	return true;
}

void SessionFileWriter::writeHeader(const SessionHead& head) {
	if (file_ == nullptr || broken_) { return; }
	Json line = JsonObject();
	line.put("v", JsonInt(head.version));
	line.put("type", JsonString("session_head"));
	if (!head.sessionId.empty()) { line.put("session", JsonString(head.sessionId)); }
	line.put("robot", JsonString(head.robot));
	line.put("tools_fp", JsonString(head.toolsFp));
	line.put("system_fp", JsonString(head.systemFp));
	line.put("created", JsonString(auditTimestamp()));
	const std::string text = line.toString();
	if (std::fwrite(text.data(), 1, text.size(), file_) != text.size()
		|| std::fputc('\n', file_) == EOF
		|| std::fflush(file_) != 0) {
		broken_ = true;
	}
}

bool SessionFileWriter::writeClearMarker() {
	if (file_ == nullptr || broken_) { return false; }
	static const char kMarker[] = "{\"type\":\"session_clear\"}";
	if (std::fwrite(kMarker, 1, sizeof(kMarker) - 1, file_) != sizeof(kMarker) - 1
		|| std::fputc('\n', file_) == EOF
		|| std::fflush(file_) != 0) {
		broken_ = true;
		return false;
	}
	return true;
}

bool SessionFileWriter::appendMessage(const ChatMessage& m) {
	if (file_ == nullptr || broken_) { return false; }
	const std::string text = encodeSessionMessage(m);
	if (text.empty()) { return true; }	/* system：调用方不该给，给了也不写 */
	if (std::fwrite(text.data(), 1, text.size(), file_) != text.size()
		|| std::fputc('\n', file_) == EOF
		|| std::fflush(file_) != 0) {
		/* 磁盘满/权限变化这类运行期故障：置 broken 并返回 false，由调用方
		 * 响亮告知并停用——静默丢历史正是本项目要避免的形态 */
		broken_ = true;
		return false;
	}
	return true;
}

namespace {

/* 单行 JSON → ChatMessage。只核对单行形态（role 合法、必备字段在），
 * 行与行之间的协议自洽由调用方以 historyProtocolViolation 复核。 */
bool decodeSessionMessage(const Json& v, ChatMessage& out, std::string& e) {
	if (!v.isObj()) { e = "不是 JSON 对象"; return false; }
	const Json* role = v.find("role");
	if (role == nullptr || !role->isStr()) { e = "缺 role 或不是字符串"; return false; }
	out.role = role->getValue();
	if (out.role != "user" && out.role != "assistant" && out.role != "tool") {
		e = "未知 role '" + out.role + "'"; return false;
	}
	const Json* content = v.find("content");
	if (content == nullptr || !content->isStr()) { e = "缺 content 或不是字符串"; return false; }
	out.content = content->getValue();
	if (out.role == "tool") {
		const Json* id = v.find("tool_call_id");
		if (id == nullptr || !id->isStr()) { e = "tool 消息缺 tool_call_id"; return false; }
		out.toolCallId = id->getValue();
	}
	if (out.role == "assistant") {
		if (const Json* calls = v.find("tool_calls")) {
			if (!calls->isArr()) { e = "tool_calls 不是数组"; return false; }
			for (std::size_t i = 0; i < calls->size(); ++i) {
				const Json& c = calls->at(i);
				if (!c.isObj()) { e = "tool_calls[" + std::to_string(i) + "] 不是对象"; return false; }
				ToolCall call;
				const Json* cid = c.find("id");
				const Json* cname = c.find("name");
				const Json* cargs = c.find("arguments");
				if (cid == nullptr || !cid->isStr() || cname == nullptr || !cname->isStr()
					|| cargs == nullptr || !cargs->isStr()) {
					e = "tool_calls[" + std::to_string(i) + "] 缺 id/name/arguments";
					return false;
				}
				call.id = cid->getValue();
				call.name = cname->getValue();
				call.arguments = cargs->getValue();
				out.toolCalls.push_back(std::move(call));
			}
		}
		/* provider 附加字段原样读回（未过任何判据——与宿主对 provider 的
		 * 既有待遇一致：extras 本来就是「不理解、只带回」的透传通道） */
		if (const Json* extras = v.find("extras")) {
			if (!extras->isObj()) { e = "extras 不是对象"; return false; }
			out.extras = *extras;
		}
	}
	/* images_omitted 只作信息记录，读回时无需重建（图片本来就不恢复） */
	return true;
}

} // namespace

bool loadSessionFile(const std::string& path, LoadedSession& out,
		std::vector<std::string>& warnings, std::string& err) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "无法读取会话文件 '" + path + "'（不存在或没有读权限）";
		return false;
	}
	std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	bool headSeen = false;
	std::size_t lineNo = 0;
	std::size_t pos = 0;
	while (pos <= all.size()) {
		std::size_t nl = all.find('\n', pos);
		const bool last = nl == std::string::npos;
		const std::string lineText = last ? all.substr(pos) : all.substr(pos, nl - pos);
		++lineNo;
		pos = last ? all.size() + 1 : nl + 1;
		if (lineText.empty() && last) { break; }
		if (lineText.empty()) { continue; }

		Json parsed;
		try {
			parsed = parseJson(lineText);
		} catch (const std::exception& e) {
			/* 崩溃可能留下最后一行残缺：跳过并点名，不整份作废 */
			warnings.push_back("会话文件 '" + path + "' 第 " + std::to_string(lineNo)
				+ " 行不是合法 JSON（" + e.what() + "），已跳过该行");
			continue;
		}
		if (!parsed.isObj()) {
			warnings.push_back("会话文件 '" + path + "' 第 " + std::to_string(lineNo)
				+ " 行不是 JSON 对象，已跳过该行");
			continue;
		}
		/* /clear 分界标记：丢弃此前累积的消息（批次 1）。标记前若尚无会话头
		 * （理论不该出现），按损坏行处理——没有头整份都要拒绝。 */
		if (const Json* t = parsed.find("type");
			t != nullptr && t->isStr() && t->getValue() == "session_clear") {
			if (!headSeen) {
				warnings.push_back("会话文件 '" + path + "' 第 " + std::to_string(lineNo)
					+ " 行的 session_clear 出现在会话头之前，已跳过该行");
				continue;
			}
			out.messages.clear();
			continue;
		}
		if (!headSeen) {
			const Json* type = parsed.find("type");
			const Json* ver = parsed.find("v");
			if (type == nullptr || !type->isStr() || type->getValue() != "session_head"
				|| ver == nullptr || !ver->isInt()) {
				err = "会话文件 '" + path + "' 首行不是会话头（缺 v/type=session_head），"
					"指纹无从核对，拒绝恢复";
				return false;
			}
			if (ver->getInt() != 1) {
				err = "会话文件 '" + path + "' 版本为 " + std::to_string(ver->getInt())
					+ "（本程序只认 v=1），拒绝恢复";
				return false;
			}
			out.head.version = 1;
			if (const Json* s = parsed.find("session"); s != nullptr && s->isStr()) {
				out.head.sessionId = s->getValue();
			}
			const Json* robot = parsed.find("robot");
			const Json* toolsFp = parsed.find("tools_fp");
			const Json* systemFp = parsed.find("system_fp");
			if (robot == nullptr || !robot->isStr() || toolsFp == nullptr || !toolsFp->isStr()
				|| systemFp == nullptr || !systemFp->isStr()) {
				err = "会话文件 '" + path + "' 会话头缺 robot/tools_fp/system_fp，拒绝恢复";
				return false;
			}
			out.head.robot = robot->getValue();
			out.head.toolsFp = toolsFp->getValue();
			out.head.systemFp = systemFp->getValue();
			headSeen = true;
			continue;
		}
		ChatMessage m;
		std::string e;
		if (!decodeSessionMessage(parsed, m, e)) {
			warnings.push_back("会话文件 '" + path + "' 第 " + std::to_string(lineNo)
				+ " 行不是合法消息行（" + e + "），已跳过该行");
			continue;
		}
		out.messages.push_back(std::move(m));
	}
	if (!headSeen) {
		err = "会话文件 '" + path + "' 为空或缺会话头，无会话可恢复";
		return false;
	}
	return true;
}

std::string sessionHeadMismatch(const SessionHead& expected, const SessionHead& found) {
	if (expected.robot != found.robot) {
		return "结构脚本不同（会话头是 '" + found.robot + "'，当前是 '" + expected.robot
			+ "'）；旧上下文与当前机器人对不上号";
	}
	if (expected.toolsFp != found.toolsFp) {
		return "工具集不同（会话头的工具集指纹 " + found.toolsFp + "，当前 "
			+ expected.toolsFp + "）；工具调用历史对当前会话不再适用";
	}
	if (expected.systemFp != found.systemFp) {
		return "系统提示词不同（提示词包或脚本清单变了）；旧上下文基于另一份系统提示词";
	}
	return "";
}

std::string sessionPermsWarning(const std::string& path) {
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { return ""; }
	if ((st.st_mode & (S_IRWXG | S_IRWXO)) == 0) { return ""; }
	return "会话文件 '" + path + "' 权限宽于 600（含完整对话内容，同等敏感），"
		"建议 chmod 600 " + path;
}

} // namespace agent
} // namespace bot
