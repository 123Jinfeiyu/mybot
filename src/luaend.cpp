#include "luaend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

#include "util.hpp"

namespace bot {

/* ---- 读取 Lua 栈顶错误对象为字符串 ----
 * lua_tolstring 对非 string/number 返回空指针——先判型；
 * string/number 原样转串，其余返回类型说明（issue #5 H5）。 */
std::string luaErrorString(lua_State* L) {
	int t = lua_type(L, -1);
	if (t == LUA_TSTRING || t == LUA_TNUMBER) {
		if (const char* s = lua_tolstring(L, -1, nullptr)) { return s; }
		return "<error tostring failed>";
	}
	return "<non-string error object, Lua type " + std::to_string(t) + ">";
}

/* ---- 探测全局变量类型（getglobal + type + pop，栈深不变）---- */
int luaGlobalType(lua_State* L, const char* name) {
	lua_getglobal(L, name);
	int tp = lua_type(L, -1);
	lua_pop(L, 1);
	return tp;
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
	 * 显式拒绝任何含 '..' 分量的路径；白名单前缀本身不含 '..'，全路径检查即可。 */
	if (std::strstr(path, "..") != nullptr) {
		return luaL_error(L, "sandbox: dofile/loadfile path must not contain '..' (traversal rejected): %s", path);
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
	std::string modulePath;
	if (const char* env = std::getenv("BOT4CJ_MODULE_PATH")) { modulePath = env; }
	if (modulePath.empty()) { modulePath = "./modules"; }
	int n = 0;
	std::size_t b = 0;
	while (b <= modulePath.size()) {
		std::size_t e = modulePath.find(':', b);
		std::string p = (e == std::string::npos)
			? modulePath.substr(b) : modulePath.substr(b, e - b);
		if (e == std::string::npos) { b = modulePath.size() + 1; }
		else { b = e + 1; }
		if (p.empty()) { continue; }
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

/* ---- 将 Json 值递归压入 Lua 栈（数组/对象转 Lua 表，H-03）---- */
void push_lua_value(lua_State* L, const Json& val) {
	if (val.isStr()) {
		lua_pushstring(L, val.s.c_str());
	} else if (val.isInt()) {
		lua_pushinteger(L, val.getInt());
	} else if (val.isFloat()) {
		lua_pushnumber(L, val.getFloat());
	} else if (val.isBool()) {
		lua_pushboolean(L, val.getBool() ? 1 : 0);
	} else if (val.isNull()) {
		lua_pushnil(L);
	} else if (val.isArr()) {
		lua_createtable(L, static_cast<int>(val.size()), 0);
		for (std::size_t i = 0; i < val.size(); ++i) {
			push_lua_value(L, val.at(i));
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
	} else if (val.isObj()) {
		lua_createtable(L, 0, 0);
		for (const auto& kv : val.items()) {
			push_lua_value(L, kv.second);
			lua_setfield(L, -2, kv.first.c_str());
		}
	} else {
		logD([] { return "push_lua_value: unsupported JsonValue type, pushed nil"; });
		lua_pushnil(L);
	}
}

/* ---- 读取 Lua 栈上 idx 处的值并转换为 Json（递归）；表转 JsonArray/JsonObject ----
 * 调用前后栈深不变。
 * C-9 防线：每层嵌套表压一个 C++ 栈帧，只有碰到标量才终止——驱动返回自引用表
 * （t.self = t，最常见的笔误形态）即无限递归直到裸 SIGSEGV，无环但极深的表同理。
 * 用 depth 计数在 kMaxLuaNesting 处抛 LuaNestingTooDeep，由 call_lua 就地转 ERR。
 * 检查放在进入表分支的最开头（本层还没压任何东西），抛出时栈不受本层污染。 */
/* double → int64 的合法值域（与键路径 L3 防线同一判据）：LLONG_MAX 的
 * double 表示向上舍入为 2^63，用其比较会放行溢出值，故直接以 ±2^63 为界。 */
constexpr double kMinIntAsDouble = -9223372036854775808.0;	/* -2^63 */
constexpr double kMaxIntAsDouble = 9223372036854775808.0;	/* 2^63 */

Json lua_value_to_json(lua_State* L, int idx, int depth) {
	int t = lua_type(L, idx);
	if (t == LUA_TNUMBER) {
		double d = lua_tonumber(L, idx);
		/* 审计 H-1 防线（键路径有 L3 判据、值路径曾缺失）：NaN/inf/超出
		 * int64 的 double 做 static_cast<long long> 是未定义行为（UBSan
		 * float-cast-overflow 实证）。非有限或超界直接走浮点分支，与
		 * json.cpp 的序列化契约一致（非有限输出 null）。 */
		if (std::isfinite(d) && d >= kMinIntAsDouble && d < kMaxIntAsDouble) {
			long long i = static_cast<long long>(d);
			return (static_cast<double>(i) == d) ? JsonInt(i) : JsonFloat(d);
		}
		return JsonFloat(d);
	}
	if (t == LUA_TBOOLEAN) {
		return JsonBool(lua_toboolean(L, idx) != 0);
	}
	if (t == LUA_TNIL) {
		return JsonNull();
	}
	if (t == LUA_TSTRING) {
		if (const char* s = lua_tolstring(L, idx, nullptr)) { return JsonString(s); }
		return JsonNull();
	}
	if (t == LUA_TTABLE) {
		if (depth > kMaxLuaNesting) { throw LuaNestingTooDeep(kMaxLuaNesting); }
		std::vector<long long> arrKeys;
		std::map<long long, Json> arrVals;
		Json obj = JsonObject();
		lua_pushvalue(L, idx);			/* 表副本到栈顶 */
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			int keyType = lua_type(L, -2);
			if (keyType == LUA_TNUMBER) {
				double kd = lua_tonumber(L, -2);
				/* L3 防线：NaN / 超出 int64 范围的键在 static_cast<long long> 时是
				 * 未定义行为。先验后与 C7 非整键同路转字符串键保留数据。
				 * 值域常量与值路径共用（见文件级 kMin/MaxIntAsDouble 注释）。 */
				if (!std::isfinite(kd) || kd >= kMaxIntAsDouble || kd < kMinIntAsDouble) {
					Json vj = lua_value_to_json(L, -1, depth + 1);
					obj.put(std::to_string(kd), vj);
				} else {
					/* C7 防线：非整数数字键（如 3.5）截断会静默丢数据——转字符串键保留 */
					long long k = static_cast<long long>(kd);
					if (static_cast<double>(k) != kd) {
						Json vj = lua_value_to_json(L, -1, depth + 1);
						obj.put(std::to_string(kd), vj);
					} else {
						Json vj = lua_value_to_json(L, -1, depth + 1);
						arrVals[k] = vj;
						arrKeys.push_back(k);
					}
				}
			} else if (keyType == LUA_TSTRING) {
				if (const char* ks = lua_tolstring(L, -2, nullptr)) {
					Json vj = lua_value_to_json(L, -1, depth + 1);
					obj.put(ks, vj);
				}
			}
			lua_pop(L, 1);			/* 弹出值，保留键供下一次 Lua_next */
		}
		lua_pop(L, 1);				/* 弹出表副本 */
		if (!arrKeys.empty()) {
			/* 必须排序拷贝后按其迭代（同原实现对拷贝排序） */
			std::sort(arrKeys.begin(), arrKeys.end());
			if (obj.objSize() > 0) {
				/* 混合键表：整体转对象（数字键转字符串键），不丢字段 */
				for (long long k : arrKeys) { obj.put(std::to_string(k), arrVals[k]); }
				return obj;
			}
			Json arr = JsonArray();
			for (long long k : arrKeys) { arr.add(arrVals[k]); }
			return arr;
		}
		if (obj.objSize() > 0) { return obj; }
		return JsonArray();			/* 空表 → 空数组 */
	}
	logD([t] { return "lua_value_to_json: unsupported Lua type " + std::to_string(t) + ", returned null"; });
	return JsonNull();
}

/* ---- 调用 Lua 脚本的全局 mode 函数（build/call/destroy/read）----
 * 每次调用后重置栈指针（恢复调用前栈深）。pcall 失败显式带 ERR 契约。 */
Json call_lua(lua_State* L, const Json& json) {
	const Json& obj = json.asObject();
	Json ret = JsonObject();

	int top = lua_gettop(L);

	/* C4 防线：mode 缺失/非字符串时清晰报错，而非 bad_optional_access 裸异常 */
	auto modeField = obj.get("mode");
	if (!modeField || !modeField->isStr()) {
		throw IllegalArgumentException("call_lua: 'mode' field missing or not a string"
			" (expected build/call/destroy/read)");
	}
	std::string mode = modeField->getValue();
	lua_getglobal(L, mode.c_str());

	/* 创建 Lua 表，装入全部字段 */
	lua_createtable(L, 0, 0);
	for (const auto& kv : obj.items()) {
		push_lua_value(L, kv.second);
		lua_setfield(L, -2, kv.first.c_str());
	}

	/* C3 防线：pcall 成功但返回 0 个值时，栈顶是调用前的旧栈顶——
	 * 以进入本函数时的栈深 top 为基准算返回值个数，nret > 0 才检查
	 * 返回值（否则会把无关栈上值当返回表遍历）。 */
	int status = lua_pcall(L, 1, LUA_MULTRET, 0);
	if (status == LUA_OK) {
		int nret = lua_gettop(L) - top;
		if (nret > 0 && lua_type(L, -nret) == LUA_TTABLE) {
			lua_pushnil(L);
			while (lua_next(L, -2) != 0) {
				int keyType = lua_type(L, -2);
				if (keyType != LUA_TSTRING) {
					logD([keyType] { return "Lua return field skipped: key type " + std::to_string(keyType); });
					lua_pop(L, 1);		/* 弹出值，保留键供下一次 lua_next */
					continue;
				}
				const char* key = lua_tolstring(L, -2, nullptr);
				Json val;
				try {
					val = lua_value_to_json(L, -1);
				} catch (const LuaNestingTooDeep& e) {
					/* C-9：就地转 ERR，绝不让它逃出去——call_lua 的调用方里
					 * freeImpl 是在 call_lua 之后才 lua_close 的，异常一逃就跳过
					 * 状态机关闭（泄漏），末尾的 lua_settop 也会被跳过而脏掉栈。 */
					ret = JsonObject();		/* 丢弃已部分填充的字段，只留契约错误 */
					ret.put("result", JsonString("ERR"));
					ret.put("message", JsonString("Lua function '" + mode + "' returned "
						+ e.what()));
					break;
				}
				logD([key, &val] { return std::string(key) + ": " + val.toString(); });
				ret.put(key, val);
				lua_pop(L, 1);
			}
		} else {
			/* N3 防线：pcall 成功但首返回值不是表（少写 return → 0 个值，或返回
			 * nil/数字/字符串）。旧实现折成空表 {}，而 ensureOk 见「无 result 键」
			 * 即判成功——驱动漏 return 会被静默当成调用成功，硬件根本没动。 */
			std::string got = nret > 0
				? std::string("a non-table value (Lua type ") + lua_typename(L, lua_type(L, -nret)) + ")"
				: "no value";
			ret.put("result", JsonString("ERR"));
			ret.put("message", JsonString("Lua function '" + mode + "' returned " + got
				+ "; the driver contract requires a table like {result=\"OK\", ...}"));
		}
	} else {
		std::string errStr = luaErrorString(L);
		std::cerr << "Lua 错误详情: " << errStr << std::endl;
		ret.put("result", JsonString("ERR"));
		ret.put("message", JsonString(errStr));
	}

	lua_settop(L, top);
	return ret;
}

} // namespace bot