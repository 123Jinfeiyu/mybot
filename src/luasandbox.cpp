/* luasandbox.cpp —— Lua 沙箱收紧（N-17：自 luaend.cpp 拆分）
 *
 * 职责单一化：本文件只管「驱动进程内沙箱」——中和 os/io 危险入口、
 * dofile/loadfile 白名单化。Json↔Lua 桥接见 luabridge.cpp；
 * 伞头 luaend.hpp 的对外声明不变。
 */

#include "luaend.hpp"

#include <cstring>
#include <string>

#include "util.hpp"

namespace bot {

/* ---- '..' 路径分量判定（审计 #8）：逐段与 ".." 精确比较，
 * 不误伤含 '..' 子串的合法模块名 ---- */
static bool hasDotDotComponent(const char* path) {
	const char* p = path;
	while (*p != '\0') {
		const char* e = std::strchr(p, '/');
		std::size_t len = e != nullptr ? static_cast<std::size_t>(e - p) : std::strlen(p);
		if (len == 2 && p[0] == '.' && p[1] == '.') { return true; }
		if (e == nullptr) { break; }
		p = e + 1;
	}
	return false;
}

/* ---- 受限 dofile/loadfile 的路径校验（M3 收口）----
 * upvalue：1 = 白名单路径表（array 部分，每项一个允许前缀），2 = 原函数。
 * 白名单在 sandbox_lua 内由 BOT4CJ_MODULE_PATH（缺省 ./modules）逐段构建。
 * 放行条件：路径以某前缀开头且紧随 '/'（或恰好等于前缀本身），
 * 保证只能加载模块树内的文件，'..' 穿越依赖前缀逐字符比对天然失效。 */
static int restrictedLoader(lua_State* L) {
	luaL_checktype(L, 1, LUA_TSTRING);
	const char* path = lua_tostring(L, 1);

	lua_pushvalue(L, lua_upvalueindex(1));		/* 白名单表 */
	lua_pushnil(L);
	bool allowed = false;
	while (lua_next(L, -2) != 0) {
		const char* prefix = lua_tostring(L, -1);
		size_t len = prefix != nullptr ? lua_objlen(L, -1) : 0;
		if (prefix != nullptr
				&& strncmp(path, prefix, len) == 0
				&& (path[len] == '/' || path[len] == '\0')) {
			allowed = true;
		}
		lua_pop(L, 1);		/* 弹值留键 */
	}
	lua_pop(L, 1);			/* 弹白名单表 */

	if (!allowed) {
		return luaL_error(L, "sandbox: dofile/loadfile only allowed inside the module path (BOT4CJ_MODULE_PATH or ./modules), rejected: %s", path);
	}
	/* 前缀匹配挡不住 './modules/../CMakeLists.txt' 这类穿越（前缀后紧跟 '/'），
	 * 拒绝任何含 '..' 路径分量的请求；按分量精确比较而非子串匹配——审计 #8：
	 * strstr 会误拒含 '..' 子串的合法模块名（foo..bar.lua 之类）。 */
	if (hasDotDotComponent(path)) {
		return luaL_error(L, "sandbox: dofile/loadfile path must not contain '..' component (traversal rejected): %s", path);
	}

	lua_pushvalue(L, lua_upvalueindex(2));		/* 原函数 */
	lua_pushvalue(L, 1);						/* path */
	lua_call(L, 1, 1);
	return 1;
}

/* ---- 收紧 Lua 沙箱（issue #9 M1 可落地部分；M3 补 dofile/loadfile 收口）----
 * 保留 os.getenv/product、os.time|clock、require（驱动仅取 ffi/bit）；
 * 中和可直接破坏宿主的入口：os.execute/exit/remove/rename、io 五件套，
 * 并把全局 dofile/loadfile 限制到模块路径白名单内（驱动经 mpath.resolve
 * 加载 lib 目录下脚本的既有用法不变，'..' 穿越与绝对路径外读被拒绝）。
 * 各全局缺表时跳过对应组。 */
void sandbox_lua(lua_State* L) {
	struct Group { const char* tab; const char* keys[5]; int n; };
	static const Group groups[] = {
		{"os", {"execute", "exit", "remove", "rename", nullptr}, 4},
		{"io", {"popen", "open", "lines", "input", "output"}, 5},
	};
	for (const auto& g : groups) {
		lua_getglobal(L, g.tab);
		if (lua_type(L, -1) == LUA_TTABLE) {
			for (int i = 0; i < g.n; ++i) {
				lua_pushnil(L);
				lua_setfield(L, -2, g.keys[i]);	/* 表在 -2 */
			}
		}
		lua_pop(L, 1);
	}

	/* M3：dofile/loadfile 白名单化。白名单来自 BOT4CJ_MODULE_PATH（':' 分段，
	 * 去空），缺省 "./modules"——与 modules/lib/mpath.lua 的 paths() 同语义。 */
	lua_createtable(L, 0, 0);
	int n = 0;
	for (const std::string& p : splitModulePaths(modulePaths())) {
		lua_pushlstring(L, p.data(), p.size());
		lua_rawseti(L, -2, ++n);
	}
	/* 白名单为空不可能（缺省值兜底），仍防御：空表时 restrictedLoader 全拒 */

	/* pushcclosure 的 upvalue 编号：先压的值 = index 1。restrictedLoader 约定
	 * upvalue(1)=白名单表、(2)=原函数——故压入原函数后用 insert 把表拷贝
	 * 挪到其下方，再 pushcclosure 恰好消耗这两个值。 */
	lua_getglobal(L, "dofile");
	lua_pushvalue(L, 1);		/* 白名单表拷贝 */
	lua_insert(L, -2);			/* 顺序调整为 [表, 原函数] */
	lua_pushcclosure(L, restrictedLoader, 2);
	lua_setglobal(L, "dofile");

	lua_getglobal(L, "loadfile");
	lua_pushvalue(L, 1);
	lua_insert(L, -2);
	lua_pushcclosure(L, restrictedLoader, 2);
	lua_setglobal(L, "loadfile");

	lua_pop(L, 1);				/* 弹出白名单表，栈深复原 */
}

} // namespace bot
