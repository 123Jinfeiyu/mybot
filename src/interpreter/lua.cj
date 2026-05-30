package bot4cj.interpreter
import std.collection.*
import stdx.encoding.json.*
foreign func luaL_newstate(): CPointer<Unit>
foreign func lua_close(L: CPointer<Unit>): Unit
foreign func luaL_openlibs(L: CPointer<Unit>): Unit
foreign func luaL_loadfilex(L: CPointer<Unit>, filename: CString, mode: CPointer<Unit>): Int32
foreign func lua_pcallk (L: CPointer<Unit>,
                nargs: Int32,
                nresults: Int32,
                msgh: Int32,
                ctx: Int64,
                k: CPointer<Unit>): Int32
foreign func lua_createtable(L: CPointer<Unit>, narr: Int32, nrec: Int32): Unit
foreign func lua_pushstring(L: CPointer<Unit>, value: CString): Unit
foreign func lua_setfield(L: CPointer<Unit>, index: Int32, key: CString): Unit
foreign func lua_istable(L: CPointer<Unit>,  index: Int32): Int32
foreign func lua_settop(L: CPointer<Unit>, n: Int32): Unit
foreign func lua_type(L: CPointer<Unit>, index: Int32):  Int32
foreign func lua_tolstring(L: CPointer<Unit>, index: Int32, plen: CPointer<UIntNative>): CString
foreign func lua_pushnil(L: CPointer<Unit>): Unit
foreign func lua_next(L: CPointer<Unit>, index: Int32): Int32

const LUA_OK: Int32 = 0
const LUA_MULTRET: Int32 = -1 	/* ---- 将所有值都压入栈中 ----*/
const LUA_TTABLE: Int32 = 5	/* ---- 表的类型通常为5 ---- */
public class Lua {
	private let L: CPointer<Unit>
	public init() {
		unsafe {
			L = luaL_newstate()
			luaL_openlibs(L)
		}
	}

	/* ---- FIXME: 提供机制而非策略，将逻辑后推到别的类 ---- */
	public func runFile_withTable(filename: String, obj: JsonObject, nret: Int32): Int32 {
		unsafe {
			let fn = LibC.mallocCString(filename)
			if (fn.isEmpty()) { return -1i32 }
			var status = luaL_loadfilex(L, fn, CPointer<Unit>())
			if (status == LUA_OK) {
				push_string_dict(obj)
				status = lua_pcallk(L, nret, LUA_MULTRET, 0, 0, CPointer<Unit>())
				println(status)
				if (status == LUA_OK) {
					if (isLuaTable(-1i32)) {
						read_string_dict()
					}
					lua_settop(L, (-1i32)-1i32)
				}

				else if (status != LUA_OK) {
					let  err = lua_tolstring(L, -1, CPointer<UIntNative>());
					println("Lua 错误详情: ${err.toString()}");
					lua_settop(L, (-1i32)-1i32);
				}

			}
			LibC.free(fn)
			return status
		}
	}

	public func close() {
		unsafe { lua_close(L) }
	}

	public func lua_pop(index: Int32): Unit {
		unsafe { lua_settop(L, (-index) - 1i32) }
	}

	public func push_string_dict(obj: JsonObject) {
		unsafe { lua_createtable(L, 0i32, 0i32) }
		let fields: HashMap<String, JsonValue> = obj.getFields()
		for (k in fields.keys()) {
			let v: String =  fields.get(k).getOrThrow().asString().getValue()
			unsafe {
				let sk = LibC.mallocCString(k)
				let sv = LibC.mallocCString(v)
				lua_pushstring(L, sv)
				lua_setfield(L, -2i32, sk)
				LibC.free(sk)
				LibC.free(sv)
			}
		}
	}

	public func read_string_dict() {
		/* ---- 假设栈顶是表 ---- */
		
		/* ---- 压入 nil 作为遍历的起始键 ---- */
		unsafe {
			lua_pushnil(L)
			while (lua_next(L, -2) != 0i32) {
				/* ---- 此时栈：[表, 键, 值] ---- */
				let key = lua_tostring(-2)
				let val = lua_tostring(-1)
				println("${key}: ${val}")
				lua_pop(1)
			}
		}
	}

	public func isLuaTable(index: Int32): Bool {
		unsafe {
			let t = lua_type(L, index)
			println(t)
			return t == LUA_TTABLE
		}
	}

	public func lua_tostring(index: Int32): String {
		unsafe {
			let cstr = lua_tolstring(L, index, CPointer<UIntNative>())
			if (!cstr.isEmpty()) {
				return cstr.toString()
			}
			else { throw NoneValueException("Lua value") }
		}
	}
}
