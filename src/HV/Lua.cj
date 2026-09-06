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

/* ---- Lua 类型常量与取值/压栈 API 由 luajit4cj 提供（1.0.1 起导出，P7/M5 补齐） ---- */

/* ---- 读取 Lua 栈顶错误对象为字符串 ----
 *  lua_tolstring 对非 string/number 的值返回空指针，直接 toString() 是空指针解引用；
 *  先判型：string/number 原样转串（lua_tolstring 会把数字原地转字符串），
 *  其余对象返回类型说明（issue #5 清单 H5） */
public func luaErrorString(L: lua_State): String {
	let t = unsafe { Lua_type(L, -1) }
	if (t == LUA_TSTRING || t == LUA_TNUMBER) {
		let s = unsafe { Lua_tostring(L, -1) }
		return s.toString()
	}
	return "<non-string error object, Lua type ${t}>"
}

/* ---- 收紧 Lua 沙箱（issue #9 M1 可落地部分）----
 *  保留 os.getenv（模块路径）/ os.time|clock / dofile / require（HAL 的 ffi 依赖），
 *  中和可直接破坏宿主的入口：shell 执行（os.execute / io.popen）、进程退出（os.exit）、
 *  任意文件读写（io.open / io.lines / os.remove / os.rename）。
 *  各全局缺表时跳过对应组，不中断启动。 */
public func sandbox_lua(L: lua_State): Unit {
	unsafe {
		let osKeys = ["execute", "exit", "remove", "rename"]
		let kos = LibC.mallocCString("os")
		Lua_getglobal(L, kos)
		if (Lua_type(L, -1) == LUA_TTABLE) {
			for (name in osKeys) {
				let kn = LibC.mallocCString(name)
				Lua_pushnil(L)
				Lua_setfield(L, -2, kn)	/* 表在 -2：pushnil 后值在栈顶，-1 会索引 nil 致 PANIC */
				LibC.free(kn)
			}
		}
		Lua_pop(L, 1)
		LibC.free(kos)

		let ioKeys = ["popen", "open", "lines"]
		let kio = LibC.mallocCString("io")
		Lua_getglobal(L, kio)
		if (Lua_type(L, -1) == LUA_TTABLE) {
			for (name in ioKeys) {
				let kn = LibC.mallocCString(name)
				Lua_pushnil(L)
				Lua_setfield(L, -2, kn)	/* 同上：表在 -2 */
				LibC.free(kn)
			}
		}
		Lua_pop(L, 1)
		LibC.free(kio)
	}
}

public class MtVars_Lua <: MatrixVars_Hardware {

	/*
	 *  构造函数 => 传入事件函数，硬件构造函数以及硬件解构函数
	 *
	 *  FIXME: 将构造函数设置为 private
	 *
	 *  readCb: 可选的无副作用读回调（P4/M1）。有状态的 Lua 驱动（如 Ema/Kalman 滤波）
	 *  在 Lua 侧定义 read(data) 返回最近一次 call 的输出快照；无 read 硬件（守护/传感器）
	 *  在回调内回退到实时 call 语义（= play({}) 向后兼容）。
	 */
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None,
			readCb!: Option<() -> JsonValue> = None

	) {
		super(things: things, rise: rise, free: free)
		this.readCb = readCb
	}

	public override func read(): JsonValue {
		if (let Some(cb) <- this.readCb) { return cb() }
		return super.read()
	}

	private let readCb: Option<() -> JsonValue>


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

				/* 获取元数据（P8/M2：一次收集——静态列缓存重放 + 引用列实时快照） */
				let metadata = getMetadata()
				let metaFields: HashMap<String, JsonValue> = metadata.getFields()

				/*  从元数据浅拷贝（不污染共享源；转发侧复用同一份干净元数据）  */
				let obj = JsonObject()
				for (k in metaFields.keys()) {
					obj.put(k, metaFields.get(k).getOrThrow())
				}
				obj.put("mode", JsonString("call"))

				/*  合并两张（json_obj）表（元数据和参数）  */
				let fields: HashMap<String, JsonValue> = json.asObject().getFields()
				for (k in fields.keys()) {
					let val = fields.get(k).getOrThrow()
					obj.put(k, val)
				}
				logD("Lua scripts call parameters: ${obj.toString()}")

				/* 调用Lua解释器 */
				let res = call_lua(L, obj)

				logD("Lua script call result: ${res.toString()}")

				/* ---- 检查本驱动 call 的返回契约（含显式 ERR 即中断） ---- */
				MatrixVars_Hardware.ensureOk(res, ctx: "call ${luapath}")

				/* ---- controller 转发：元数据 ∪ 自己Lua的返回表 ---- */
				if (let Some(ctrl) <- controller) {
					/* 转发 payload = 同一份干净元数据 ∪ 自己Lua的返回表
					 * （原实现二次 getMetadata()：重复 O(cols·rows) 扫描且引用列被
					 *   read() 两遍；P8/M2 后一次收集两次浅拷贝） */
					let payload = JsonObject()
					for (k in metaFields.keys()) {
						payload.put(k, metaFields.get(k).getOrThrow())
					}
					let retFields: HashMap<String, JsonValue> = res.asObject().getFields()
					for (k in retFields.keys()) {
						payload.put(k, retFields.get(k).getOrThrow())
					}
					logD("Forward to controller: ${payload.toString()}")

					let ctrl_res = ctrl.play(payload)
					logD("Controller result: ${ctrl_res.toString()}")

					/* ---- 检查 controller 返回值（如 PCA9685 写寄存器失败） ---- */
					MatrixVars_Hardware.ensureOk(ctrl_res, ctx: "controller of ${luapath}")

					return ctrl_res
				}

				return res

			},

			/* 无副作用读（P4/M1）：Lua 驱动若定义 read(data)（滤波器返回快照）→ 走快照；
			 * 否则回退实时 call（= play({}) 向后兼容，守护/传感器/动作硬件引用语义不变）。
			 * 注意：不做 ensureOk——引用读是数据流而非动作，无样本/无输入属过渡态
			 * （如滤波器未 pump 前被 build 期引用），不应像显式 play 那样 halt-on-error。 */
			readCb: Some({ =>
				unsafe {
					let rc = LibC.mallocCString("read")
					Lua_getglobal(L, rc)
					let tp = Lua_type(L, -1)
					Lua_pop(L, 1)
					LibC.free(rc)
					if (tp != LUA_TNIL) {
						let metadata = getMetadata()
						metadata.put("mode", JsonString("read"))
						return call_lua(L, metadata)
					}
				}
				/* Lua 未定义 read：回退实时 call 语义 */
				let metadata = getMetadata()
				metadata.put("mode", JsonString("call"))
				return call_lua(L, metadata)
			}),

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
				logD("Lua scripts build parameters: ${obj.toString()}")

				/* 开始构建 */
				let res = call_lua(L, obj)

				logD("Lua script build result: ${res.toString()}")

				/* ---- 检查 build 返回契约：失败即抛（main 的 finally 会清理已建硬件） ---- */
				MatrixVars_Hardware.ensureOk(res, ctx: "build ${luapath}")
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
				logD("Lua scripts destroy parameters: ${obj.toString()}")

				let res = call_lua(L, obj)

				logD("Lua script destroy result: ${res.toString()}")
				/* ---- 先释放 Lua 状态机，再检查返回契约——避免检查抛异常时泄漏 ---- */
				destroy_lua(L)

				/* ---- 检查 destroy 返回契约：ERR 抛出让 deactive 的 try-catch 记录 ---- */
				MatrixVars_Hardware.ensureOk(res, ctx: "destroy ${luapath}")

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
			sandbox_lua(L)	/* 中和 os.execute/io.popen 等宿主入口（issue #9 M1） */

			/* -------- 设置Lua状态机 -------- */
			var status =  Lua_loadfilex(L, filename_c, CPointer<C_Char>())
			LibC.free(filename_c)

			if (status == LUA_OK) {
				var status = Lua_pcall(L, 0, LUA_MULTRET, 0)
				logD("lua build status: ${status}")
				if (status == LUA_OK) { return true  }
			}

			/* -------- 从栈顶获取错误信息（判型防空指针，见 luaErrorString） -------- */
			println(luaErrorString(L))
			Lua_settop(L, 0)	/* 恢复栈，错误对象不滞留（issue #5 清单 H5） */

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
				else if (val is JsonBool) {
					/* P7/M5：布尔入参不再丢弃 */
					let v = val.asBool().getValue()
					let b = if (v) { 1 } else { 0 }
					let sk = LibC.mallocCString(k)
					Lua_pushboolean(L, Int32(b))
					Lua_setfield(L, -2, sk)
					LibC.free(sk)
				}
				else if (val is JsonNull) {
					/* P7/M5：null 入参推 nil */
					let sk = LibC.mallocCString(k)
					Lua_pushnil(L)
					Lua_setfield(L, -2, sk)
					LibC.free(sk)
				}
			}

			var status = Lua_pcall(L, 1, LUA_MULTRET, 0)
			logD("lua status: ${status}")
			if (status == LUA_OK) {
				if (Lua_type(L, -1) == LUA_TTABLE) {
					/* ---- 栈顶是表  压入 nil 作为遍历的起始键 ---- */
					Lua_pushnil(L)
					while(Lua_next(L, -2) != 0) {
						/* ---- 此时栈：[表, 键, 值] ----
						 * H5 加固（issue #5 清单）：
						 * 1) 键只接受 string——Lua 手册警告遍历期间对【键】调 lua_tolstring
						 *    会原地改写键值，数字键将扰乱后续 lua_next；
						 * 2) 值支持 number/boolean/nil/string（P7/M5 类型保留），其余类型
						 *    （table/function 等）跳过——不再 throw，避免跳过栈尾 Lua_settop
						 *    造成 Lua 栈泄漏 */
						let keyType = Lua_type(L, -2)
						let valType = Lua_type(L, -1)
						if (keyType != LUA_TSTRING ||
							(valType != LUA_TSTRING && valType != LUA_TNUMBER &&
							 valType != LUA_TBOOLEAN && valType != LUA_TNIL)) {
							logD("Lua return field skipped: key type ${keyType}, value type ${valType}")
							Lua_pop(L, 1)	/* 弹出值，保留键供下一次 Lua_next */
							continue
						}
						let key = Lua_tostring(L, -2)
						var val: JsonValue
						if (valType == LUA_TNUMBER) {
							let d = Lua_tonumber(L, -1)
							let i = Lua_tointeger(L, -1)
							val = if (Float64(i) == d) { JsonInt(i) } else { JsonFloat(d) }
						}
						else if (valType == LUA_TBOOLEAN) {
							val = JsonBool(Lua_toboolean(L, -1) != 0)
						}
						else if (valType == LUA_TNIL) {
							val = JsonNull()
						}
						else {
							let s = Lua_tostring(L, -1)
							val = JsonString(s.toString())
						}
						logD("${key}: ${val.toString()}")
						ret.put(key.toString(), val)
						Lua_pop(L, 1)
					}
				}
			}

			else if (status != LUA_OK) {
				println("Lua 错误详情: ${luaErrorString(L)}")
			}

			/* -------- 恢复Lua状态机 -------- */
			Lua_settop(L, top)
		}
		return ret
	}
}
