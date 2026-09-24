#include "agenthistory.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "agentaudit.hpp"	/* fingerprint64 / auditTimestamp（指纹与时间戳单点） */
#include "jsonparse.hpp"	/* parseJson */

namespace bot {
namespace agent {

/* ---- 会话持久化（批次 1，2026-09-23）----
 * 历史所有权与按任务块的整块裁剪（HistoryManager::trim）已随 C++ REPL 迁
 * Rust（agent/src/history.rs 的 message_weight + trim 是其孪生实现）删除。 */

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

std::string historyProtocolViolation(const std::vector<ChatMessage>& history) {
	/* 已声明的 id：按出现顺序收集，同时记下是被哪条 assistant 消息声明的 */
	std::vector<std::string> declared;
	for (std::size_t i = 0; i < history.size(); ++i) {
		const ChatMessage& m = history[i];
		if (m.role != "assistant") { continue; }
		for (const ToolCall& c : m.toolCalls) {
			if (c.id.empty()) {
				return "messages[" + std::to_string(i)
					+ "]: assistant 声明了一条 id 为空的工具调用（服务端 400）";
			}
			declared.push_back(c.id);
		}
	}

	std::vector<std::string> answered;
	for (std::size_t i = 0; i < history.size(); ++i) {
		const ChatMessage& m = history[i];
		if (m.role != "tool") { continue; }
		if (m.toolCallId.empty()) {
			return "messages[" + std::to_string(i) + "]: tool 消息缺少 tool_call_id（服务端 400）";
		}
		if (std::find(declared.begin(), declared.end(), m.toolCallId) == declared.end()) {
			return "messages[" + std::to_string(i) + "]: tool_call_id '" + m.toolCallId
				+ "' 不匹配任何 assistant 声明的工具调用（孤儿 tool 消息）";
		}
		if (std::find(answered.begin(), answered.end(), m.toolCallId) != answered.end()) {
			return "messages[" + std::to_string(i) + "]: tool_call_id '" + m.toolCallId
				+ "' 被回答了不止一次（服务端 400 answered more than once）";
		}
		answered.push_back(m.toolCallId);
	}

	for (const std::string& id : declared) {
		if (std::find(answered.begin(), answered.end(), id) == answered.end()) {
			return "assistant 声明的工具调用 '" + id + "' 没有得到任何 tool 回传（服务端 400）";
		}
	}
	return "";
}

} // namespace agent
} // namespace bot
