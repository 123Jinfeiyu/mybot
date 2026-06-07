/* 机器人构成模块
 * INFO(20260514-qing): 该模块确定机器人结构，为解耦考虑应该从解析出的矩阵对象中构建
 * TODO(20260527-qing): 使脚本中的数值亦可从运行时确定  */

package bot4cj
import std.io.*
import std.time.*
import std.random.*
import std.collection.*
import std.convert.*
import stdx.encoding.json.*
import bot4cj.hd.*
//import bot4cj.hd.extern.*
import bot4cj.math.*
import bot4cj.interpreter.*
import ctype4cj.*

/* ---------------- 硬件变量基类 ----------------
 * 接收一个lambda表达式，在回调函数中定义行为
 * 通过JsonValue这种通用的数据结构进行传值
 * TODO*(20260517-qing): 因为析构器（终结器）的调用时间不确定，需要手动构造RAII机制
 * - 需要进一步解耦各种原始硬件模块类，使其初始化部分单独调用
 * TODO(20260519-qing): 考虑将@Action矩阵对象置为硬件变量的一种
 * TODO*(20260529-qing): 增加微调器机制，通过不同的微调器对@Action进行微调
 * ----------------------------------------------  */
open class MatrixVars_Hardware {

	/*  ---------------- 主初始化器 ----------------
	 *  - 输入一个回调函数，在这里采用json值进行参数传递，能够为多种硬件同一接口
	 *  - NOTE(20260516-qing): 但是这样的话，我怎么知道矩阵式中的对象有哪些输入参数呢
	 *  --------------------------------------------  */
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit>,
			free!: Option<() -> Unit>  ) {
		this.things = things
		this.rise = rise
		this.free = free  }

	/* -------- 事件回调，接收和返回JsonValue -------- */
	private let things: (JsonValue) -> JsonValue	

	/* -------- 召集事件，不接受参数 -------- */
	private let rise: Option<() -> Unit>

	/* -------- 遣返事件，不接受参数 -------- */
	private let free: Option<() -> Unit>

	/* -------- 事件发生器 -------- */
	public func play(input: JsonValue): JsonValue {
		return things(input)  }

	/* -------- 伪初始化器 -------- */
	public func build() {
		if (let Some(callback) <- rise) {
			callback()  }
	}

	/* -------- 伪解构器 -------- */
	public func destroy() {
		if (let Some(callback) <- free) {
			callback()  }
	}

	/* ----------------- */
	/* ----------------- */
	/* INFO(20260514-qing):  这两个函放在外面很突兀，考虑放进Matrix_HardVars  (done) */

	/* -------- 获得项目值 -------- */
	public static func getMt(obj: MatrixObject, row: Int64, col: Int64): String
	{
		let raw = obj[row, col]
		if (raw == "-" && row > 1) {
			return getMt(obj, row - 1, col)  }
		return raw
	}

	/* -------- 解析项目值 -------- */
	public static func parseMt<T>(obj: MatrixObject, row: Int64, col: Int64): T  where T <: Parsable<T>
	{
		let raw = obj[row, col]
		if (raw == "-" && row > 1) {
			let prev = getMt(obj, row-1, col)
			return T.parse(prev)
		}
		else if (raw == "-" && !row > 1) {
			return T.parse("0")  }

		// else if (!raw == "-")
		return T.parse(raw)
	}
	/* ----------------- */
	/* ----------------- */
}


/* ---- 用于注册到Lua的I2C操作C语言函数 ----
 *
 * ---- 打开一个I2C设备 ----  */
@C
func Lua_openI2c(L: lua_State): C_Int {
	unsafe {
		let filename: CString = Lua_checkstring(L, 1)	/* 第一个参数 */
		let fd = openI2cDev(filename)
		let val: lua_Integer = Int64(fd)	/* lua_Integer */
		Lua_pushinteger(L, val) 
		return 1	/* 返回1，表示有1个返回值 */
	}
}

/* ---- 关闭I2C设备 ---- */
@C
func Lua_closeI2c(L: lua_State): C_Int {
	unsafe {
		let fd = Int32(Lua_checkinteger(L, 1))	/* C_Int */
		closeI2cDev(fd)
		return 0
	}
}

/* ---- 设置I2C地址 ---- */
@C
func Lua_setI2cAddr(L: lua_State): C_Int {
	unsafe {
		let fd = Int32(Lua_checkinteger(L, 1))	/* C_Int */
		let addr = UInt8(Lua_checkinteger(L, 2))
		setI2cAddr(fd, addr)
		return 0
	}
}

/* ---- 写I2C寄存器 ---- */
@C
func Lua_writeI2cReg(L: lua_State): C_Int {
	unsafe {
		let fd = Int32(Lua_checkinteger(L, 1))	/* C_Int */
		let reg = UInt8(Lua_checkinteger(L, 2))
		let val = UInt8(Lua_checkinteger(L, 3))
		writeI2cReg(fd, reg, val)
		return 0
	}
}

/* ---- 读I2C寄存器 ---- */
@C
func Lua_readI2cReg(L: lua_State): C_Int {
	unsafe {
		let fd = Int32(Lua_checkinteger(L, 1))	/* C_Int */
		let reg = UInt8(Lua_checkinteger(L, 2))
		let res = Int64(readI2cReg(fd, reg))	/* lua_Integer */
		Lua_pushinteger(L, res)
		return 1
	}
}

/* ---- 毫秒级延时 ---- */
@C
func Lua_milliSleep(L: lua_State): C_Int {
	unsafe {
		let n = Int64(Lua_checkinteger(L, 1))
		sleep(Duration.millisecond * n)
		return 0
	}
}



/* ---- Lua解释器 - 硬件变量 ---- */
class MtVars_Lua <: MatrixVars_Hardware {

	/* ---- 构造函数 => 传入召集函数以及解散函数 ---- */
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None  )
		{  super(things: things, rise: rise, free: free)  }

	/* ---- 工厂函数 => 运行指定Lua脚本，传入矩阵中的所有参数 ---- */
	public static func Factory(luapath: String, metadata: JsonValue): MtVars_Lua
	{

		/* ---- 从矩阵中取出所有的参数，移动到了机器人结构类里 ---- */
		//let obj = JsonObject()
		//let col = Defs.col
		//for (c in 0..col) {
		//	let colname = Defs[0, c]
		//	let colval = getMt(Defs, i, c)
		//	obj.put(colname, JsonString(colval))
		//}

		let mtVars_Lua = MtVars_Lua(
			things: {json: JsonValue =>

				/* ---- 从元数据jsonobj进行深拷贝 ---- */
				let obj = JsonValue.fromStr(metadata.toJsonString()).asObject()
				obj.put("mode", JsonString("call"))

				/* --- 合并两张jsonobj表（元数据和参数） --- */
				let fields: HashMap<String, JsonValue> = json.asObject().getFields()
				for (k in fields.keys()) {
					let val = fields.get(k).getOrThrow()
					obj.put(k, val)
				}
				println("Lua scripts call parameters: ${obj.toString()}")
				let res = call_lua(luapath, json)
				println("Lua script call result: ${res.toString()}")
				return res

			},
			rise: { =>
				/* ---- 从元数据进行深拷贝 ---- */
				let obj = JsonValue.fromStr(metadata.toJsonString()).asObject()
				/* ---- 插入模式指令 ---- */
				obj.put("mode", JsonString("build"))
				println("Lua scripts build parameters: ${obj.toString()}")
				let res = call_lua(luapath, obj)
				println("Lua script build result: ${res.toString()}")
			},
			free: { =>
				/* ---- 从元数据进行深拷贝 ---- */
				let obj = JsonValue.fromStr(metadata.toJsonString()).asObject()
				/* ---- 插入模式指令 ---- */
				obj.put("mode", JsonString("destroy"))
				println("Lua scripts destroy parameters: ${obj.toString()}")
				let res = call_lua(luapath, obj)
				println("Lua script destroy result: ${res.toString()}")
			}
		)


		/* ---- 返回构造的硬件变量 ---- */
		return mtVars_Lua
	}

	/* ---- 调用Lua脚本的方法  ---- */
	private static func call_lua(luapath: String, json: JsonValue): JsonValue {
		let obj = json.asObject()
		let ret = JsonObject()
		unsafe {
			/* ---- 申请资源 ---- */
			let L: lua_State = Lua_init()
			let filename_c = LibC.mallocCString(luapath)
			if (filename_c.isEmpty()) {
				throw IllegalMemoryException("LibC.mallocCString()")
			}

			/* ---- 加载Lua库、注册Lua函数 ---- */
			Lua_loadlib(L)

			/* milli_sleep() */
			let cstr_sleep = LibC.mallocCString("milli_sleep")
			Lua_register(L, cstr_sleep, Lua_milliSleep)
			LibC.free(cstr_sleep)

			/* ---- i2c_open() ---- */
			let cstr_open = LibC.mallocCString("i2c_open")
			Lua_register(L, cstr_open, Lua_openI2c)
			LibC.free(cstr_open)

			/* ---- i2c_close() ---- */
			let cstr_close = LibC.mallocCString("i2c_close")
			Lua_register(L, cstr_close, Lua_closeI2c)
			LibC.free(cstr_close)

			/* ---- i2c_set_addr() ---- */
			let cstr_set = LibC.mallocCString("i2c_set_addr")
			Lua_register(L, cstr_set, Lua_setI2cAddr)
			LibC.free(cstr_set)

			/* ---- i2c_write_reg() ---- */
			let cstr_write = LibC.mallocCString("i2c_write_reg")
			Lua_register(L, cstr_write, Lua_writeI2cReg)
			LibC.free(cstr_write)

			/* ---- i2c_read_reg() ---- */
			let cstr_read = LibC.mallocCString("i2c_read_reg")
			Lua_register(L, cstr_read, Lua_readI2cReg)
			LibC.free(cstr_read)

			/* ---- 设置Lua状态机 ---- */
			var status =  Lua_loadfilex(L, filename_c, CPointer<C_Char>())
			if (status == LUA_OK) {

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
				}

				status = Lua_pcallk(L, 1, LUA_MULTRET, 0, 0, CPointer<Unit>())
				println("lua status: ${status}")
				if (status == LUA_OK) {
					if (Lua_type(L, -1) == LUA_TTABLE) {
						/* ---- 栈顶是表 ---- 
							* ---- 压入 nil 作为遍历的起始键 ---- */
						Lua_pushnil(L)
						while(Lua_next(L, -2) != 0) {
							/* ---- 此时栈：[表, 键, 值] ----
							 * ---- 检查键值是否为空指针 ---- 
							 * ---- FIXME(20260531-qing) ---- */
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
					Lua_pop(L, 1)
				}

				else if (status != LUA_OK) {
					let err  = Lua_tostring(L, -1)
					if (!err.isEmpty()) {
						println("Lua 错误详情: ${err.toString()}")
					}
					Lua_pop(L, 1)
				}

			}

			else {
				// 从栈顶获取错误信息
				let error_msg = Lua_tostring(L, -1);
				println(error_msg.toString());
			}

			/* ---- 释放资源 ---- */
			LibC.free(filename_c)			
			Lua_free(L)
		}

		return ret
	}
}

/* -------- 别名变量 -------- */
class MtVars_Alias <: MatrixVars_Hardware {
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None  )
		{  super(things: things, rise: rise, free: free)  }

	public static func Factory(AliasDefs: MatrixObject, i: Int64,
			_: HashMap<String, MatrixVars_Hardware>): MtVars_Alias
	{
		let idxName = AliasDefs.indexOfCol(0, "name")
		let idxAlias= AliasDefs.indexOfCol(0, "alias")

		let name = AliasDefs[i, idxName]
		let alias= getMt(AliasDefs, i, idxAlias)

		println("Create a Alias vars: ${name}, witch means ${alias}")
		let mtvars_alias = MtVars_Alias(things: {_: JsonValue =>
			let str = JsonString(alias)	/* 直接返回JsonString（alias） */
			return str
		})

		return mtvars_alias
	}
}

/* -------- I2C变量类 -------- */
class MtVars_I2C <: MatrixVars_Hardware {
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None  )
		{  super(things: things, rise: rise, free: free)  }

	/*  @I2C
	 *   name        path;
	 *  i2c-0  /dev/i2c-0;
	 *  @
	 */
	public static func Factory(	/*  工厂模式，根据矩阵对象构造  */
		I2CDefs: MatrixObject, i: Int64, _: HashMap<String, MatrixVars_Hardware>): MtVars_I2C
	{
		let idxName  = I2CDefs.indexOfCol(0, "name")
		let idxPath  = I2CDefs.indexOfCol(0, "path")

		let name = I2CDefs[i, idxName]
		let path = getMt(I2CDefs, i, idxPath)

		println("Create a I2C hard-Var: ${name},\tpath: ${path}")	/* debug */
		let i2c = I2C_Orig(path)	/*  在这里创建传统I2C硬件对象，通过闭包保存在MtVars_I2C里  */
		let mtvars_i2c = MtVars_I2C(things: {json: JsonValue =>			/*  {"mode": "set_addr", "addr": 0x01} */
			//let obj = json.asObject()					/*  {"mode": "set_val",  "reg": 0x01, "val": 0x56} */
			//let mode = obj.get("mode").getOrThrow().asString().getValue()	/*  {"mode": "get_val",  "reg": 0x02} */
			//if (mode == "get_fd") {
				let fd = i2c.getFd()
				let ret = JsonObject()
				ret.put("fd",JsonInt(Int64(fd)))
				return ret
			//}
			//else if (mode == "set_addr")  {
			//	let addr = obj.get("addr").getOrThrow().asInt().getValue()
			//	i2c.setAddr(UInt8(addr))
			//	let ret = JsonObject()
			//	ret.put("status", JsonString("OK"))
			//	return ret  }
			//else if (mode == "set_val") {
			//	let reg = obj.get("reg").getOrThrow().asInt().getValue()
			//	let val = obj.get("val").getOrThrow().asInt().getValue()
			//	i2c.writeReg(UInt8(reg), UInt8(val))
			//	let ret = JsonObject()
			//	ret.put("status", JsonString("OK"))
			//	return ret  }
			//else if (mode == "get_val") {
			//	let reg = obj.get("reg").getOrThrow().asInt().getValue()
			//	let raw = i2c.readReg(UInt8(reg))
			//	let ret = JsonObject()
			//	ret.put("status", JsonString("OK"))
			//	ret.put("value", JsonInt(Int64(raw)))
			//	return ret  }

			/* ---- 准备返回值 ---- */
			/* FIXME: 改为抛出异常 */
			//let ret = JsonObject()
			//ret.put("status", JsonString("ERR"))
			//ret.put("message", JsonString("Invalid Mode"))
			//return ret
		},

		free: {=>
			println("I2C HVar: ${name} is closing now")
			i2c._close()
		})

		return mtvars_i2c
	}
}

/* -------- MPU6050_BIND硬件变量 --------*/
class MtVars_MPU6050_BIND <: MatrixVars_Hardware {
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None) {
		super(things: things, rise: rise, free: free)  }

	/* 工厂函数，通过矩阵对象构建硬件变量
	 * @MPU6050_BIND
	 *      name    I2C   addr;
	 * mpu6050-0  i2c-1   0x69;
	 */
	public static func Factory (
			MPU6050_BIND_Defs: MatrixObject, i: Int64,
			map: HashMap<String, MatrixVars_Hardware>)//: MtVars_MPU6050_BIND
	{
		let idxName = MPU6050_BIND_Defs.indexOfCol(0, "name")
		let idxI2c  = MPU6050_BIND_Defs.indexOfCol(0, "I2C")
		let idxAddr = MPU6050_BIND_Defs.indexOfCol(0, "addr")

		let name = MPU6050_BIND_Defs[i, idxName]
		let i2c  = getMt(MPU6050_BIND_Defs, i, idxI2c)
		let addr = getMt(MPU6050_BIND_Defs, i, idxAddr)

		println("Create a MPU6050_BIND HVar: ${name}, use i2c interf: ${i2c}, i2c addr: ${addr}")
		let i2c_HardVar = map[i2c]
		let param = JsonObject()
		param.put("mode", JsonString("get_fd"))
		let ret = i2c_HardVar.play(param)
		let fd = Int32(ret.asObject().get("fd").getOrThrow().asInt().getValue())
		let _addr = UInt8.parse(addr)

		let mpu6050 = MPU6050(fd, _addr)		

		let mtvars_mpu6050bind = MtVars_MPU6050_BIND(
			things: {json: JsonValue => 
				let ret = JsonObject()
				ret.put("status", JsonString("OK"))
				return ret
			},
			rise: {=>
				println("MPU6050_BIND HVar: ${name} is starting now")
				mpu6050.setup()
				mpu6050.startDaemon()
			},
			free: {=>
				println("MPU6050_BIND HVar: ${name} is closing now")
				mpu6050.stopDaemon()
				mpu6050.waitDaemon()
			}
		)

		return mtvars_mpu6050bind
	}
}

/* -------- PCA9685硬件变量 -------- */
//class MtVars_PCA9685 <: MatrixVars_Hardware {
//	public init(things!: (JsonValue) -> JsonValue,
//			rise!: Option<() -> Unit> = None,
//			free!: Option<() -> Unit> = None  )
//		{  super(things: things, rise: rise, free: free)  }
//
//	/*
//	 *  @PCA9685
//	 *       name    I2C   path;
//	 *  pca9685-0  i2c-1   0x40;
//	 *  @
//	 */
//	public static func Factory(	/*  工厂模式，根据矩阵对象构造，并在哈系变量表中搜索相关依赖 */
//		PCA9685Defs: MatrixObject, i: Int64, map: HashMap<String, MatrixVars_Hardware>): MtVars_PCA9685
//	{
//		let idxName = PCA9685Defs.indexOfCol(0, "name")
//		let idxI2c  = PCA9685Defs.indexOfCol(0, "I2C")
//		let idxAddr = PCA9685Defs.indexOfCol(0, "addr")
//
//		let name = PCA9685Defs[i, idxName]
//		let i2c = getMt(PCA9685Defs, i, idxI2c)
//		let addr = getMt(PCA9685Defs, i, idxAddr)
//
//		println("Create a PCA9685 hard-Var: ${name}, i2c: ${i2c}, path: ${addr}")
//
//		let i2c_HardVar = map[i2c]	/* 这个I2C硬件变量的引用通过闭包保存在该PCA9685及MtVars_PCA9685中 */
//		let obj = JsonObject()
//		obj.put("mode", JsonString("get_fd"))
//		let ret = i2c_HardVar.play(obj)
//		let fd = Int32(ret.asObject().get("fd").getOrThrow().asInt().getValue())
//		let pca9685 = PCA9685(
//			//i2c_setAddr: {addr =>
//			//	let obj = JsonObject()
//			//	obj.put("mode", JsonString("set_addr"))
//			//	obj.put("addr", JsonInt(Int64(addr)))
//			//	i2c_HardVar.play(obj)},
//			//i2c_readReg: {reg: UInt8 =>
//			//	let obj = JsonObject()
//			//	obj.put("mode", JsonString("get_val"))
//			//	obj.put("reg", JsonInt(Int64(reg)))
//			//	let res = i2c_HardVar.play(obj)
//			//	let val = res.asObject().get("value").getOrThrow().asInt().getValue()
//			//	return Int8(val)},
//			//i2c_writeReg: {reg, val =>
//			//	let obj = JsonObject()
//			//	obj.put("mode", JsonString("set_val"))
//			//	obj.put("reg", JsonInt(Int64(reg)))
//			//	obj.put("val", JsonInt(Int64(val)))
//			//	i2c_HardVar.play(obj)},
//			fd,
//			addr: UInt8.parse(addr)
//		)
//
//		/* {"channel": 0, "angle": 90.0}  */
//		let mtvars_pca9685 = MtVars_PCA9685(things: {json: JsonValue =>
//			let channel = json.asObject().get("channel").getOrThrow().asInt().getValue()
//			let angle = json.asObject().get("angle").getOrThrow().asFloat().getValue()
//			pca9685.setAngle(UInt8(channel), Float32(angle))
//			let ret = JsonObject()
//			ret.put("status", JsonString("OK"))
//			return ret
//		},
//			rise: {=>
//				println("HardVar ${name} is setup now.")
//				pca9685.setup()
//			}
//		)
//
//		return mtvars_pca9685
//	}
//}

/* -------- 舵机变量类 -------- 
 * TODO(20260515-qing): 现在普通的舵机类也是传入回调函数的，
 * - 但是我们考虑到最大值、最小值的限制和漂移值的调整，
 * - 还是要专门做一个舵机硬件变量  */
class MtVars_Servo <: MatrixVars_Hardware {
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None  )
		{  super(things: things, rise: rise, free: free)  }

	/*
	 *  @Servo
	 *  name   max  min  offset  controller  channel;
	 *    s1    90    -       -   pca9685-0        0;
	 *  @
	 */
	public static func Factory(	/* 工厂模式：使用矩阵对象构造 */
		ServoDefs: MatrixObject, i: Int64, map: HashMap<String, MatrixVars_Hardware>)//: MtVars_Servo
	{
		let idxName = ServoDefs.indexOfCol(0, "name")
		let idxMax = ServoDefs.indexOfCol(0, "max")
		let idxMin = ServoDefs.indexOfCol(0, "min")
		let idxOffset = ServoDefs.indexOfCol(0,"offset")
		let idxCtrl = ServoDefs.indexOfCol(0, "controller")
		let idxChannel = ServoDefs.indexOfCol(0, "channel")

		let name = ServoDefs[i, idxName]
		let max = parseMt<Float32>(ServoDefs, i, idxMax)
		let min = parseMt<Float32> (ServoDefs,i, idxMin)
		let offset = parseMt<Float32>(ServoDefs, i, idxOffset)
		let ctrl = getMt(ServoDefs, i, idxCtrl)
		let channel = parseMt<UInt8>(ServoDefs, i, idxChannel)

		print("Create a Servo hard-Var: ${name}, max: ${max}, min: ${min}, offset: ${offset}")
		println(", controller: ${ctrl}, channel: ${channel}")

		let controller = map[ctrl]	/* like pca9685 */
		let servo = Servo(max: max, min: min, offset: offset,
			callback_setAngle: {angle: Float32 =>
				let obj = JsonObject()
				obj.put("channel", JsonInt(Int64(channel)))
				obj.put("angle", JsonFloat(Float64(angle)))
				controller.play(obj)
		})

		/*
		 * {"angle": 180.0}
		 */
		let mtvars_servo = MtVars_Servo(things: {json=>
				let angle = json.asObject().get("angle").getOrThrow().asFloat().getValue()
				servo.setAngle(Float32(angle))
				let ret = JsonObject()
				ret.put("status", JsonString("OK"))
				return ret
		})
		return mtvars_servo
	}
}

/*
 * 扬声器类
 * - 使用工厂函数
 * - TODO: 输入扬声器设备名（扬声器又是一台戏）
 */
class MtVars_Speaker <: MatrixVars_Hardware {
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None  )
		{  super(things: things, rise: rise, free: free)  }

	/*
	 *  @Speaker
	 *  name  channels  sample_rate  volumn  max_f  min_f;
	 *   spk         2        48000     0.6   5000      -;
	 *  @
	 */
	public static func Factory(	/*  工厂函数，根据矩阵对象进行构造  */
		SpeakerDefs: MatrixObject, i: Int64, map: HashMap<String, MatrixVars_Hardware>): MtVars_Speaker {

		let idxName = SpeakerDefs.indexOfCol(0, "name")
		let idxChannels = SpeakerDefs.indexOfCol(0, "channels")
		let idxSampleRate = SpeakerDefs.indexOfCol(0, "sample_rate")
		let idxVolumn = SpeakerDefs.indexOfCol(0, "volumn")
		let idxMax_f = SpeakerDefs.indexOfCol(0, "max_f")
		let idxMin_f = SpeakerDefs.indexOfCol(0, "min_f")

		let name = SpeakerDefs[i, idxName]
		let channels = parseMt<Int32>(SpeakerDefs, i, idxChannels)
		let sample_rate = parseMt<Int32>(SpeakerDefs, i, idxSampleRate)
		let volume = parseMt<Float64>(SpeakerDefs, i, idxVolumn)
		let max_f = parseMt<Float32>(SpeakerDefs, i, idxMax_f)
		let min_f = parseMt<Float32>(SpeakerDefs, i, idxMin_f)

		println("Create a Speaker: ${name}, channels: ${channels}, sample_rate: ${sample_rate}, volume: $(volume), max_f: ${max_f}, min_f: ${min_f}")		
		let spk = Spk(channels: channels, sample_rate: sample_rate)
		let mtvar_spk = MtVars_Speaker(things: {json =>		// {"frequency": 1000.0, "delay": 400}
			let json_obj = json.asObject()
			var freq  = Float32(json_obj.get("frequency").getOrThrow().asFloat().getValue())
			if (freq > max_f) { freq = max_f }
			if (freq < min_f) { freq = min_f }
			let delay  = Int32(json_obj.get("delay").getOrThrow().asFloat().getValue())
			spk.play(freq, volume, delay)
			/* ---- 准备返回值 ---- */
			let ret = JsonObject()	/* FIXME: 处理异常情况 */
			ret.put("status", JsonString("OK"))
			return ret
		})

		return mtvar_spk
	}
}



/* -------- 机器人结构类 --------
 * 机器人结构是动态的，从脚本中解析的 
 * TODO(20260514-qing): 调整为输入MatrixObject
 * TODO(20260515-qing): 需要重写一遍硬件类(done)
 * TODO(20260518-qing): 使用神经网络RNN直接根据动作+传感器脚本生成下一步动作
 * TODO(20260527-qing): 增加硬件模块行为脚本
 * TODO*(20260527-qing): 为@Action表演矩阵对象增加位置指示器，以增量行动  */
class RobotStruct {

	/* -------- 存放解析结果和创建的变量 -------- */
	private let C = LinkedList<MatrixObject>()
	private let M = HashMap<String, MatrixVars_Hardware>() 

	/* -------- 使用矩阵脚本驱动机器人 --------
	 *  不行就将矩阵脚本转化为bot脚本吧  */
	public init(input: InputStream) {
		readMatrix(input)
		createHardVars()
	}

	/* -------- 激活所有硬件变量 -------- */
	public func  active() {
		for (item in M.values()) {
			item.build()
		}
	}

	/* -------- 消除所有硬件变量 --------
	 * INFO(20260517-qing): 它需要以倒序方式进行清除  */
	public func deactive() {
		let items = M.values().toArray()
		for (i in items.size-1..=0:-1) {
			items[i].destroy()
		}
	}

	/* -------- 执行@Action矩阵中的内容 -------- */
	public func run() {
		//run_drl_action()
	}

	/* -------- 储存整个矩阵变量表 --------
	 * NOTE(20260517-qing): 使用完毕要关闭输出流  */
	public func save(out: OutputStream) {
		let ms = MatrixSaver(out)
		ms.save(C)
	}

	/* -------- 阅读矩阵脚本 --------
	 * TODO(20260504-qing): 使用矩阵脚本存储神经网络模型  */
	private func readMatrix(file: InputStream){
		let sr = StringReader<InputStream>(file)
		let parser =  MatrixParser(sr)
		while (true) {
			if (let Some(O) <- parser.parse()) {
				let n = O.getName();
				println(n)
				C.addLast(O)  }
			else { break }
		}
	}

	/* -------- 创建硬件变量字典 --------
	 * TODO(20260515-qing): 解耦合  
	 * FIXME(20260518-qing): 在插入哈希表之前如果有已经存在的键值，要释放它的资源  */
	private func createHardVars()
	{

		for (matrix in C) {
			let MtName = matrix.getName()

			/* ---- 【特殊的】I2C硬件 ---- */
			if (MtName == "I2C") {
				let idxName = matrix.indexOfCol(0,"name")
				for (i in 1..matrix.size) {
					let name = matrix[i, idxName]
					M[name] = MtVars_I2C.Factory(matrix, i, M)
				}
			}

			/* ---- 【特殊的】Alias别名硬件 ---- */
			else if (MtName == "Alias") {
				let idxName =  matrix.indexOfCol(0, "name")
				for (i in 1..matrix.size) {
					let name =  matrix[i, idxName]
					M[name] = MtVars_Alias.Factory(matrix, i, M)
				}
			}

			/* ---- 【特殊的】Servo舵机硬件 ---- */
			else if (MtName == "Servo") {
				let idxName = matrix.indexOfCol(0, "name")
				for (i in 1..matrix.size) {
					let name = matrix[i, idxName]
					M[name] = MtVars_Servo.Factory(matrix, i, M)
				}
			}

			/* ---- [特殊的] Action矩阵---- */
			else if (MtName == "Action") {
				;
			}

			/* ---- 【通用的】Lua脚本硬件 ---- */
			else if (matrix.size > 0) {
				let idxName = matrix.indexOfCol(0, "name")
				for (i in 1..matrix.size ) {
					let obj = JsonObject()
					let name = matrix[i, idxName]
					for (j in 0..matrix.col) {
						let colName = matrix[0, j]
						let value = MatrixVars_Hardware.getMt(matrix, i, j)
						/* ---- FIXME: 加入点号别名的检索逻辑 ---- */
						if (let Some(mt) <- M.get(value)) {
							/*---- 检测出存在该名称的变量，传入空对象参数 ----
							 *----   求取返回值  ---- */
							let json = JsonObject()
							let res = mt.play(json).asObject()
							let fields: HashMap<String, JsonValue> = res.getFields()
							for (k in fields.keys()) {
								let val = fields.get(k).getOrThrow()
								obj.put(k, val)
							}
								
						}
						else {
							obj.put(colName, JsonString(value))
						}
					}
					/* ---- fixme: 这里的脚本路径是写死的，随后我们需要设计一个机制 ---- */
					M[name] = MtVars_Lua.Factory("./modules/${MtName}.lua", obj)
				}
			}
		}
	}

	/* -------- 创建别名硬件变量 -------- */
	//private func createAliasVars() {
	//	let AliasDefs = findMt(C, "Alias").getOrThrow()	/* MatrixObject */
	//	for (i in 1..AliasDefs.size) {
	//		let idxName = AliasDefs.indexOfCol(0, "name")
	//		let name = AliasDefs[i, idxName]
	//		M[name] = MtVars_Alias.Factory(AliasDefs, i, M)
	//	}
	//}

	/* -------- 创建I2C机组硬件变量 -------- */
	//private func createI2CVars() {
	//	let I2CDefs= findMt(C, "I2C").getOrThrow()	/* MatrixObject */
	//	for (i in 1..I2CDefs.size) {
	//		let idxName = I2CDefs.indexOfCol(0, "name")
	//		let name = I2CDefs[i, idxName]
	//		M[name]  = MtVars_I2C.Factory(I2CDefs, i, M)
	//	}
	//	}

	/* -------- 创建MPU6050_BIND硬件变量 -------- */
	//private func createMPU6050Bind() {
	//	let MPU6050BindDefs = findMt(C, "MPU6050_BIND").getOrThrow()
	//	for (i in 1..MPU6050BindDefs.size) {
	//		let idxName = MPU6050BindDefs.indexOfCol(0, "name")
	//		let name: String = MPU6050BindDefs[i, idxName]
	//		M.add(name, MtVars_MPU6050_BIND.Factory(MPU6050BindDefs, i, M))
	//	}
	//}

	/* -------- 创建PCA9685硬件变量 -------- */
//	private func createPCA9685Vars() {
//		let PCA9685Defs = findMt(C, "PCA9685").getOrThrow()
//		for (i in 1..PCA9685Defs.size) {
//			let idxName = PCA9685Defs.indexOfCol(0, "name")
//			let name = PCA9685Defs[i, idxName]
//			M[name]=  MtVars_PCA9685.Factory(PCA9685Defs, i, M)
//		}
//	}

	/* -------- 创建Servo舵机硬件变量 -------- */
	//private func createServoVars() {
	//	let ServoDefs = findMt(C, "Servo").getOrThrow()
	//	for (i in 1..ServoDefs.size) {
	//		let idxName = ServoDefs.indexOfCol(0, "name")
	//		let name = ServoDefs[i, idxName]
	//		M[name] = MtVars_Servo.Factory(ServoDefs, i, M)
	//	}
	//}


	/* -------- 创建扬声器机组硬件变量 -------- */
	//private func createSpeakerVars() {
	//	let SpeakerDefs = findMt(C, "Speaker").getOrThrow()	/* MatrixObject */
	//	for (i in 1..SpeakerDefs.size) {
	//		let idxName = SpeakerDefs.indexOfCol(0, "name")
	//		let name = SpeakerDefs[i, idxName]
	//		M[name]= MtVars_Speaker.Factory(SpeakerDefs, i, M)
	//	}
	//}

	/* -------- 在矩阵对象的容器中寻找目标 -------- */
	private static func findMt(ll: LinkedList<MatrixObject>, name: String): Option<MatrixObject>
	{
		for (item in ll) {
			var target  = item.getName()
			target = target.trimAscii() // ???
			if (name == target) {
				return item  }
		}

		return None
	}

	/* -------- 使用drl微调动作，使用RNN生成动作流 -------- */
	private func run_drl_action() {
		let ActionDefs = findMt(C, "Action").getOrThrow()	/* MatrixObject */
		for (i in 1..ActionDefs.size) {
			let set =JsonObject()		/* 每个硬件变量的参数集合 */
			for (j in 0..ActionDefs.col) {
				parse_action_matrixObject(set, ActionDefs, i, j)
			}

			/* -------- 执行一行 -------- */
			let keys = set.getFields().keys()
			for (name_dev in keys) {
				let hv = M[name_dev]
				let params: JsonValue = set.get(name_dev).getOrThrow()
				let _: JsonValue = hv.play(params)
			}


		}
	}

	/* -------- 解析@Action矩阵对象 -------- */
	private func parse_action_matrixObject(set: JsonObject, ActionDefs: MatrixObject,
			i: Int64, j: Int64) {
		let name_HV = ActionDefs[0, j]	/* 获取硬件变量名 FIXME:  该名称不得使用占位符 且不重复 */
		let arr = name_HV.split(".")
		if (arr.size != 2) {
			throw IllegalArgumentException("Action column is not valid")  }
		let name_dev = arr[0]
		var name_param = arr[1]

		var val_fp64 = MatrixVars_Hardware.parseMt<Float64>(ActionDefs, i, j)

		let AliasHV = M.get(name_param)		/* 可能的参数别名转换 */
		if (let Some(aliasHV) <- AliasHV) {
			let ret = aliasHV.play(JsonObject())
			let alias = ret.asString().getValue()
			name_param = alias
		}

		var set_params: JsonObject	/* 设置或者取得属于该硬件变量的参数 */
		if (let Some(obj_) <- set.get(name_dev)) {
			set_params = obj_.asObject()  }
		else {
			set_params = JsonObject()
			set.put(name_dev, set_params)  }

		set_params.put(name_param, JsonFloat(val_fp64))	/* 设置一个该硬件变量的参数 */
	}
}


/* -------- （旧版）从标准输入中读取动作脚本，并进行随机DRL --------
 * FIXME: 为不同的目标设计不同的奖励策略，
 *
 * TODO(qing-20260430): 使用视觉和cnn进行微调
 * 首先使机器人摄像头云台旋转不同角度（在同一位置）
 * 采集图像，然后训练机器人识别画面偏移方向（自监督学习）
 * 用这个模型去训练微调机器人走直线
 *
 * TODO: 专门设计一个机器人类，记录参数。 (done)
 *（可是这样就不通用了）(20260504-qing): 由矩阵式脚本提供通用性
 * 方案二是对于命名的模式给予固定设置
 *
 * TODO: 发声常伴随着延时，所以现在有两个方案
 *   1. cpp发声线程（可叠加声音）。
 *   2. 以发声替代延时。（done）
 *
 * TODO(qing-20260501): 让机器人和神经网络自检（自行对比是否准确）
 *
 * TODO(qing-20260502): 硬件变量、软（数值）变量，使用箭头加大括号？  */
//func run_action_drl(input: InputStream, nns: ArrayList<Nn>) {
//	let sr = StringReader<InputStream>(input)
//	let parser = ActsParser(sr)
//	let i2c = I2C(I2C_PATH)
//	let pca9685 = PCA9685(i2c_setAddr: {addr => i2c.setAddr(addr)},
//		i2c_readReg: {reg => i2c.readRegFlag(reg)},
//		i2c_writeReg:{reg, val => i2c.writeReg(reg, val)})
//	let mpu6050s = [
//		MPU6050(i2c, MPU6050.ADDR_1),
//		MPU6050(i2c, MPU6050.ADDR_0)
//	]
//	var dataSet = ArrayList<MPU6050_sensorData> ()
//	let xSet = ArrayList<Matrix>() /* 输入 */
//	let tSet = ArrayList<Matrix>() /* 教师 */
//	let rSet = ArrayList<Float32>() /* 奖励 */
//	while(true) {
//		/* 解析一行，没了就退出循环 */
//		match(parser.parse()) {
//			case Some(_) => true
//			case None => break
//		}
//
//		let x = Matrix(1, 41) /* 4个3轴加速度和角速度，16个关节，1个时延 */
//		unsafe {
//
//			let r = parser.which(parser.size-1) /* 取尾部 */
//
//			for (j in 0..r.size) {	/* 整理出输入 */
//				x.set(0u64, UInt64(j), Float32(r[j]) / 180.0f32)
//			}
//
//			for (j in 0..dataSet.size) {	/* 陀螺仪部分 */
//				let idx = UInt64(r.size) + UInt64(j) * 6u64
//				x.set(0u64, idx + 0u64, Float32(dataSet[j].accel_x) / 65535.0f32)
//				x.set(0u64, idx + 1u64, Float32(dataSet[j].accel_y) / 65535.0f32)
//				x.set(0u64, idx + 2u64, Float32(dataSet[j].accel_x) / 65535.0f32)
//				x.set(0u64, idx + 3u64, Float32(dataSet[j].gyro_x) / 65535.0f32)
//				x.set(0u64, idx + 4u64, Float32(dataSet[j].gyro_y) / 65535.0f32)
//				x.set(0u64, idx + 5u64, Float32(dataSet[j].gyro_z)  / 65535.0f32)
//			}
//
//			/* -------- 推理 -------- */
//			let y = forward(nns, x)
//
//			/* -------- 微调 -------- */
//			let random = Random(UInt64(DateTime.now().nanosecond))
//			for (j in 0..y.getCol()-1) {
//				let a = y.get(0, j)
//				let fix = (a - 0.5f32 + (random.nextFloat32() - 0.5f32) * 0.2f32)
//				r[Int64(j)] += fix * 180.0f32 * 0.4
//				y.set(0, j, fix) // 回写
//			}
//
//			/* -------- 记录 -------- */
//			xSet.add(x)
//			tSet.add(y)
//			rSet.add(0.0f32)
//
//			/* -------- 解析 -------- */
//			let rp = parser[parser.size - 1]
//			for (key in rp.keys()) {
//				print("${key}\t")
//			}
//			println()
//
//			/* -------- 做动作 -------- */
//			for (jj in 1..=10) { /* fixme: 不写死 */
//				let k = "s${jj}"
//				let v = rp[k]
//				let id = UInt8(jj) - 1u8
//				let val = Float32(v.getVal().getOrThrow())
//				pca9685.setAngle(id, val)
//			}
//
//
//			println()
//			sleep(Duration.millisecond*1)
//
//			/* -------- 读mpu6050 -------- */
//			dataSet = ArrayList<MPU6050_sensorData>()
//			for (mpu6050 in mpu6050s) {
//				dataSet.add(mpu6050.readSensor())
//			}
//
//			/* -------- 产生奖励 --------
//			 * 前进奖励策略  */
//			for (j in 0..dataSet.size) {
//				if (xSet.size == 1) { continue }
//				let pre = xSet[xSet.size-2].get(0, UInt64(r.size) + UInt64(j) * 6u64 + 0u64)
//				let aa = Float32(dataSet[j].accel_x)
//				let bb = pre * 65535.0f32
//				var rewards = if (j%2==0) { bb - aa } else { aa - bb }
//				rewards /= 10000.0f32
//				println("rewards:\t${rewards}")
//				for (k in rSet.size-1..0:-1) {
//					//xSet[k].print()
//					rSet[k] += rewards
//					rewards *= 0.9
//				}
//			}
//
//			/* -------- 时延 -------- */
//			let freq = rp['spk'].getVal().getOrThrow()
//			let tt = rp['delay'].getVal().getOrThrow()
//			if (freq == 0.0f32) {
//				sleep(Duration.millisecond * Int64(tt * 1000.0f32) / 1000)
//			}
//			else {
//				let spk = Spk(channels: 2i32, sample_rate: 48000i32)
//				spk.play(freq, 0.6, Int32(tt))	/* 以发声代替延时 */
//			}
//		}
//
//	}
//
//	/* -------- 打印损失 -------- */
//	for (i in 0..xSet.size) {
//		println("----------------------------------")
//		println(rSet[i])
//		let loss = train(nns, xSet[i], tSet[i], rSet[i])
//		println("loss:\t${loss}")
//	}
//
//	/* -------- 选择性储存 -------- */
//	if (let Some(P) <- NNS_OUT) {
//		save_nns(nns, P)
//	}
//}
