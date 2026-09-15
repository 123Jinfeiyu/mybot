#include "luaend.hpp"

#include <algorithm>
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

/* ---- 收紧 Lua 沙箱（issue #9 M1 可落地部分）----
 * 保留 os.getenv/product、os.time|clock、dofile、require；
 * 中和可直接破坏宿主的入口。各全局缺表时跳过对应组。 */
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
 * 调用前后栈深不变。 */
Json lua_value_to_json(lua_State* L, int idx) {
	int t = lua_type(L, idx);
	if (t == LUA_TNUMBER) {
		double d = lua_tonumber(L, idx);
		long long i = static_cast<long long>(d);
		return (static_cast<double>(i) == d) ? JsonInt(i) : JsonFloat(d);
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
		std::vector<long long> arrKeys;
		std::map<long long, Json> arrVals;
		Json obj = JsonObject();
		lua_pushvalue(L, idx);			/* 表副本到栈顶 */
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			int keyType = lua_type(L, -2);
			if (keyType == LUA_TNUMBER) {
				double kd = lua_tonumber(L, -2);
				/* C7 防线：非整数数字键（如 3.5）截断会静默丢数据——转字符串键保留 */
				long long k = static_cast<long long>(kd);
				if (static_cast<double>(k) != kd) {
					Json vj = lua_value_to_json(L, -1);
					obj.put(std::to_string(kd), vj);
				} else {
					Json vj = lua_value_to_json(L, -1);
					arrVals[k] = vj;
					arrKeys.push_back(k);
				}
			} else if (keyType == LUA_TSTRING) {
				if (const char* ks = lua_tolstring(L, -2, nullptr)) {
					Json vj = lua_value_to_json(L, -1);
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
				Json val = lua_value_to_json(L, -1);
				logD([key, &val] { return std::string(key) + ": " + val.toString(); });
				ret.put(key, val);
				lua_pop(L, 1);
			}
		}
	} else {
		std::string errStr = luaErrorString(L);
		std::cout << "Lua 错误详情: " << errStr << std::endl;
		ret.put("result", JsonString("ERR"));
		ret.put("message", JsonString(errStr));
	}

	lua_settop(L, top);
	return ret;
}

} // namespace bot