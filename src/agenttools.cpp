#include "agenttools.hpp"

/* run_command 的子进程实现已拆至 agent_subprocess（超时/进程组终止/
 * 输出截断/环境清洗），本 TU 只做工具装配。 */
#include "agent_subprocess.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "actionsource.hpp"
#include "hardware.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace bot {
namespace agent {

namespace {

/* ---- 参数取值 ---- */

std::string requireString(const Json& args, const std::string& key) {
	if (!args.isObj()) { throw IllegalArgumentException("工具参数必须是 JSON 对象"); }
	auto v = args.get(key);
	if (!v || !v->isStr() || v->getValue().empty()) {
		throw IllegalArgumentException("缺少字符串参数 " + key);
	}
	return v->getValue();
}

int optionalInt(const Json& args, const std::string& key, int fallback) {
	auto v = args.get(key);
	/* 键缺失或显式 null 都算「未提供」：模型常给可选字段填 null，
	 * 这与参数类型写错是两回事，不应混为一谈。 */
	if (!v || v->isNull()) { return fallback; }
	if (v->isInt()) {
		long long i = v->getInt();
		/* 整数路径同样收口：Json 的 int64 直接窄化到 int 会静默回绕 */
		if (i < std::numeric_limits<int>::min() || i > std::numeric_limits<int>::max()) {
			throw IllegalArgumentException("参数 " + key + " 超出整数范围");
		}
		return static_cast<int>(i);
	}
	if (v->isFloat()) {
		double d = v->getFloat();
		/* P1-1 防线（2026-09-19 评估）：模型返回的 "timeout_seconds": 1e30 这类值
		 * 直接 static_cast<int> 是 float-cast-overflow UB（非有限同理）。与
		 * luabridge.cpp 的 int64 转换同一族判据，此处按 int 宽度取界。 */
		if (!doubleFitsInt(d)) {
			throw IllegalArgumentException("参数 " + key + " 数值超出范围或非有限");
		}
		return static_cast<int>(d);
	}
	/* 类型不符不再静默回落（原实现 return fallback）：与 requireString 同款严格。
	 * 静默用缺省值会让模型把「参数被忽略」误读成「参数已生效」，而工具报错会
	 * 折成文本回传，模型据此就能自我修正。 */
	throw IllegalArgumentException("参数 " + key + " 类型不符：需要整数");
}

/* ---- 文件读取（带上限，拒绝而非截断）---- */

/* 中危 #5 防线（2026-09-19 审计）：打开必须带 O_NOFOLLOW，拒绝符号链接。
 * 路径的词法校验（resolveActionScriptPath）只保证「请求的名字」在 scripts 目录
 * 之下，而 open/ifstream 会跟随软链——能写 scripts/ 的本地进程预置一条软链，
 * 就能让任意文件被当动作脚本执行，且其首行注释还会经 list_action_scripts 回显
 * 进模型上下文。这里用 open + O_NOFOLLOW 一步到底（不做 lstat 后再 open 的
 * check-then-use，那中间有 TOCTOU 窗口）。
 * 残余：O_NOFOLLOW 只看最后一段，路径中间的目录若是软链仍会被穿过——不过能写
 * scripts/ 的进程本就能直接放真脚本，这不构成增量能力；要彻底收紧得改 openat
 * 逐段打开，收益不抵复杂度。 */
/* 动作脚本读取（M2/B2-3 单点）：底层 FD 骨架收敛到 util::readFdNoFollowLimited，
 * 此处只配中文报错文案（grep 守门锚定原文不变）。 */
std::string readFileLimited(const std::string& path, std::size_t cap) {
	std::string data;
	switch (readFdNoFollowLimited(path, cap, data)) {
	case FdReadStatus::SYMLINK:
		throw IllegalArgumentException("拒绝符号链接: " + path);
	case FdReadStatus::OPEN_FAILED:
		throw IllegalArgumentException("无法打开文件 " + path);
	case FdReadStatus::READ_FAILED:
		throw IllegalArgumentException("读取文件失败: " + path);
	case FdReadStatus::TOO_LARGE:
		throw IllegalArgumentException("文件超过 " + std::to_string(cap) + " 字节上限: " + path);
	case FdReadStatus::TOTAL_TOO_LARGE:
	case FdReadStatus::OK:
		return data;
	}
	return data;
}

/* 动作脚本首行注释作为说明（"# 标准动作" → "标准动作"）。
 * 返回值必然进系统提示词 → 必须消毒（2026-09-21 审计 AG-1）：磁盘上的 .mt
 * 首注释带一个非法字节，第一次请求就会被网关 400 拒掉（实测 offset 3263），
 * 会话根本起不来。 */
std::string firstComment(const std::string& content) {
	std::istringstream ss(content);
	std::string line;
	for (int i = 0; i < 20 && std::getline(ss, line); ++i) {
		std::string t = trim(line);
		if (t.empty()) { continue; }
		if (t[0] == '#') { return sanitizeUtf8(trim(t.substr(1))); }
		break;	/* 首行非注释即认为无说明 */
	}
	return "";
}

/* ---- JSON Schema 小工具 ---- */

Json stringParam(const std::string& desc) {
	Json p = JsonObject();
	p.put("type", JsonString("string"));
	p.put("description", JsonString(desc));
	return p;
}

Json intParam(const std::string& desc) {
	Json p = JsonObject();
	p.put("type", JsonString("integer"));
	p.put("description", JsonString(desc));
	return p;
}

Json toolSchema(const std::vector<std::pair<std::string, Json>>& props,
		const std::vector<std::string>& required) {
	Json properties = JsonObject();
	for (const auto& [name, schema] : props) { properties.put(name, schema); }
	Json req = JsonArray();
	for (const auto& r : required) { req.add(JsonString(r)); }
	Json s = JsonObject();
	s.put("type", JsonString("object"));
	s.put("properties", properties);
	s.put("required", req);
	return s;
}

/* ---- 图片 data URL 的 MIME（AG-9 单点）----
 * 规则：**以驱动声明的 format 为准**，再用 base64 前几字节做一次自洽交叉校验。
 * 为什么两道：
 *   ① 只信 format → 驱动写错（或第三方驱动硬编码）就会发错标签，正是本缺陷的成因；
 *   ② 只信魔数 → 宿主就得自带一份格式表，与驱动的判据重复且必然漂移。
 * 所以宿主采用驱动的声明，同时对「声明与内容不符」fail-closed——那说明驱动有
 * bug，此时报错比把错标签发出去好（网关的 400 只会说「图片解析错误」）。
 * 支持的四种与 modules/lib/mjpeg_frame.lua 的 SUPPORTED_FORMATS 同集合。 */
std::string sniffImageFormat(const std::string& b64) {
	/* 判据最多需要前 12 字节；**能解多少解多少**——把最小长度写成 16 会把
	 * 合法的小图（9 字节 JPEG 编码后只有 12 个 base64 字符）误判成「不是图片」，
	 * 实测就是这么红的。每条魔数判据自己会检查 head 够不够长。 */
	if (b64.size() < 4) { return ""; }
	static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	auto val = [](char c) -> int {
		const char* p = std::strchr(kB64, c);
		return p == nullptr ? -1 : static_cast<int>(p - kB64);
	};
	std::string head;
	const std::size_t limit = std::min<std::size_t>(16, b64.size());
	for (std::size_t i = 0; i + 3 < limit; i += 4) {
		const int a = val(b64[i]), b = val(b64[i + 1]), c = val(b64[i + 2]), d = val(b64[i + 3]);
		if (a < 0 || b < 0 || c < 0 || d < 0) { return ""; }
		( void )0;
		head.push_back(static_cast<char>((a << 2) | (b >> 4)));
		head.push_back(static_cast<char>(((b & 0x0f) << 4) | (c >> 2)));
		head.push_back(static_cast<char>(((c & 0x03) << 6) | d));
	}
	const auto startsWith = [&head](const char* pfx, std::size_t n) {
		return head.size() >= n && head.compare(0, n, pfx, n) == 0;
	};
	if (head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xff
		&& static_cast<unsigned char>(head[1]) == 0xd8
		&& static_cast<unsigned char>(head[2]) == 0xff) {
		return "jpeg";
	}
	if (startsWith("\x89PNG\r\n\x1a\n", 8)) { return "png"; }
	if (startsWith("GIF87a", 6) || startsWith("GIF89a", 6)) { return "gif"; }
	if (startsWith("RIFF", 4) && head.size() >= 12 && head.compare(8, 4, "WEBP") == 0) {
		return "webp";
	}
	return "";
}

/* ---- 图片体积上限的两道判据（2026-09-21 第三轮审计 AG-19）----
 * `capture_image` 是宿主里唯一会搬运 MiB 级字符串的地方，而它此前把
 * 2 MiB 判据放在**拿到 base64 之后**：驱动仿真上限 16 MiB（base64 约 21 MB）
 * 意味着「一张注定被拒的图」要先走完 base64 编码、跨 Json/Lua 边界，
 * 再被拒——实测 10 MiB 的图让进程峰值到 219 MiB（见 .tmp/memaudit/）。
 * 现在按驱动已经给出的原始字节数**先判**，编码/跨界的代价直接省掉。
 *
 * 两道都要留：① 声明是不可信输入（驱动的 `bytes` 可能漏填或算错）；
 * ② 声明对不上时，base64 长度这道兜底必须命中。判据调用点因此**不允许**
 * 用 Json::getValue()——那个访问器按值返回整串，比一次长度就拷贝 2 MiB
 * （原实现 `b64->getValue().size() > kMaxImageBase64Bytes` 正是如此，
 * 同一段代码还另外拷了 5 次）。一律借用 Json 节点自己的 s。 */
std::string checkImageSize(const Json& res, std::size_t b64Bytes) {
	/* ① 原始字节数的声明（base64 = ceil(raw/3)*4 → raw 上限取 3/4 向下取整）*/
	if (const Json* raw = res.find("bytes"); raw != nullptr && raw->isInt()) {
		const long long declared = raw->getInt();
		const long long maxRaw = static_cast<long long>(kMaxImageBase64Bytes / 4) * 3;
		if (declared > maxRaw) {
			return "图像过大：" + std::to_string(declared) + " 字节原始数据，超过上限 "
				+ std::to_string(maxRaw) + " 字节（宿主按 base64 上限 "
				+ std::to_string(kMaxImageBase64Bytes)
				+ " 字节折算，未做 base64 编码即拒绝）；请降低分辨率或换小图"
				  "（大图会击穿会话历史预算）";
		}
	}
	/* ② base64 实际长度（声明缺失/偏低时的兜底；此时已编码，只能事后拒）*/
	if (b64Bytes > kMaxImageBase64Bytes) {
		return "图像过大：base64 " + std::to_string(b64Bytes) + " 字节，超过上限 "
			+ std::to_string(kMaxImageBase64Bytes)
			+ " 字节；请降低分辨率或换小图（大图会击穿会话历史预算）";
	}
	return "";
}

std::string dataUrlMimeFor(const Json& res, const std::string& b64) {
	auto fmt = res.get("format");
	if (!fmt || !fmt->isStr() || fmt->getValue().empty()) {
		throw std::runtime_error("摄像头驱动没有返回 format 字段（无法确定图片 MIME）");
	}
	const std::string declared = fmt->getValue();
	if (declared != "jpeg" && declared != "png" && declared != "gif" && declared != "webp") {
		throw std::runtime_error("摄像头驱动返回了不受支持的图片格式 '" + declared
			+ "'（仅支持 jpeg/png/gif/webp）");
	}
	const std::string actual = sniffImageFormat(b64);
	if (actual.empty()) {
		throw std::runtime_error("图片内容不是受支持的格式（JPEG/PNG/GIF/WebP 魔数都不匹配）");
	}
	if (actual != declared) {
		throw std::runtime_error("图片声明格式 '" + declared + "' 与内容实际格式 '"
			+ actual + "' 不符（驱动 bug；拒绝发出错标签的 data URL）");
	}
	/* 驱动给的是**格式名**（jpeg/png/…），data URL 要的是 **MIME**（image/…）：
	 * 两者在四个受支持格式上恰好一一对应（jpeg 的 MIME 是 image/jpeg，不是
	 * image/jpg），所以直接前缀 image/ 即可，不需要再维护一张映射表。 */
	return "image/" + declared;
}

/* 2.3：种类判定走虚 kindName（叶子类单点），此处只做展示文案映射 */
const char* kindOf(const MatrixVars_Hardware* hw) {
	if (hw == nullptr) { return "未知"; }
	std::string_view k = hw->kindName();
	if (k == "alias") { return "别名"; }
	if (k == "vars") { return "数值变量"; }
	if (k == "lua") { return "驱动"; }
	if (k == "daemon") { return "守护"; }
	return "未知";
}

} // namespace

bool AgentToolRegistry::contains(const std::string& name) const {
	for (const auto& t : tools_) {
		if (t.name == name) { return true; }
	}
	return false;
}

AgentToolResult AgentToolRegistry::execute(const std::string& name, const Json& args) const {
	for (const auto& t : tools_) {
		if (t.name != name) { continue; }
		try {
			return t.execute(args);
		} catch (const std::exception& e) {
			return {std::string("工具执行失败: ") + e.what(), ""};
		}
	}
	std::string avail;
	for (const auto& t : tools_) {
		if (!avail.empty()) { avail += ", "; }
		avail += t.name;
	}
	return {"未知工具 " + name + "。可用工具: " + avail, ""};
}

std::vector<ToolSpec> AgentToolRegistry::specs() const {
	std::vector<ToolSpec> out;
	out.reserve(tools_.size());
	for (const auto& t : tools_) { out.push_back(ToolSpec{t.name, t.description, t.parameters}); }
	return out;
}

std::string resolveActionScriptPath(const std::string& scriptsDir, const std::string& requested) {
	std::string s = trim(requested);
	if (s.empty()) { throw IllegalArgumentException("缺少 script 参数（动作脚本相对路径）"); }
	/* 段判据单点（M2/B2-1）：agenttools/mtinclude/luasandbox 共用 util::checkRelPath，
	 * 报错文案留在调用方——此处的枚举分支各自映射回本函数字面。 */
	switch (checkRelPath(s)) {
	case RelPathCheck::ABSOLUTE:
		throw IllegalArgumentException("script 必须是脚本目录下的相对路径，不接受绝对路径: " + s);
	case RelPathCheck::NUL:
		throw IllegalArgumentException("script 含非法字符（NUL）");
	case RelPathCheck::EMPTY_OR_DOT:
		throw IllegalArgumentException("script 路径含空段或 '.' : " + s);
	case RelPathCheck::DOTDOT:
		throw IllegalArgumentException("script 不允许越出脚本目录: " + s);
	default:
		break;
	}
	std::size_t slash = s.rfind('/');
	std::string last = (slash == std::string::npos) ? s : s.substr(slash + 1);
	if (last.size() < 3 || last.compare(last.size() - 3, 3, ".mt") != 0) {
		throw IllegalArgumentException("只允许执行 .mt 动作脚本: " + s);
	}
	std::string full = scriptsDir;
	while (full.size() > 1 && full.back() == '/') { full.pop_back(); }
	full += "/" + s;
	return full;
}

std::string describeActionScripts(const std::string& scriptsDir) {
	fs::path root(scriptsDir);
	std::error_code ec;
	if (!fs::is_directory(root, ec)) {
		return "(动作脚本目录不存在: " + scriptsDir + ")";
	}
	std::vector<std::string> rels;
	try {
		for (const auto& e :
		     fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
			std::error_code tec;
			if (!e.is_regular_file(tec)) { continue; }
			/* 软链一律不收（中危 #5）：脚本目录里放一条软链，就能把目录外的
			 * 任意文件拉进来当脚本执行，并让它的首注释经本清单回显给模型。 */
			if (e.is_symlink(tec)) { continue; }
			if (e.path().extension() != ".mt") { continue; }
			/* 纯词法求相对路径：fs::relative 内部会 weakly_canonical（跟随
			 * 软链），对软链目录项会算出带 `..` 的越界路径。 */
			fs::path rel = e.path().lexically_relative(root);
			if (rel.empty() || *rel.begin() == "..") { continue; }
			rels.push_back(rel.generic_string());
		}
	} catch (const fs::filesystem_error&) {
		/* 个别子目录不可读：已收集的照常输出，不因一处权限问题整段失败 */
	}
	std::sort(rels.begin(), rels.end());
	if (rels.empty()) { return "(脚本目录下没有 .mt 动作脚本: " + scriptsDir + ")"; }
	std::string out;
	for (const auto& rel : rels) {
		std::string desc;
		try {
			desc = firstComment(readFileLimited((root / rel).string(), 4096));
		} catch (const std::exception&) {
			desc.clear();
		}
		out += "  " + rel;
		if (!desc.empty()) { out += "  — " + desc; }
		out += "\n";
	}
	return out;
}

std::string describeHardware(RobotStruct& robot) {
	const auto& m = robot.registry().byName();
	if (m.empty()) { return "(没有注册任何硬件变量)"; }
	std::size_t width = 0;
	for (const auto& [name, hw] : m) {
		(void)hw;
		width = std::max(width, name.size());
	}
	std::string out = "已注册硬件变量（" + std::to_string(m.size()) + " 个）:\n";
	for (const auto& [name, hw] : m) {
		out += "  " + name + std::string(width - name.size() + 2, ' ') + kindOf(hw) + "\n";
	}
	return out;
}

/* ---- 各工具的装配函数（阶段 3 拆分）：makeAgentTools 只做组装 ---- */

static AgentTool makeListActionScriptsTool(const AgentToolContext& ctx) {
	return AgentTool{
		"list_action_scripts",
		"列出机器人可用的动作脚本（.mt 文件），返回相对路径与说明。"
		"需要让机器人做动作前先调用它，挑选与目标最匹配的脚本。",
		toolSchema({}, {}),
		[ctx](const Json&) -> AgentToolResult {
			return {describeActionScripts(ctx.scriptsDir), ""};
		}};
}

static AgentTool makeRunActionScriptTool(const AgentToolContext& ctx) {
	return AgentTool{
		"run_action_script",
		"在常驻的机器人上执行一个动作脚本（.mt），例如站立、前进、转向、举爪。"
		"脚本执行完毕后硬件保持上电常驻，可以接着执行下一个脚本。"
		"script 必须是 list_action_scripts 返回的相对路径。返回执行成功或报错原因；"
		"报错时机器人状态可能停在半途，请读清原因再决定下一步。",
		toolSchema({{"script", stringParam("动作脚本相对路径，如 std.mt 或 test/pwm.mt")}},
			{"script"}),
		[ctx](const Json& args) -> AgentToolResult {
			std::string req;
			try {
				if (ctx.robot == nullptr) { throw std::runtime_error("没有常驻机器人实例"); }
				req = requireString(args, "script");
				std::string path = resolveActionScriptPath(ctx.scriptsDir, req);
				/* @Include 基准目录 = scripts-dir 根：与本工具的相对路径语义
				 * 一致（include 名同样不得越出该树），多轮执行重复设置幂等 */
				ctx.robot->setActionIncludeBaseDir(ctx.scriptsDir);
				std::string content = readFileLimited(path, kMaxScriptBytes);
				if (trim(content).empty()) { throw IllegalArgumentException("脚本内容为空: " + path); }
				const std::size_t bytes = content.size();
				auto reader = std::make_unique<LimitedReader>(
					std::make_unique<MemoryReader>(std::move(content)),
					kMaxScriptBytes, "action script");
				ctx.robot->run(*reader);
				return {"已执行动作脚本 " + req + "（" + std::to_string(bytes) + " 字节）", ""};
			} catch (const std::exception& e) {
				return {"动作脚本执行失败 [" + req + "]: " + e.what(), ""};
			}
		}};
}

static AgentTool makeListHardwareTool(const AgentToolContext& ctx) {
	return AgentTool{
		"list_hardware",
		"列出当前机器人的硬件变量清单（结构脚本注册的驱动、别名、数值变量）。"
		"只读清单，不会让任何器件动作。",
		toolSchema({}, {}),
		[ctx](const Json&) -> AgentToolResult {
			std::string out;
			if (!ctx.structurePath.empty()) { out += "结构脚本: " + ctx.structurePath + "\n"; }
			if (ctx.robot == nullptr) {
				out += "(没有常驻机器人实例)";
				return {out, ""};
			}
			out += describeHardware(*ctx.robot);
			return {out, ""};
		}};
}

static AgentTool makeCaptureImageTool(const AgentToolContext& ctx) {
	return AgentTool{
		"capture_image",
		"拍摄一张机器人当前环境的照片。图片会直接进入你的上下文，你能看到画面内容；"
		"任务涉及观察环境、寻找物体、判断前方是否有障碍时，先调用它再做结论。",
		toolSchema({}, {}),
		[ctx](const Json&) -> AgentToolResult {
			Json params = JsonObject();
			/* 只在显式配置时才传 path/src：结构脚本若把同名列声明成元数据列，
			 * 再传同名参数会被 rejectReservedKeys 拒掉（结构脚本优先）。 */
			if (!ctx.cameraDevice.empty()) {
				params.put("path", JsonString(ctx.cameraDevice));
			}
			if (!ctx.cameraSrc.empty()) {
				params.put("src", JsonString(ctx.cameraSrc));
				/* src_root 把「允许读取的目录」一并交给驱动（P1-2 防线）：
				 * 驱动据此只放行该目录内的文件，任意路径读（/etc/shadow 之类）
				 * 被拒。目录取配置路径的真实所在目录（canonical 展开软链与
				 * 相对段，与驱动侧 realpath 判据一致）；路径尚不存在时退一步
				 * 解析到最深的已存在父目录，好让驱动报「打不开」而不是「未放行」。
				 * 两步都解析不出绝对路径时**不回落成 "."**——那等于放行整个当前
				 * 工作目录，比本意宽得多；这条判据是边界，宁可拒绝也不猜。 */
				std::error_code ec;
				fs::path real = fs::canonical(ctx.cameraSrc, ec);
				if (ec) { real = fs::weakly_canonical(ctx.cameraSrc, ec); }
				/* parent_path() 只在「有根路径却无根目录」的相对形态上抛
				 * filesystem_error，绝对路径不会——所以先判绝对再取父目录。 */
				fs::path parent = real.is_absolute() ? real.parent_path() : fs::path();
				if (ec || !parent.is_absolute()) {
					throw IllegalArgumentException("无法解析模拟图片来源所在目录: "
						+ ctx.cameraSrc);
				}
				params.put("src_root", JsonString(parent.string()));
			}
			Json res = ctx.camera->playQuiet(params);	/* ERR 契约在此抛出 */
			/* 借用而非拷贝（AG-19）：Json::getValue() 按值返回，一次拍照原本要
			 * 为同一串 base64 拷 6 次（本函数 5 处 + 体积判据 1 处）。 */
			const Json* b64Node = res.find("b64");
			if (b64Node == nullptr || !b64Node->isStr() || b64Node->s.empty()) {
				throw std::runtime_error("摄像头驱动没有返回图像数据（b64 为空）");
			}
			const std::string& b64 = b64Node->s;
			/* 体积判据（AG-19）：先用驱动声明的原始字节数判，再做 base64 编码；
			 * 声明对不上时由 base64 长度兜底。拒绝文案由 checkImageSize 单点产出。 */
			if (std::string sizeErr = checkImageSize(res, b64.size()); !sizeErr.empty()) {
				throw std::runtime_error(sizeErr);
			}
			std::string desc = "已拍摄照片";
			auto w = res.get("width");
			auto h = res.get("height");
			if (w && h && w->isInt() && h->isInt()) {
				desc += "（" + std::to_string(w->getInt()) + "x"
					+ std::to_string(h->getInt()) + "）";
			}
			auto bytes = res.get("bytes");
			if (bytes && bytes->isInt()) {
				desc += "，" + std::to_string(bytes->getInt()) + " 字节";
			}
			auto src = res.get("source");
			if (src && src->isStr() && src->getValue() == "file") {
				desc += "，来源：模拟图片";
			}
			/* MIME 取自**驱动返回的格式**（2026-09-21 审计 AG-9），不再硬编码
			 * image/jpeg：仿真来源可以是 PNG/GIF/WebP，硬编码会把「JPEG 标签 +
			 * PNG 字节」发给网关，严格解析器直接 400。驱动侧已按魔数判定并对
			 * 不认识的格式响亮拒绝，这里只需原样采用并做一次自洽交叉校验。 */
			std::string mime = dataUrlMimeFor(res, b64);
			return {desc + "。画面已加入你的上下文。",
				"data:" + mime + ";base64," + b64};
		}};
}

static AgentTool makeRunCommandTool(const AgentToolContext& ctx) {
	return AgentTool{
		"run_command",
		"在机器人主机上执行 shell 命令，返回退出码、标准输出与标准错误。"
		"适合查看系统状态、读取日志、检查设备节点；不要用它直接驱动硬件"
		"（驱动硬件请用 run_action_script）。命令有超时限制，输出超过 64KB 会截断。",
		toolSchema({{"command", stringParam("要执行的 shell 命令")},
			    {"timeout_seconds", intParam("超时秒数，默认 10，最大 60")}},
			{"command"}),
		[ctx](const Json& args) -> AgentToolResult {
			std::string cmd = requireString(args, "command");
			int timeoutSec = optionalInt(args, "timeout_seconds", ctx.commandTimeoutSec);
			if (timeoutSec < 1) { timeoutSec = 1; }
			if (timeoutSec > ctx.maxCommandTimeoutSec) { timeoutSec = ctx.maxCommandTimeoutSec; }
			try {
				return {formatRunResult(runShell(cmd, timeoutSec)), ""};
			} catch (const std::exception& e) {
				return {std::string("命令执行失败: ") + e.what(), ""};
			}
		}};
}

AgentToolRegistry makeAgentTools(const AgentToolContext& ctx) {
	AgentToolRegistry reg;
	reg.add(makeListActionScriptsTool(ctx));
	reg.add(makeRunActionScriptTool(ctx));
	reg.add(makeListHardwareTool(ctx));
	if (ctx.camera != nullptr) { reg.add(makeCaptureImageTool(ctx)); }
	if (ctx.allowShell) { reg.add(makeRunCommandTool(ctx)); }
	return reg;
}

} // namespace agent
} // namespace bot
