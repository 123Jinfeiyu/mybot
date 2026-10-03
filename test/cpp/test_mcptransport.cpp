/* test_mcptransport.cpp —— MCP 传输与认证域（帧编解码/凭证/探测/契约钉子）
 *
 * 自 test_mcpbackend.cpp 拆出（2026-10-01，行为保持），镜像 src/mcptransport.cpp
 * 的域边界：帧编码与严格 UTF-8、SO_PEERCRED、socket 路径占用探测（A-1）、
 * 空闲 deadline 合成（A-2）、帧上限/回执 schema 的 Rust 孪生契约钉子、
 * 首帧共享密钥三态（MCP-1）与 AuthFailGate。
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "agentmode.hpp"	/* AgentModeConfig（context.get 用例） */
#include "agenttools.hpp"
#include "jsonparse.hpp"
#include "mcpbackend.hpp"
#include "robot.hpp"
#include "util_except.hpp"
#include "util_str.hpp"
#include "util_utf8.hpp"
#include "fixtures.hpp"
#include "harness.hpp"
#include "mcp_fixtures.hpp"
#include "agenttools_fixtures.hpp"

using namespace bot;
using namespace bot::agent;

using mfx::makeBackend;

/* ---- 帧编码：4 字节 big-endian 长度 + 载荷（与 pymsg/gomsg/libmsg 字节兼容）---- */
TH_TEST(mcpbackend_frame_encode_is_4byte_be_plus_payload) {
	const std::string f = mcpEncodeFrame("abc");
	TH_CHECK(f.size() == 7);
	TH_CHECK(static_cast<unsigned char>(f[0]) == 0u);
	TH_CHECK(static_cast<unsigned char>(f[1]) == 0u);
	TH_CHECK(static_cast<unsigned char>(f[2]) == 0u);
	TH_CHECK(static_cast<unsigned char>(f[3]) == 3u);
	TH_CHECK(f.compare(4, 3, "abc") == 0);

	/* 空载荷合法（libmsg 口径）：只有 4 字节头，长度 0 */
	const std::string e = mcpEncodeFrame("");
	TH_CHECK(e.size() == kMcpHeaderSize);
	TH_CHECK(static_cast<unsigned char>(e[3]) == 0u);

	/* 300 = 0x0000012C：验证多字节 BE 编码正确 */
	const std::string big = mcpEncodeFrame(std::string(300, 'x'));
	TH_CHECK(big.size() == 304);
	TH_CHECK(static_cast<unsigned char>(big[2]) == 0x01u);
	TH_CHECK(static_cast<unsigned char>(big[3]) == 0x2Cu);
}

/* ---- 写侧 deadline（2026-10-01 批次 A / P1-3）----
 * 此前 sendFrame 无 deadline，靠 acceptLoop 的 SO_SNDTIMEO 兜底——而 setsockopt
 * 失败只告警，该路径上慢读端可无限挂住单线程后端。Rust 侧同类修复见 0333f1a。
 * 这里钉：对端不读 + 载荷远超 socket 缓冲 → deadline 内返回 false（不再无限
 * 挂住）；对端正常读 → 发满返回 true 且字节形态正确。 */
TH_TEST(mcpbackend_sendframe_deadline_unblocks_slow_reader) {
	int fds[2] = {-1, -1};
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		TH_SKIP("socketpair 不可用");
	}
	/* 对端不读：send 缓冲填满后 send() 阻塞——旧实现在这里永久挂住 */
	const std::string big(kMcpMaxFrameBytes, 'x');
	const auto t0 = std::chrono::steady_clock::now();
	TH_CHECK(!sendFrame(fds[0], big, 1));	/* 1 秒 deadline */
	const long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();
	TH_CHECK_MSG(elapsed < 5000, "慢读端挂住写侧: " + std::to_string(elapsed) + "ms");
	::close(fds[0]);
	::close(fds[1]);

	/* 对端正常读：小载荷发满，4 字节 BE 长度 + 载荷字节形态正确 */
	int ok[2] = {-1, -1};
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ok) != 0) {
		TH_SKIP("socketpair 不可用");
	}
	TH_CHECK(sendFrame(ok[0], "hello", kMcpSendTimeoutSec));
	char buf[16] = {0};
	TH_CHECK(::recv(ok[1], buf, sizeof(buf), 0) == 9);
	TH_CHECK(std::memcmp(buf + 4, "hello", 5) == 0);
	::close(ok[0]);
	::close(ok[1]);
}

/* ---- 严格 UTF-8 校验：拒 overlong / 代理区 / 截断 / 超范围 ---- */
TH_TEST(mcpbackend_valid_utf8_accepts_and_rejects) {
	TH_CHECK(isValidUtf8("hello"));			/* ASCII */
	TH_CHECK(isValidUtf8("\xe4\xb8\xad"));		/* 中（3 字节）*/
	TH_CHECK(isValidUtf8("\xf0\x9f\x98\x80"));	/* 😀（4 字节）*/
	TH_CHECK(isValidUtf8(""));			/* 空串合法 */

	TH_CHECK(!isValidUtf8("\xc0\x80"));		/* overlong 编码的 NUL */
	TH_CHECK(!isValidUtf8("\xed\xa0\x80"));	/* 代理区 D800 */
	TH_CHECK(!isValidUtf8("\xe4\xb8"));		/* 3 字节序列被截断 */
	TH_CHECK(!isValidUtf8("\x80"));		/* 续字节打头 */
	TH_CHECK(!isValidUtf8("\xf5\x80\x80\x80"));	/* > U+10FFFF */
}


/* ---- SO_PEERCRED：同进程 socketpair 匹配；非 socket fd 读取失败即拒 ---- */
TH_TEST(mcpbackend_peercred_matches_self_and_rejects_non_socket) {
	std::ostringstream log;

	int fds[2] = {-1, -1};
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		TH_SKIP("socketpair 不可用");
	}
	/* 同进程两端：uid/gid 必然一致 → 放行 */
	TH_CHECK(mcpCheckPeerCred(fds[0], log));
	::close(fds[0]);
	::close(fds[1]);

	/* 非 socket fd：getsockopt(SO_PEERCRED) 失败 → 拒（覆盖失败分支）*/
	const int fd = ::open("/dev/null", O_RDONLY);
	TH_CHECK(fd >= 0);
	TH_CHECK(!mcpCheckPeerCred(fd, log));
	::close(fd);
}

/* ---- A-1（2026-09-25 审计）：socket 路径占用探测四态 ----
 * 原实现无条件 unlink：误配同一路径的后启动者会静默偷走 socket 文件、先启动者
 * 永远不可达。探测必须分清「无人监听（可覆盖）」与「活实例（拒绝启动）」。 */
TH_TEST(mcpbackend_probe_socket_path_free_stale_busy_unusable) {
	fx::TempDir tmp("mcp-probe");
	std::string why;
	const std::string base = (tmp.path() / "probe").string();

	/* ① 路径不存在 → 空闲可绑定 */
	TH_CHECK(mcpProbeSocketPath(base, why) == McpSocketProbe::PathFree);
	TH_CHECK(why.empty());

	/* ② 陈旧 socket（bind 后关闭、无人监听）→ 可安全覆盖 */
	{
		const std::string stale = base + "-stale";
		const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
		TH_CHECK(s >= 0);
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", stale.c_str());
		TH_CHECK(::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
		::close(s);	/* 文件留盘、无监听者 → stale */
		TH_CHECK(mcpProbeSocketPath(stale, why) == McpSocketProbe::PathFree);
		TH_CHECK(why.empty());
	}

	/* ③ 活监听（bind + listen）→ 拒绝启动且点名成因 */
	{
		const std::string busy = base + "-busy";
		const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
		TH_CHECK(s >= 0);
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", busy.c_str());
		TH_CHECK(::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
		TH_CHECK(::listen(s, 1) == 0);
		const McpSocketProbe r = mcpProbeSocketPath(busy, why);
		TH_CHECK(r == McpSocketProbe::PathBusy);
		TH_CHECK_MSG(why.find("监听") != std::string::npos, "成因点名: " + why);
		::close(s);
	}

	/* ④ 非 socket 文件（普通文件）→ 不替操作者删除 */
	{
		const std::string plain = base + "-plain";
		std::ofstream f(plain, std::ios::binary);
		f << "keep me";
		f.close();
		const McpSocketProbe r = mcpProbeSocketPath(plain, why);
		TH_CHECK(r == McpSocketProbe::PathUnusable);
		TH_CHECK_MSG(why.find("socket") != std::string::npos, "成因点名: " + why);
	}
}

/* ---- A-2（2026-09-25 审计）：空闲 deadline 合成 ----
 * 取「墙钟终点」与「此刻 + 空闲预算」的较早者；都不设 = 不限（max）。 */
TH_TEST(mcpbackend_idle_deadline_composes_wall_clock_and_idle_budget) {
	using Clock = std::chrono::steady_clock;

	/* 都不设：恒 max（空闲不限，长生命周期语义不变） */
	{
		McpBackend b = makeBackend(AgentToolRegistry{});
		TH_CHECK(b.idleDeadline(0) == Clock::time_point::max());
		TH_CHECK(b.idleDeadline(-1) == Clock::time_point::max());
	}
	/* 只设空闲：≈ now + idle（在窗口内） */
	{
		McpBackend b = makeBackend(AgentToolRegistry{});
		const Clock::time_point before = Clock::now();
		const Clock::time_point d = b.idleDeadline(10);
		TH_CHECK(d > before);
		TH_CHECK(d <= Clock::now() + std::chrono::seconds(12));
	}
	/* 只设墙钟（1 小时）：≈ connStart + 1h，远小于 max */
	{
		McpBackend b = makeBackend(AgentToolRegistry{}, /*callBudget=*/0, /*wallClock=*/3600);
		const Clock::time_point d = b.idleDeadline(0);
		TH_CHECK(d != Clock::time_point::max());
		TH_CHECK(d > Clock::now() + std::chrono::seconds(3500));
	}
	/* 都设、空闲更早 → 取空闲那条 */
	{
		McpBackend b = makeBackend(AgentToolRegistry{}, /*callBudget=*/0, /*wallClock=*/3600);
		const Clock::time_point d = b.idleDeadline(5);
		TH_CHECK(d <= Clock::now() + std::chrono::seconds(7));
	}
	/* 都设、墙钟更早（1 秒预算）→ 取墙钟那条（不是 100 秒空闲） */
	{
		McpBackend b = makeBackend(AgentToolRegistry{}, /*callBudget=*/0, /*wallClock=*/1);
		const Clock::time_point d = b.idleDeadline(100);
		TH_CHECK(d < Clock::now() + std::chrono::seconds(2));
		TH_CHECK(d > Clock::now() - std::chrono::seconds(2));
	}
}

/* ==================== 跨语言契约钉子（2026-09-25）====================
 * 帧上限/帧头与 ToolResult 七字段是 C++ 后端与 Rust agent 各持一份的「同一契约
 * 的孪生」（两侧注释各自声明"改动必须双侧同步"），但同步此前只靠人记——漂移
 * 没有本侧运行期症状（Rust 发一个自认合法的 9 MiB 帧会被后端拒收；新增字段在
 * Rust 侧静默丢失），只能像 daemon_defaults_match_lua_source（test_robot.cpp）
 * 那样读源比对钉死。七字段的真相链：结构体成员（agenttools.hpp）→ JSON 键
 * （mcpbackend.cpp 的序列化点）→ Rust 字段（bridge.rs），camel↔snake 逐字段
 * 对应——下面手写的清单就是**规约本体**，任何一侧漂移都以它为基准红。 */

namespace {

#ifndef MYBOT_SOURCE_DIR
#define MYBOT_SOURCE_DIR "."
#endif

/* 读仓库内源文件（编译期锚定源码根，不依赖运行期 CWD） */
std::string readRepoSource(const std::string& rel) {
	const std::string path = std::string(MYBOT_SOURCE_DIR) + "/" + rel;
	std::ifstream f(path, std::ios::binary);
	if (!f) { throw std::runtime_error("契约钉子读不到源文件: " + path); }
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 求常量表达式的值：只支持非负整数字面量、'*'、'+' 与空白——覆盖
 * "8 * 1024 * 1024" 这类惯用形态足够。解析失败返回 false，用例红并带回原文，
 * 让改常量的人顺手适配本函数，而不是静默放过。 */
bool evalConstExpr(const std::string& expr, long long& out) {
	std::size_t i = 0;
	const auto skipWs = [&] {
		while (i < expr.size() && (expr[i] == ' ' || expr[i] == '\t'
				|| expr[i] == '\n' || expr[i] == '\r')) { ++i; }
	};
	const auto digits = [&](long long& v) {
		v = 0;
		const std::size_t begin = i;
		while (i < expr.size() && expr[i] >= '0' && expr[i] <= '9') {
			v = v * 10 + (expr[i] - '0');
			++i;
		}
		return i > begin;
	};
	long long sum = 0;
	skipWs();
	if (!digits(sum)) { return false; }		/* 首项 */
	for (;;) {
		skipWs();
		if (i < expr.size() && expr[i] == '*') {
			++i;
			long long factor = 0;
			skipWs();
			if (!digits(factor)) { return false; }
			sum *= factor;
			continue;
		}
		if (i < expr.size() && expr[i] == '+') {
			++i;
			long long term = 0;
			skipWs();
			if (!digits(term)) { return false; }
			sum += term;
			continue;
		}
		break;
	}
	skipWs();
	if (i != expr.size()) { return false; }
	out = sum;
	return true;
}

/* 取声明 "pub const NAME: T = RHS;" 的右侧原文（取声明点到下一个 ';'） */
std::string rustConstRhs(const std::string& src, const std::string& decl) {
	const std::size_t at = src.find(decl);
	if (at == std::string::npos) { return {}; }
	const std::size_t eq = src.find('=', at);
	const std::size_t semi = src.find(';', eq);
	if (eq == std::string::npos || semi == std::string::npos) { return {}; }
	return src.substr(eq + 1, semi - eq - 1);
}

/* 块内逐行抽取「行首 \tstd::string / \tbool 开头的成员名」——agenttools.hpp 的
 * AgentToolResult 数据成员形态（构造函数行与初始化列表行不匹配该形态） */
[[maybe_unused]] std::vector<std::string> cppMemberNames(const std::string& src,
		std::size_t begin, std::size_t end) {
	std::vector<std::string> names;
	std::size_t pos = begin;
	while (pos < end) {
		const std::size_t eol = src.find('\n', pos);
		const std::size_t lineEnd = (eol == std::string::npos || eol > end) ? end : eol;
		const std::string line = src.substr(pos, lineEnd - pos);
		for (const char* type : {"std::string ", "bool "}) {
			const std::string prefix = std::string("\t") + type;
			if (line.compare(0, prefix.size(), prefix) == 0) {
				std::size_t n = prefix.size();
				while (n < line.size() && (isalnum(static_cast<unsigned char>(line[n]))
						|| line[n] == '_')) { ++n; }
				names.push_back(line.substr(prefix.size(), n - prefix.size()));
			}
		}
		if (eol == std::string::npos) { break; }
		pos = eol + 1;
	}
	return names;
}

/* 块内逐行抽取 data.put("KEY" 的键名——mcpbackend.cpp 序列化点的形态 */
[[maybe_unused]] std::vector<std::string> jsonPutKeys(const std::string& src,
		std::size_t begin, std::size_t end) {
	std::vector<std::string> keys;
	const std::string pat = "data.put(\"";
	std::size_t at = begin;
	while ((at = src.find(pat, at)) != std::string::npos && at < end) {
		const std::size_t close = src.find('"', at + pat.size());
		if (close == std::string::npos || close > end) { break; }
		keys.push_back(src.substr(at + pat.size(), close - at - pat.size()));
		at = close;
	}
	return keys;
}

/* 块内逐行抽取 "pub <name>:" 的字段名——bridge.rs 的 Rust struct 形态
 * （字段行带缩进，先跳行首空白再匹配） */
[[maybe_unused]] std::vector<std::string> rustFieldNames(const std::string& src,
		std::size_t begin, std::size_t end) {
	std::vector<std::string> names;
	std::size_t pos = begin;
	while (pos < end) {
		const std::size_t eol = src.find('\n', pos);
		const std::size_t lineEnd = (eol == std::string::npos || eol > end) ? end : eol;
		const std::string line = src.substr(pos, lineEnd - pos);
		const std::string prefix = "pub ";
		const std::size_t first = line.find_first_not_of(" \t");
		if (first != std::string::npos
				&& line.compare(first, prefix.size(), prefix) == 0) {
			const std::size_t colon = line.find(':', first + prefix.size());
			if (colon != std::string::npos) {
				names.push_back(line.substr(first + prefix.size(),
					colon - first - prefix.size()));
			}
		}
		if (eol == std::string::npos) { break; }
		pos = eol + 1;
	}
	return names;
}

/* 集合相等（名单顺序不是线格式的一部分；数量与名字是） */
[[maybe_unused]] bool sameNameSet(const std::vector<std::string>& a, const std::vector<std::string>& b) {
	if (a.size() != b.size()) { return false; }
	std::multiset<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
	return sa == sb;
}

/* joinNames 不再本地手写：直接用生产单点 bot::joinNames（util_str.hpp，
 * 2026-10-01 收敛），测试顺带钉住它的 ", " 缺省分隔符行为。 */

} // namespace

/* ---- 帧上限/帧头三常量：kMcpMaxFrameBytes ↔ kJsonMaxBytes ↔ frame.rs 孪生 ---- */
TH_TEST(mcpbackend_frame_limit_matches_rust_twin) {
	/* 后端帧上限与 JSON 解析器上限同源（mcpbackend.hpp 注释承诺的「对齐」） */
	TH_CHECK_MSG(kMcpMaxFrameBytes == kJsonMaxBytes,
		"kMcpMaxFrameBytes 应与 jsonparse 的 kJsonMaxBytes 相等（8 MiB 对齐承诺）");

	const std::string frameRs = readRepoSource("agent/src/frame.rs");
	const std::string maxRhs = rustConstRhs(frameRs, "pub const MAX_FRAME_BYTES");
	TH_CHECK_MSG(!maxRhs.empty(), "frame.rs 找不到 pub const MAX_FRAME_BYTES 声明");
	long long rustMax = 0;
	TH_CHECK_MSG(evalConstExpr(maxRhs, rustMax) && rustMax == static_cast<long long>(kMcpMaxFrameBytes),
		"frame.rs 的 MAX_FRAME_BYTES（'" + maxRhs + "' → " + std::to_string(rustMax)
		+ "）应与 kMcpMaxFrameBytes（" + std::to_string(kMcpMaxFrameBytes) + "）相等"
		+ "——帧上限是跨进程契约，改一侧必须同步另一侧");

	const std::string hdrRhs = rustConstRhs(frameRs, "pub const HEADER_LEN");
	TH_CHECK_MSG(!hdrRhs.empty(), "frame.rs 找不到 pub const HEADER_LEN 声明");
	long long rustHeader = 0;
	TH_CHECK_MSG(evalConstExpr(hdrRhs, rustHeader) && rustHeader == static_cast<long long>(kMcpHeaderSize),
		"frame.rs 的 HEADER_LEN（'" + hdrRhs + "' → " + std::to_string(rustHeader)
		+ "）应与 kMcpHeaderSize（" + std::to_string(kMcpHeaderSize) + "）相等");
}

/* ---- ToolResult 七字段：C++ 后端与 Rust protocol.rs 的行为契约 ---- */
TH_TEST(mcpbackend_toolresult_schema_matches_rust_outcome) {
	/* 不再读取生产源码猜测序列化实现；直接验证线上响应的字段集合。
	 * Rust protocol.rs 的 serde 测试验证同一组 camelCase wire names。 */
	AgentToolRegistry reg;
	reg.add(mfx::echoTool());
	McpBackend b = mfx::makeBackend(std::move(reg));
	const Json r = parseJson(b.handleFrame(
		R"({"_route":"tool.call","name":"echo","arguments":{}})"));
	TH_CHECK(mfx::respOk(r));
	const Json* data = mfx::respData(r);
	TH_CHECK(data != nullptr);
	const std::set<std::string> expected = {
		"text", "imageDataUrl", "ok", "error", "executed", "paramError", "summary",
	};
	std::set<std::string> actual;
	if (data != nullptr) {
		for (const auto& item : data->items()) { actual.insert(item.first); }
	}
	TH_CHECK_MSG(actual == expected, "tool.call 回执字段集合与跨进程协议不一致");
}

/* ---- 首帧共享密钥（MCP-1，2026-09-29 审计 P2-1）----
 * 纯协议件 mcpCheckAuthFrame 的三态判据：正确密钥 Ok；错误密钥/缺 token 字段/
 * token 非字符串/非法 JSON/顶层非对象 Failed；合法 JSON 对象但 _route 不是 auth
 * （旧客户端直发业务请求）NotAuthFrame——serveConnection 据此区分报错文案。 */
TH_TEST(mcpbackend_auth_frame_accepts_matching_token) {
	using bot::agent::McpAuthOutcome;
	const std::string payload = R"({"_route":"auth","token":"secret-1"})";
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1", payload) == McpAuthOutcome::Ok);
	/* 参数顺序无关：serde_json 按键取值 */
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"token":"secret-1","_route":"auth"})") == McpAuthOutcome::Ok);
}

TH_TEST(mcpbackend_auth_frame_rejects_mismatch_and_malformed) {
	using bot::agent::McpAuthOutcome;
	/* 错误密钥：长度相同（防止「比长度短路」的假实现混过）与长度不同两类 */
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"_route":"auth","token":"secret-2"})") == McpAuthOutcome::Failed);
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"_route":"auth","token":"s"})") == McpAuthOutcome::Failed);
	/* 注：expected="" 对 token="" 是合法匹配（Ok），不在本用例——不可达路径：
	 * captureMcpAuthToken 对空值拒绝启动，serveConnection 只在 authToken
	 * 非空时才强制首帧认证；纯函数不重复上游不变式。 */
	/* token 字段缺失 / 非字符串 */
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"_route":"auth"})") == McpAuthOutcome::Failed);
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"_route":"auth","token":123})") == McpAuthOutcome::Failed);
	/* 非法 JSON / 顶层非对象（与 handleFrame 的固定串口径一致：不回显输入） */
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1", "not json")
		== McpAuthOutcome::Failed);
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1", "[1,2]")
		== McpAuthOutcome::Failed);
}

TH_TEST(mcpbackend_auth_frame_distinguishes_non_auth_first_frame) {
	using bot::agent::McpAuthOutcome;
	/* 旧客户端形态：首帧直接是业务请求（合法 JSON 对象、_route 非 auth） */
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"_route":"tool.list"})") == McpAuthOutcome::NotAuthFrame);
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1",
			R"({"_route":"ping"})") == McpAuthOutcome::NotAuthFrame);
	/* _route 缺失或非字符串同样落 NotAuthFrame（有 _route 判据才有区分意义） */
	TH_CHECK(bot::agent::mcpCheckAuthFrame("secret-1", "{}")
		== McpAuthOutcome::NotAuthFrame);
}

/* ---- 首帧认证失败门（2026-09-30 审计 P1：与 TCP 口令 authMaxFails=5 同口径）----
 * serveConnection/acceptLoop 是匿名命名空间静态函数，单测触不到；判据本体
 * AuthFailGate 是纯结构（2026-10-01 收敛到 util_cli.hpp 与 TCP 侧共用），
 * 这里钉住语义：只数认证失败、通过才归零、第 5 次到顶触发停止。哑对端不动
 * 计数由 ConnAuth::Neutral 保证（类型层面已区分，不在此重复）。 */
TH_TEST(mcpbackend_auth_fail_gate_counts_consecutive_rejections) {
	bot::AuthFailGate g;
	TH_CHECK(g.fails == 0);
	/* 前 4 次失败：仍在窗口内，不触发 */
	for (int i = 1; i < g.max; ++i) {
		TH_CHECK(!g.recordRejected());
		TH_CHECK(g.fails == i);
	}
	/* 第 5 次到顶：返回 true（acceptLoop 据此停止服务） */
	TH_CHECK(g.recordRejected());
	TH_CHECK(g.fails == g.max);

	/* 通过归零：爆破计数是「连续」语义，一次成功认证重新开窗 */
	g.recordPassed();
	TH_CHECK(g.fails == 0);
	TH_CHECK(!g.recordRejected());
	TH_CHECK(g.fails == 1);

	/* 交错失败与通过永远到不了顶：合法 agent 的偶发手滑不会锁死后端 */
	bot::AuthFailGate interleave;
	for (int i = 0; i < 100; ++i) {
		(void)interleave.recordRejected();
		interleave.recordPassed();
	}
	TH_CHECK(interleave.fails == 0);
}
