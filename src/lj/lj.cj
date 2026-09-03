package bot4cj.lj
import ctype4cj.*

/*
 * ---------------- LuaJIT 仓颉绑定 ----------------
 * 替代 lua4cj（Lua 5.4），直绑 libluajit-5.1.so（Lua 5.1 API）。
 *
 * INFO(20260904-qing): 与 Lua 5.4 绑定的差异（符号已用 nm -D 审计）：
 *   - 无 lua_pcallk，使用 lua_pcall
 *   - lua_getglobal/lua_setglobal 在 5.1 是宏，改绑底层 lua_getfield + LUA_GLOBALSINDEX
 *   - lua_getfield 返回 void（5.4 返回 int）
 *   - lua_pushstring/luaL_loadfilex 为真实导出符号，直接绑定
 */

/* ---- 状态机与数值类型 ---- */
public type lua_State = CPointer<Unit>
public type lua_Integer = C_LongLong	/* LuaJIT: ptrdiff_t，arm64 上 64 位 */
public type lua_Number = C_Double

/* ---- foreign func：libluajit-5.1.so 的 C 符号 ---- */
foreign func luaL_newstate(): lua_State
foreign func lua_close(L: lua_State): Unit
foreign func luaL_openlibs(L: lua_State): Unit
foreign func luaL_loadfilex(L: lua_State, filename: CString, mode: CPointer<C_Char>): C_Int
foreign func lua_pcall(L: lua_State, nargs: C_Int, nresults: C_Int, errfunc: C_Int): C_Int
foreign func lua_createtable(L: lua_State, narr: C_Int, nrec: C_Int): Unit
foreign func lua_pushstring(L: lua_State, value: CString): Unit
foreign func lua_setfield(L: lua_State, index: C_Int, key: CString): Unit
foreign func lua_getfield(L: lua_State, index: C_Int, key: CString): Unit
foreign func lua_type(L: lua_State, index: C_Int): C_Int
foreign func lua_gettop(L: lua_State): C_Int
foreign func lua_settop(L: lua_State, n: C_Int): Unit
foreign func lua_tolstring(L: lua_State, index: C_Int, plen: CPointer<UIntNative>): CString
foreign func lua_pushnil(L: lua_State): Unit
foreign func lua_next(L: lua_State, index: C_Int): C_Int
foreign func lua_pushinteger(L: lua_State, n: lua_Integer): Unit
foreign func lua_pushnumber(L: lua_State, n: lua_Number): Unit

/* ---- 常量 ---- */
public const LUA_OK: C_Int = 0
public const LUA_ERR: C_Int = 1			/* 自定义：状态机为空 */
public const LUA_MULTRET: C_Int = -1		/* 将所有返回值压栈 */
public const LUA_TTABLE: C_Int = 5		/* 表类型 */
public const LUA_GLOBALSINDEX: C_Int = -10002	/* 5.1 伪索引：全局表 */

/* ---- 初始化Lua状态机 ---- */
@C
public func Lua_init(): lua_State {
	let L: lua_State = unsafe { luaL_newstate() }
	return L
}

/* ---- 检查Lua状态机是否初始化成功 ---- */
@C
public func Lua_check(L: lua_State): C_Int {
	if (L.isNull()) { return LUA_ERR }
	else            { return LUA_OK  }
}

/* ---- 释放Lua状态机 ---- */
@C
public func Lua_free(L: lua_State): Unit {
	unsafe { lua_close(L) }
}

/* ---- 加载Lua标准库（含 ffi，HAL.lua 依赖） ---- */
@C
public func Lua_loadlib(L: lua_State): Unit {
	unsafe { luaL_openlibs(L) }
}

/* ---- 从文件加载Lua代码块并编译压栈（不运行） ---- */
@C
public func Lua_loadfilex(L: lua_State, filename: CString, mode: CPointer<C_Char>): C_Int {
	return unsafe { luaL_loadfilex(L, filename, mode) }
}

/* ---- 受保护调用（5.1 无 pcallk，不支持跨C协程yield） ---- */
@C
public func Lua_pcall(L: lua_State, nargs: C_Int, nresults: C_Int, errfunc: C_Int): C_Int {
	return unsafe { lua_pcall(L, nargs, nresults, errfunc) }
}

/* ---- 创建空表压栈 ---- */
@C
public func Lua_createtable(L: lua_State, narr: C_Int, nrec: C_Int): Unit {
	unsafe { lua_createtable(L, narr, nrec) }
}

/* ---- 将C字符串压栈 ---- */
@C
public func Lua_pushstring(L: lua_State, value: CString): Unit {
	unsafe { lua_pushstring(L, value) }
}

/* ---- 给表字段赋值（弹出栈顶值） ---- */
@C
public func Lua_setfield(L: lua_State, index: C_Int, key: CString): Unit {
	unsafe { lua_setfield(L, index, key) }
}

/* ---- 在全局表中搜索目标并压栈（5.1 的 lua_getglobal 是宏，展开为此调用） ---- */
@C
public func Lua_getglobal(L: lua_State, name: CString): Unit {
	unsafe { lua_getfield(L, LUA_GLOBALSINDEX, name) }
}

/* ---- 获取栈上指定索引处值的类型 ---- */
@C
public func Lua_type(L: lua_State, index: C_Int): C_Int {
	return unsafe { lua_type(L, index) }
}

/* ---- 将 nil 压栈 ---- */
@C
public func Lua_pushnil(L: lua_State): Unit {
	unsafe { lua_pushnil(L) }
}

/* ---- 遍历表：取出下一个键值对压栈 ---- */
@C
public func Lua_next(L: lua_State, index: C_Int): C_Int {
	return unsafe { lua_next(L, index) }
}

/* ---- 将整数压栈 ---- */
@C
public func Lua_pushinteger(L: lua_State, n: lua_Integer): Unit {
	unsafe { lua_pushinteger(L, n) }
}

/* ---- 将浮点数压栈 ---- */
@C
public func Lua_pushnumber(L: lua_State, n: lua_Number): Unit {
	unsafe { lua_pushnumber(L, n) }
}

/* ---- 将栈上指定索引处的值转换为C字符串 ----
 * NOTE: 返回值不可被 free（指向Lua内部缓冲） */
@C
public func Lua_tostring(L: lua_State, index: C_Int): CString {
	unsafe {
		let cstr = lua_tolstring(L, index, CPointer<UIntNative>())
		return cstr
	}
}

/* ---- 获取栈顶指针 ---- */
@C
public func Lua_gettop(L: lua_State): C_Int {
	return unsafe { lua_gettop(L) }
}

/* ---- 设置栈顶指针 ---- */
@C
public func Lua_settop(L: lua_State, n: C_Int): Unit {
	unsafe { lua_settop(L, n) }
}

/* ---- 从栈顶弹出指定数量的元素 ---- */
@C
public func Lua_pop(L: lua_State, index: Int32): Unit {
	unsafe { lua_settop(L, (-index) - 1i32) }
}
