package bot4cj.interpreter
import std.collection.*
import stdx.encoding.json.*
import ctype4cj.*

/* ---- 定义Lua状态机类型 ---- */
public type lua_State = CPointer<Unit>

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

public const LUA_OK: C_Int = 0
public const LUA_MULTRET: C_Int = -1 	/* ---- 将所有值都压入栈中 ----*/
public const LUA_TTABLE: C_Int = 5	/* ---- 表的类型通常为5 ---- */



/* ---- 初始化Lua状态机 ---- */
@C
public func Lua_init(): lua_State {
	let L: lua_State = unsafe { luaL_newstate() }
	if (L.isNull()) {
		throw IllegalMemoryException("Lua_init()")
	}

	return L
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

/* ----获取全局变量值，将全局变量的值压入栈顶。 ---- */
@C
public func Lua_getglobal(L: lua_State, name: CString): Unit
{
	unsafe { lua_getglobal(L, name) }
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

/* ---- 遍历表。
 * ---- 它从表中取出一个键值对，并将键和值压入栈。 ---- */
@C
public func Lua_next(L: lua_State, index: C_Int): C_Int
{
	return unsafe { lua_next(L, index) }
}


/* ----------------------------------------------------------- */
/* NOTE(20260530-qing): 考虑到CFunc兼容性，先进行一次CFunc封装 */
//public class Lua {
//	private let L: CPointer<Unit>
//	public init() {
//		unsafe {
//			L = luaL_newstate()
//			luaL_openlibs(L)
//		}
//	}
//
//	/* ---- FIXME: 提供机制而非策略，将逻辑后推到别的类 ---- */
//	public func runFile_withTable(filename: String, obj: JsonObject, nret: Int32): Int32 {
//		unsafe {
//			let fn = LibC.mallocCString(filename)
//			if (fn.isEmpty()) { return -1i32 }
//			var status = Lua_loadfilex(L, fn, CPointer<C_Char>())
//			if (status == LUA_OK) {
//				push_string_dict(obj)
//				status = lua_pcallk(L, nret, LUA_MULTRET, 0, 0, CPointer<Unit>())
//				if (status == LUA_OK) {
//					if (isLuaTable(-1i32)) {
//						read_string_dict()
//					}
//					lua_settop(L, (-1i32)-1i32)
//				}
//
//				else if (status != LUA_OK) {
//					let  err = lua_tolstring(L, -1, CPointer<UIntNative>());
//					println("Lua 错误详情: ${err.toString()}");
//					lua_settop(L, (-1i32)-1i32);
//				}
//
//			}
//			LibC.free(fn)
//			return status
//		}
//	}
//
//	public func close() {
//		unsafe { lua_close(L) }
//	}
//
//	public func lua_pop(index: Int32): Unit {
//		unsafe { lua_settop(L, (-index) - 1i32) }
//	}
//
//	public func push_string_dict(obj: JsonObject) {
//		unsafe { lua_createtable(L, 0i32, 0i32) }
//		let fields: HashMap<String, JsonValue> = obj.getFields()
//		for (k in fields.keys()) {
//			let v: String =  fields.get(k).getOrThrow().asString().getValue()
//			unsafe {
//				let sk = LibC.mallocCString(k)
//				let sv = LibC.mallocCString(v)
//				lua_pushstring(L, sv)
//				lua_setfield(L, -2i32, sk)
//				LibC.free(sk)
//				LibC.free(sv)
//			}
//		}
//	}
//
//	public func read_string_dict() {
//		/* ---- 假设栈顶是表 ---- */
//		
//		/* ---- 压入 nil 作为遍历的起始键 ---- */
//		unsafe {
//			lua_pushnil(L)
//			while (lua_next(L, -2) != 0i32) {
//				/* ---- 此时栈：[表, 键, 值] ---- */
//				let key = lua_tostring(-2)
//				let val = lua_tostring(-1)
//				println("${key}: ${val}")
//				lua_pop(1)
//			}
//		}
//	}
//
//	public func isLuaTable(index: Int32): Bool {
//		unsafe {
//			let t = lua_type(L, index)
//			return t == LUA_TTABLE
//		}
//	}
//
//	public func lua_tostring(index: Int32): String {
//		unsafe {
//			let cstr = lua_tolstring(L, index, CPointer<UIntNative>())
//			if (!cstr.isEmpty()) {
//				return cstr.toString()
//			}
//			else { throw NoneValueException("Lua value") }
//		}
//	}
//}
