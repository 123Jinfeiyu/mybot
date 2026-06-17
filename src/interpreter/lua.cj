package bot4cj.interpreter
import std.collection.*
import stdx.encoding.json.*
import ctype4cj.*

/* ---- 定义Lua状态机类型 ---- */
public type lua_State = CPointer<Unit>
public type lua_Integer = C_LongLong
public type lua_CFunction = CFunc<(lua_State) -> C_Int>

foreign func luaL_newstate(): lua_State
foreign func lua_close(L: lua_State): Unit
foreign func luaL_openlibs(L: lua_State): Unit
foreign func luaL_loadfilex(L: lua_State, filename: CString, mode: CPointer<C_Char>): C_Int
foreign func lua_pcallk (L: lua_State,
                nargs: C_Int,
                nresults: C_Int,
                msgh: C_Int,
                ctx: C_IntPtr,	/* ---- 指针大小的有符号整数 ---- */
                k: CPointer<Unit>  ): C_Int
foreign func lua_createtable(L: lua_State, narr: C_Int, nrec: C_Int): Unit
foreign func lua_pushstring(L: lua_State, value: CString): Unit
foreign func lua_setfield(L: lua_State, index: C_Int, key: CString): Unit
foreign func lua_type(L: lua_State, index: C_Int):  C_Int
foreign func lua_settop(L: lua_State, n: C_Int): Unit
foreign func lua_tolstring(L: lua_State, index: C_Int, plen: CPointer<UIntNative>): CString
foreign func lua_pushnil(L: lua_State): Unit
foreign func lua_next(L: lua_State, index: C_Int): C_Int
foreign func lua_getglobal(L: lua_State, name: CString): Unit
foreign func luaL_checkinteger(L: lua_State, arg: C_Int): lua_Integer
foreign func luaL_checklstring(L: lua_State, arg: C_Int, len: CPointer<UIntNative>): CString
foreign func lua_pushinteger(L: lua_State, n: lua_Integer): Unit
foreign func lua_pushcclosure(L: lua_State, f: lua_CFunction, n: C_Int): Unit
foreign func lua_setglobal(L: lua_State, name: CString): Unit

public const LUA_OK: C_Int = 0
public const LUA_ERR: C_Int = 1		/* ---- 这是我自己写的 ---- */
public const LUA_MULTRET: C_Int = -1 	/* ---- 将所有值都压入栈中 ----*/
public const LUA_TTABLE: C_Int = 5	/* ---- 表的类型通常为5 ---- */



/* ---- 初始化Lua状态机 ---- */
@C
public func Lua_init(): lua_State {
	let L: lua_State = unsafe { luaL_newstate() }
	//if (L.isNull()) {
	//	throw IllegalMemoryException("Lua_init()")
	//}

	return L
}


/* 检查Lua状态机是否初始化成功 */
@C
public func Lua_check(L: lua_State): C_Int {
	if (L.isNull()) { return LUA_ERR }
	else     { return LUA_OK }
}

/* ---- 释放Lua状态机 ---- */
@C
public func Lua_free(L: lua_State): Unit {
	unsafe { lua_close(L) }
}

/* ---- 加载Lua标准库 ---- */
@C
public func Lua_loadlib(L: lua_State): Unit {
	unsafe { luaL_openlibs(L) }
}

/* ---- 从文件加载一个Lua代码块（chunk）并将其编译成可执行的函数，然后压入栈顶。
 * ---- 它只负责加载和编译，不会运行代码 ---- */
@C
public func Lua_loadfilex(L: lua_State, filename: CString, mode: CPointer<C_Char>): C_Int
{
	return unsafe { luaL_loadfilex(L, filename, mode) }
}

/* ---- 受保护地调用 Lua 函数的函数，
 * ---- 是 lua_pcall() 的增强版本，增加了对C 协程（yield） 的支持 ---- */
@C
public func Lua_pcallk(L: lua_State, nargs: C_Int, nresults: C_Int, msgh: C_Int, ctx: C_IntPtr, k: CPointer<Unit>): C_Int
{
	return unsafe { lua_pcallk(L, nargs, nresults, msgh, ctx, k) }
}

/* ---- 用于预先创建表（table）的函数。
 * ---- 它会创建一个新的空表并将其压入栈顶 ----*/
@C
public func Lua_createtable(L: lua_State, narr: C_Int, nrec: C_Int): Unit
{
	unsafe { lua_createtable(L, narr, nrec) }
}

/* ---- 将 C 字符串压入 Lua 栈 ---- */
@C
public func Lua_pushstring(L: lua_State, value: CString): Unit
{
	unsafe { lua_pushstring(L, value) }
}

/* ---- 给表的字段赋值的函数 ---- */
@C
public func Lua_setfield(L: lua_State, index: C_Int, key: CString): Unit
{
	unsafe { lua_setfield(L, index, key) }
}

/* ---- 获取栈上指定索引处值的类型 ---- */
@C
public func Lua_type(L: lua_State, index: C_Int): C_Int
{
	return unsafe { lua_type(L, index) }
}

/* ---- 将 nil 值压入 Lua 栈 ---- */
@C
public func Lua_pushnil(L: lua_State): Unit
{
	unsafe { lua_pushnil(L) }
}

/* ---- 遍历表。
 * ---- 它从表中取出一个键值对，并将键和值压入栈。 ---- */
@C
public func Lua_next(L: lua_State, index: C_Int): C_Int
{
	return unsafe { lua_next(L, index) }
}

/* ---- 将整数值压入 Lua 栈 ---- */
@C
public func Lua_pushinteger(L: lua_State, n: lua_Integer): Unit
{
	unsafe { lua_pushinteger(L, n) }
}
/* ----获取全局变量值，将全局变量的值压入栈顶。 ---- */
@C
public func Lua_getglobal(L: lua_State, name: CString): Unit
{
	unsafe { lua_getglobal(L, name) }
}

/* ---- 从 Lua 栈中检查并提取整数值 ---- */
@C
public func Lua_checkinteger(L: lua_State, arg: C_Int): lua_Integer
{
	return unsafe { luaL_checkinteger(L, arg) }
}


/* -------------------------------------------------- */
/* ---- 将栈上指定索引处的 Lua 值转换为 C 字符串 ---- 
 * NOTE(20260530-qing): 其返回值不可被free  */
@C
public func Lua_tostring(L: lua_State, index: C_Int): CString {
	unsafe {
		let cstr = lua_tolstring(L, index, CPointer<UIntNative>())
		return cstr
	}
}

/* ---- 从栈顶弹出指定数量的元素 ---- */
@C
public func Lua_pop(L: lua_State, index: Int32): Unit {
	unsafe { lua_settop(L, (-index) - 1i32) }
}

/* ---- 从 Lua 栈中检查并提取字符串 ---- */
@C
public func Lua_checkstring(L: lua_State, arg: C_Int): CString
{
	return unsafe { luaL_checklstring(L, arg, CPointer<UIntNative>()) }
}

/* ---- 将 C 函数注册为 Lua 全局函数 ---- */
@C
public func Lua_register(L: lua_State, n: CString, f: lua_CFunction): Unit {
	unsafe {
		lua_pushcclosure(L, f, 0)
		lua_setglobal(L, n)
	}
}

