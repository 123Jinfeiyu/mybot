/* test_luaend.cpp —— Lua↔Json 边界（call_lua / lua_value_to_json / 深度上限）
 *
 * luaend.cpp 是宿主与 55 个驱动之间唯一的进出通道，此前只被 E2E 间接覆盖，
 * 一条深度契约都不曾直接测过——于是两类缺陷都能静默存活：
 *   C-9   驱动返回自引用表（t.self = t 这类笔误）→ 无界递归 → 裸 SIGSEGV
 *   泄漏  若在转换处把异常抛出去，call_lua 末尾的 lua_settop 与调用方
 *         freeImpl 里的 lua_close 都会被跳过（状态机脏 + 泄漏）
 * 因此这里的断言不只是「报错形态」，还包括「抛出后栈深复原」与「同一
 * lua_State 仍可继续服务下一次调用」——后者正是就地 catch 成 ERR 的理由。
 */

#include <memory>
#include <stdexcept>
#include <string>

#include "harness.hpp"
#include "json.hpp"
#include "luaend.hpp"

using bot::Json;
using bot::JsonFloat;
using bot::JsonInt;
using bot::JsonObject;
using bot::JsonString;

namespace {

/* 独立 lua_State 的 RAII 持有者（本文件不碰沙箱，只开标准库） */
class State {
	lua_State* L_;

public:
	State() : L_(luaL_newstate()) {
		if (L_ == nullptr) { throw std::runtime_error("luaL_newstate failed"); }
		luaL_openlibs(L_);
	}
	~State() { if (L_ != nullptr) { lua_close(L_); } }
	State(const State&) = delete;
	State& operator=(const State&) = delete;

	lua_State* get() const { return L_; }
	int top() const { return lua_gettop(L_); }

	void chunk(const std::string& src) {
		if (luaL_dostring(L_, src.c_str()) != LUA_OK) {
			std::string err = bot::luaErrorString(L_);
			lua_settop(L_, 0);
			throw std::runtime_error("test chunk did not load: " + err);
		}
		lua_settop(L_, 0);
	}
};

Json callParams() {
	Json p = JsonObject();
	p.put("mode", JsonString("call"));
	p.put("x", JsonInt(1));
	return p;
}

/* 造一个恰好 n 层嵌套的全局表：RAW = {next={next=…{leaf=7}}} */
void setNestedGlobal(State& s, const char* name, int n) {
	std::string src = std::string(name) + " = { leaf = 7 }\n";
	for (int i = 0; i < n; ++i) { src += name + std::string(" = { next = ") + name + " }\n"; }
	s.chunk(src);
	lua_getglobal(s.get(), name);			/* 留在栈顶供转换 */
}

bool isErr(const Json& res) {
	auto r = res.get("result");
	return r && r->isStr() && r->getValue() == "ERR";
}

} // namespace

/* ---------------- C-9：自引用返回表 ---------------- */

TH_TEST(call_lua_turns_a_self_referential_result_into_err_instead_of_crashing) {
	State s;
	s.chunk("function call(data)\n"
		"  local t = { result = \"OK\" }\n"
		"  t.self = t\n"
		"  return t\n"
		"end\n");

	int before = s.top();
	Json res = bot::call_lua(s.get(), callParams());

	TH_CHECK(isErr(res));
	auto m = res.get("message");
	TH_CHECK(m && m->isStr());
	TH_CHECK(m->getValue().find("nests deeper than") != std::string::npos);
	/* 修复前这里是无限递归 → 栈溢出，根本走不到断言 */
	TH_CHECK(s.top() == before);
}

/* 就地 catch 的全部意义：状态机还能用（异常外逃会跳过调用方的 lua_close） */
TH_TEST(the_same_state_keeps_serving_after_a_nesting_error) {
	State s;
	s.chunk("function call(data)\n"
		"  local t = { result = \"OK\" }\n"
		"  t.self = t\n"
		"  return t\n"
		"end\n");
	Json bad = bot::call_lua(s.get(), callParams());
	TH_CHECK(isErr(bad));

	s.chunk("function call(data) return { result = \"OK\", v0 = 2.5 } end\n");
	Json good = bot::call_lua(s.get(), callParams());
	TH_CHECK(!isErr(good));
	TH_CHECK(good.get("v0") && good.get("v0")->getFloat() == 2.5);
}

/* ---------------- 深度上限的边界 ---------------- */

TH_TEST(lua_value_to_json_accepts_nesting_up_to_the_limit) {
	State s;
	setNestedGlobal(s, "RAW", bot::kMaxLuaNesting);

	/* 逐层走下去，确认不是「浅拷贝蒙过检查」而是整条链真的转换成功。
	 * 按值下沉：optional 是每轮的局部量，取它的地址会悬垂。 */
	Json cur = bot::lua_value_to_json(s.get(), -1);
	lua_pop(s.get(), 1);
	for (int i = 0; i < bot::kMaxLuaNesting; ++i) {
		auto next = cur.get("next");
		TH_CHECK(next.has_value());
		if (!next.has_value()) { return; }
		cur = *next;
	}
	TH_CHECK(cur.get("leaf") && cur.get("leaf")->getInt() == 7);
}

TH_TEST(lua_value_to_json_rejects_one_level_past_the_limit) {
	State s;
	setNestedGlobal(s, "RAW", bot::kMaxLuaNesting + 1);
	TH_CHECK_THROWS_MSG(bot::LuaNestingTooDeep, "nests deeper than",
		bot::lua_value_to_json(s.get(), -1));
	lua_pop(s.get(), 1);
}

/* ---------------- 回归护栏：既有形态不受影响 ---------------- */

TH_TEST(flat_sensor_results_still_round_trip) {
	State s;
	s.chunk("function call(data) return { result = \"OK\", v0 = 41.5, count = 3, ok = true } end\n");
	Json res = bot::call_lua(s.get(), callParams());

	TH_CHECK(!isErr(res));
	TH_CHECK(res.get("v0") && res.get("v0")->getFloat() == 41.5);
	TH_CHECK(res.get("count") && res.get("count")->getInt() == 3);
	TH_CHECK(res.get("ok") && res.get("ok")->getBool());
	TH_CHECK(s.top() == 0);
}

/* 数组式返回（nn.lua 的 W[层] 这类）是合法结构，不得被深度上限误杀 */
TH_TEST(nested_arrays_survive_the_conversion) {
	State s;
	s.chunk("function call(data) return { result = \"OK\", w = { {1, 2}, {3, 4} } } end\n");
	Json res = bot::call_lua(s.get(), callParams());

	TH_CHECK(!isErr(res));
	auto w = res.get("w");
	TH_CHECK(w && w->isArr() && w->size() == 2);
	TH_CHECK(w && w->at(1).isArr() && w->at(1).at(0).getInt() == 3);
}

/* ---------------- L3：数字键转换去 UB ---------------- */

TH_TEST(oversized_number_keys_become_string_keys) {
	State s;
	/* Lua 表数字键可为超大数（t[1e300]=2）；NaN 键 LuaJIT 在赋值期即报
	 * "table index is NaN"，到不了转换层（isfinite 守卫是纵深防御）。
	 * 旧实现把键 static_cast<long long>——1e300 超界即未定义行为；
	 * 现与 C7 同路转字符串键保数据。 */
	s.chunk("T = {}\n"
		"T[1e300] = 2\n"
		"T[3] = 9\n");
	lua_getglobal(s.get(), "T");
	Json j = bot::lua_value_to_json(s.get(), -1);
	lua_pop(s.get(), 1);

	/* 混合键表整体转对象：超大数键原样进字符串键，整数键 3 并入为 "3" */
	TH_CHECK(j.isObj());
	TH_CHECK(j.get(std::to_string(1e300)) && j.get(std::to_string(1e300))->getInt() == 2);
	TH_CHECK(j.get("3") && j.get("3")->getInt() == 9);
}

/* ---------------- M3：dofile/loadfile 模块路径白名单 ---------------- */

/* 带沙箱的 State（与驱动/守护线程同路：openlibs + sandbox_lua） */
class SandboxState {
	lua_State* L_;

public:
	SandboxState() : L_(luaL_newstate()) {
		if (L_ == nullptr) { throw std::runtime_error("luaL_newstate failed"); }
		luaL_openlibs(L_);
		bot::sandbox_lua(L_);
	}
	~SandboxState() { if (L_ != nullptr) { lua_close(L_); } }
	SandboxState(const SandboxState&) = delete;
	SandboxState& operator=(const SandboxState&) = delete;

	lua_State* get() const { return L_; }

	/* 执行一段代码，失败时返回错误信息（不抛） */
	std::string run(const std::string& src) {
		if (luaL_dostring(L_, src.c_str()) == LUA_OK) {
			lua_settop(L_, 0);
			return "";
		}
		std::string err = bot::luaErrorString(L_);
		lua_settop(L_, 0);
		return err;
	}
};

/* 白名单内加载放行（以仓库真实 modules 树为准；CMake 工作目录即仓库根） */
TH_TEST(sandbox_dofile_allows_module_path) {
	SandboxState s;
	std::string err = s.run("dofile('./modules/lib/print_table.lua')");
	TH_CHECK(err.empty());
}

/* 白名单外绝对路径被拒，且错误信息带 sandbox 标记可诊断 */
TH_TEST(sandbox_dofile_rejects_outside_path) {
	SandboxState s;
	std::string err = s.run("dofile('/etc/hostname')");
	TH_CHECK(!err.empty());
	TH_CHECK(err.find("sandbox") != std::string::npos);
}

/* '..' 穿越被拒：路径字符串在白名单段之后紧跟 '..'，前缀 + '/' 规则不放行 */
TH_TEST(sandbox_dofile_rejects_dotdot_traversal) {
	SandboxState s;
	std::string err = s.run("dofile('./modules/../CMakeLists.txt')");
	TH_CHECK(!err.empty());
	TH_CHECK(err.find("sandbox") != std::string::npos);
}

/* loadfile 同规（mpath.lua 的存在性探测走同一条受限入口） */
TH_TEST(sandbox_loadfile_same_rule) {
	SandboxState s;
	std::string okErr = s.run("assert(loadfile('./modules/lib/mpath.lua'))");
	if (!okErr.empty()) { std::printf("[dbg] loadfile in-module err: %s\n", okErr.c_str()); }
	TH_CHECK(okErr.empty());
	TH_CHECK(!s.run("assert(loadfile('/etc/passwd'))").empty());
}

/* 被拒后状态机仍可用（错误走 lua_error，不污染后续调用） */
TH_TEST(sandbox_rejection_leaves_state_usable) {
	SandboxState s;
	(void)s.run("dofile('/etc/hostname')");
	std::string err = s.run("assert(1 + 1 == 2)");
	TH_CHECK(err.empty());
}
