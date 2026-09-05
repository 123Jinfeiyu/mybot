/*
 *  Lua状态机 - 硬件变量（自 r.cj 迁出）
 *
 *  TODO(20260612-qing): 提前创建Lua状态机，利用闭包进行引用储存
 *      在rise中申请的资源将在free中被释放。
 *
 *      我们将Lua状态机做成类成员存储，在原本的召集函数中进行初始化
 *      在解构函数中释放资源。
 *
 */

package bot4cj.HV
import std.collection.*
import stdx.encoding.json.*
import ctype4cj.*
import luajit4cj.*

public class MtVars_Lua <: MatrixVars_Hardware {

	/*
	 *  构造函数 => 传入事件函数，硬件构造函数以及硬件解构函数
	 *
	 *  FIXME: 将构造函数设置为 private
	 */
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None

	) {  super(things: things, rise: rise, free: free)  }


	/*
	 *  工厂函数 => 运行指定Lua脚本，传入矩阵中的所有参数（元数据）
	 *
	 *  controller: 可选的控制器硬件变量（饿汉绑定）。
	 *  声明后本硬件变量的 call 事件变为两级调用：
	 *  先运行自己的 Lua 驱动，再把「元数据 ∪ Lua返回表」转发给 controller.play()
	 */
	public static func Factory(luapath: String, getMetadata: ()->JsonObject,
			controller!: Option<MatrixVars_Hardware> = None): MtVars_Lua
	{

		/* 创建LUA状态机对象 */
		let L: lua_State = unsafe {  Lua_init()  }

		/* 检查有没有创建成功？ */
		if ( unsafe { Lua_check(L) } == LUA_ERR) {
			throw IllegalMemoryException("Lua_check()")  }


		let mtVars_Lua = MtVars_Lua(

			/*
			 *  主要事件：调用该硬件的驱动脚本
			 *  合并两张表（json_obj），一张来自创建该硬件时的矩阵（元数据）
			 *  另一张是调用该硬件事件时传入的参数（ @Action ）
			 *
			 *  其中前者是一些诸如I2C地址、SPI速率的硬件参数
			 *  后者是一些类似舵机旋转角度的实时参数
			 *
			 * TODO(20260825): 事先创建完所有ACTION的行，创建好每一步的参数
			 */
			things: { json: JsonValue =>

				/* 获取元数据 */
				let metadata = getMetadata()

				/*  从元数据（json_obj）进行深拷贝  */
				let obj = metadata
				obj.put("mode", JsonString("call"))

				/*  合并两张（json_obj）表（元数据和参数）  */
				let fields: HashMap<String, JsonValue> = json.asObject().getFields()
				for (k in fields.keys()) {
					let val = fields.get(k).getOrThrow()
					obj.put(k, val)
				}
				println("Lua scripts call parameters: ${obj.toString()}")

				/* 调用Lua解释器 */
				let res = call_lua(L, obj)

				println("Lua script call result: ${res.toString()}")

				/* ---- controller 转发：元数据 ∪ 自己Lua的返回表 ---- */
				if (let Some(ctrl) <- controller) {
					/* 重新收集元数据（skipCol 已跳过 controller 列） */
					let payload = getMetadata()
					let retFields: HashMap<String, JsonValue> = res.asObject().getFields()
					for (k in retFields.keys()) {
						payload.put(k, retFields.get(k).getOrThrow())
					}
					println("Forward to controller: ${payload.toString()}")

					let ctrl_res = ctrl.play(payload)
					println("Controller result: ${ctrl_res.toString()}")
					return ctrl_res
				}

				return res

			},

			/* 硬件构造方法：提供元数据（json_obj）  对该硬件进行初始化 */
			rise: { =>

				/*  在这里构建Lua状态机   TODO: 使用专用的异常  */
				if (build_lua(L, luapath) == false) {
					throw Exception("Failed to build Lua state from script: '${luapath}'. Please check if the file exists and is valid Lua code.")
				}


				/* 获取元数据 */
				let metadata = getMetadata()

				/* 从元数据进行深拷贝 */
				let obj = metadata

				/* 插入模式指令 （告知lua脚本这是在构建） */
				obj.put("mode", JsonString("build"))
				println("Lua scripts build parameters: ${obj.toString()}")

				/* 开始构建 */
				let res = call_lua(L, obj)

				println("Lua script build result: ${res.toString()}")
			},

			/*
			 *  硬件解构方法：传入元数据（json_obj|json_data）
			 *  然后使用Lua状态机对设备进行关闭（或者重置）
			 */
			free: { =>

				/* 获取元数据 */
				let metadata = getMetadata()

				/*  从元数据进行深拷贝  */
				let obj = metadata

				/*  插入模式指令  */
				obj.put("mode", JsonString("destroy"))
				println("Lua scripts destroy parameters: ${obj.toString()}")

				let res = call_lua(L, obj)

				println("Lua script destroy result: ${res.toString()}")
				/* ---- 在这里写就Lua状态机的解构 ---- */
				destroy_lua(L)

			}

		)


		/*
		 * --------------------------------
		 * 返回构造的硬件变量本体
		 */
		return mtVars_Lua
	}


	/*  构造Lua状态机  申请资源
	 *
	 *  NOTE(20260618-qing): 构建完毕会运行一次 */
	private static func build_lua(L: lua_State, luapath: String): Bool {
		unsafe {
			let filename_c = LibC.mallocCString(luapath)
			if (filename_c.isEmpty()) { throw IllegalMemoryException("LibC.mallocCString()")  }

			/*  加载Lua标准库（os/io等，HAL.lua 依赖；不再注册硬件cfunc）  */
			Lua_loadlib(L)

			/* -------- 设置Lua状态机 -------- */
			var status =  Lua_loadfilex(L, filename_c, CPointer<C_Char>())
			LibC.free(filename_c)

			if (status == LUA_OK) {
				var status = Lua_pcall(L, 0, LUA_MULTRET, 0)
				println("lua build status: ${status}")
				if (status == LUA_OK) { return true  }
			}

			/* -------- 从栈顶获取错误信息 -------- */
			let error_msg = Lua_tostring(L, -1);
			println(error_msg.toString());



			return false
		}
	}

	/*
	 *  释放LUA状态机
	 *
	 *  NOTE(20260618-qing): 销毁之后该变量即废弃
	 */
	private static func destroy_lua(L: lua_State) {
		unsafe { Lua_free(L) }
	}

	/*
	 *  调用Lua脚本的方法
	 *
	 *  NOTE(20260618-qing): 每次调用之后会重置栈指针
	 */
	private static func call_lua(L: lua_State, json: JsonValue): JsonValue {
		let obj = json.asObject()
		let ret = JsonObject()
		unsafe {

			/* ---- 获取栈顶指针  ----*/
			let top  = Lua_gettop(L)

			/* ---- ---- */
			let mode = obj.get("mode").getOrThrow().asString().getValue()
			let cstr = LibC.mallocCString(mode)
			Lua_getglobal(L, cstr)
			LibC.free(cstr)

			/* ----在Lua状态机里 创建Lua表 ---- */
			Lua_createtable(L, 0, 0)


			/* ---- 向Lua表中推入数据 ---- */
			let fields: HashMap<String, JsonValue> = obj.getFields()
			for (k in fields.keys()) {
				let val = fields.get(k).getOrThrow()
				if (val is JsonString) {
					let v = val.asString().getValue()
					let sk = LibC.mallocCString(k)
					let sv = LibC.mallocCString(v)
					Lua_pushstring(L, sv)
					Lua_setfield(L, -2, sk)
					LibC.free(sk)
					LibC.free(sv)
				}
				else if (val is JsonInt) {
					let v = val.asInt().getValue()
					let sk = LibC.mallocCString(k)
					let sv = Int64(v)
					Lua_pushinteger(L, sv)
					Lua_setfield(L, -2, sk)
					LibC.free(sk)
				}
				else if (val is JsonFloat) {
					let v = val.asFloat().getValue()
					let sk = LibC.mallocCString(k)
					Lua_pushnumber(L, v)
					Lua_setfield(L, -2, sk)
					LibC.free(sk)
				}
			}

			var status = Lua_pcall(L, 1, LUA_MULTRET, 0)
			println("lua status: ${status}")
			if (status == LUA_OK) {
				if (Lua_type(L, -1) == LUA_TTABLE) {
					/* ---- 栈顶是表  压入 nil 作为遍历的起始键 ---- */
					Lua_pushnil(L)
					while(Lua_next(L, -2) != 0) {
						/* ---- 此时栈：[表, 键, 值]  检查键值是否为空指针 */
						let key = Lua_tostring(L, -2)
						let val = Lua_tostring(L, -1)
						if (key.isEmpty() || val.isEmpty()) {
							throw NoneValueException("Lua_tostring()")
						}
						println("${key}: ${val}")
						ret.put(key.toString(), JsonString(val.toString()))
						Lua_pop(L, 1)
					}
				}
			}

			else if (status != LUA_OK) {
				let err  = Lua_tostring(L, -1)
				if (!err.isEmpty()) { println("Lua 错误详情: ${err.toString()}") }
			}

			/* -------- 恢复Lua状态机 -------- */
			Lua_settop(L, top)
		}
		return ret
	}
}
