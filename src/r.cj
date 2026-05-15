/* 机器人构成模块
 * INFO(20260514-qing): 该模块确定机器人结构，为解耦考虑应该从解析出的矩阵对象中构建 */

package bot4cj
import std.io.*
import std.time.*
import std.random.*
import std.collection.*
import std.convert.*
import stdx.encoding.json.*



/* 硬件接口
 * 接收一个lambda表达式，在回调函数中定义行为
 * 通过JsonValue这种通用的数据结构进行传值  */
open class MatrixVars_Hardware {
	init(things: (JsonValue) -> JsonValue) {
		this.things = things  }

	let things: (JsonValue) -> JsonValue	/* 该事件回调应该接收和返回JsonValue  */

	public func play(input: JsonValue): JsonValue {
		return things(input)  }


	/* INFO(20260514-qing):  这两个函放在外面很突兀，考虑放进Matrix_HardVars */
	/* -------- 获得项目值 -------- */
	static func getMt(obj: MatrixObject, row: Int64, col: Int64) 
	{
		let raw = obj[row, col]
		if (raw == "-" && row > 1) {
			return obj[row - 1, col]  }
		return raw
	}

	/* -------- 解析项目值 -------- */
	static func parseMt<T>(obj: MatrixObject, row: Int64, col: Int64) where T <: Parsable<T>
	{
		let raw = obj[row, col]
		if (raw == "-" && row > 1) {
			let prev = obj[row-1, col]
			return T.parse(prev)
		}
		else if (raw == "-" && !row > 1) {
			return T.parse("0")  }

		// else if (!raw == "-")
		return T.parse(raw)
	}
}


/* I2C变量类 */
class MtVars_I2C <: MatrixVars_Hardware {
	init(things: (JsonValue) -> JsonValue) {
		super(things)  }

	/*
	 *  工厂模式，根据矩阵对象构造
	 * 
	 *  @I2C
	 *   name        path;
	 *  i2c-0  /dev/i2c-0;
	 *  @
	 */
	static func Factory(I2CDefs: MatrixObject, i: Int64, _: HashMap<String, MatrixVars_Hardware>): MtVars_I2C
	{
		let idxName  = I2CDefs.indexOfCol(0, "name")
		let idxPath  = I2CDefs.indexOfCol(0, "path")

		let name = I2CDefs[i, idxName]
		let path = getMt(I2CDefs, i, idxPath)

		println("Create a I2C hard-Var: ${name},\tpath: ${path}")
		let i2c = I2C(path)
		let mtvars_i2c = MtVars_I2C({json: JsonValue =>				/*  {"mode": "set_addr", "addr": 0x01} */
			let obj = json.asObject()					/*  {"mode": "set_val",  "reg": 0x01, "val": 0x56} */
			let mode = obj.get("mode").getOrThrow().asString().getValue()	/*  {"mode": "get_val",  "reg": 0x02} */
			let ret = JsonObject()	/* 该变量用于存储返回值 */
			if (mode == "set_addr") {
				let addr = obj.get("addr").getOrThrow().asInt().getValue()
				i2c.setAddr(UInt8(addr))
				/* ---- 准备返回值 ---- */
				ret.put("status", JsonString("OK"))
				return ret
			} else if (mode == "set_val") {
				let reg = obj.get("reg").getOrThrow().asInt().getValue()
				let val = obj.get("val").getOrThrow().asInt().getValue()
				i2c.writeReg(UInt8(reg), UInt8(val))
				/* ---- 准备返回值 ---- */
				ret.put("status", JsonString("OK"))
				return ret
			} else if (mode == "get_val") {
				let reg = obj.get("reg").getOrThrow().asInt().getValue()
				let raw = i2c.readRegFlag(UInt8(reg))
				/* ---- 准备返回值 ---- */
				ret.put("status", JsonString("OK"))
				ret.put("value", JsonInt(Int64(raw)))
				return ret
			}

			/* FIXME: 改为抛出异常 */
			/* ---- 准备返回值 ---- */
			ret.put("status", JsonString("ERR"))
			ret.put("message", JsonString("Invalid Mode"))
			return ret
		})

		return mtvars_i2c
	}
}

/*  PCA9685变量类 */
class MtVars_PCA9685 <: MatrixVars_Hardware {
	init(things: (JsonValue) -> JsonValue) {
		super(things)  }

	/*  工厂模式，根据矩阵对象构造，并在哈系变量表中搜索相关依赖
	 *  @PCA9685
	 *       name    I2C   path;
	 *  pca9685-0  i2c-1   0x40;
	 *  @
	 */
	static func Factory(PCA9685Defs: MatrixObject, i: Int64, map: HashMap<String, MatrixVars_Hardware>)//: MtVars_PCA9685
	{
		let idxName = PCA9685Defs.indexOfCol(0, "name")
		let idxI2c  = PCA9685Defs.indexOfCol(0, "I2C")
		let idxPath = PCA9685Defs.indexOfCol(0, "path")

		let name = PCA9685Defs[i, idxName]
		let i2c = getMt(PCA9685Defs, i, idxI2c)
		let path = getMt(PCA9685Defs, i, idxPath)

		println("Create a I2C hard-Var: ${name},\ti2c: ${i2c},\tpath: ${path}")

		let i2c_HardVar = map[i2c]
		let pca9685 = PCA9685(i2c_setAddr: {addr =>
				let obj = JsonObject()
				obj.put("mode", JsonString("set_addr"))
				obj.put("addr", JsonInt(Int64(addr)))
				i2c_HardVar.play(obj)},
			i2c_readReg: {reg: UInt8 =>
				let obj = JsonObject()
				obj.put("mode", JsonString("get_val"))
				obj.put("reg", JsonInt(Int64(reg)))
				let res = i2c_HardVar.play(obj)
				let val = res.asObject().get("value").getOrThrow().asInt().getValue()
				return Int8(val)},
			i2c_writeReg: {reg, val =>
				let obj = JsonObject()
				obj.put("mode", JsonString("set_val"))
				obj.put("reg", JsonInt(Int64(reg)))
				obj.put("val", JsonInt(Int64(val)))
				i2c_HardVar.play(obj)}
			)

		let mtvars_pca9685 = MtVars_PCA9685({json: JsonValue =>
			let channel = json.asObject().get("channel").getOrThrow().asFloat().getValue()
			let angle = json.asObject().get("angle").getOrThrow().asFloat().getValue()
			pca9685.setAngle(UInt8(channel), Float32(angle))
			let ret = JsonObject()
			ret.put("status", JsonString("OK"))
			return ret
		})

		return mtvars_pca9685
	}
}

/* 舵机变量类 */
class MtVars_Servo <: MatrixVars_Hardware {
	init(things: (JsonValue) -> JsonValue) {
		super(things)  }
}

/*
 * 扬声器类
 * - 使用工厂函数
 * - TODO: 输入扬声器设备名（扬声器又是一台戏）
 */
class MtVars_Speaker <: MatrixVars_Hardware {
	init(things: (JsonValue) -> JsonValue) {
		super(things)  }

	/*
	 * 工厂函数，根据矩阵对象进行构造
	 *
	 *  @Speaker
	 *  name  channels  sample_rate  volumn  max_f  min_f;
	 *   spk         2        48000     0.6   5000      -;
	 *  @
	 */
	static func Factory(SpeakerDefs: MatrixObject, i: Int64, map: HashMap<String, MatrixVars_Hardware>): MtVars_Speaker
	{
		let idxName = SpeakerDefs.indexOfCol(0, "name")
		let idxChannels = SpeakerDefs.indexOfCol(0, "channels")
		let idxSampleRate = SpeakerDefs.indexOfCol(0, "sample_rate")
		let idxVolumn = SpeakerDefs.indexOfCol(0, "volumn")
		let idxMax_f = SpeakerDefs.indexOfCol(0, "max_f")
		let idxMin_f = SpeakerDefs.indexOfCol(0, "min_f")

		let name = SpeakerDefs[i, idxName]
		let channels = parseMt<Int32>(SpeakerDefs, i, idxChannels)
		let sample_rate = parseMt<Int32>(SpeakerDefs, i, idxSampleRate)
		let volume = parseMt<Float32>(SpeakerDefs, i, idxVolumn)
		let max_f = parseMt<Float32>(SpeakerDefs, i, idxMax_f)
		let min_f = parseMt<Float32>(SpeakerDefs, i, idxMin_f)

		println("Create a Speaker: ${name}")		
		let spk = Spk(channels: channels, sample_rate: sample_rate)
		let mtvar_spk = MtVars_Speaker({json =>		// {"frequency": 1000.0, "volume": 0.6, "delay": 400}
			let json_obj = json.asObject()
			var freq  = Float32(json_obj.get("frequency").getOrThrow().asFloat().getValue())
			if (freq > max_f) { freq = max_f }
			if (freq < min_f) { freq = min_f }
			let volu  = Float64(json_obj.get("volume").getOrThrow().asFloat().getValue())
			let delay  = Int32(json_obj.get("delay").getOrThrow().asInt().getValue())
			spk.play(freq, volu, delay)
			/* ---- 准备返回值 ---- */
			let ret = JsonObject()	/* FIXME: 处理异常情况 */
			ret.put("status", JsonString("OK"))
			return ret
		})

		return mtvar_spk
	}
}



/* 机器人结构类
 * 机器人结构是动态的，从脚本中解析的 
 * TODO(20260514-qing): 调整为输入MatrixObject
 * TODO(20260515-qing): 需要重写一遍硬件类  */
class RobotStruct {

	/*  存放解析结果和创建的变量 */
	let C = LinkedList<MatrixObject>()
	let M = HashMap<String, MatrixVars_Hardware>() 

	/*  使用矩阵脚本驱动机器人
	 *  不行就将矩阵脚本转化为bot脚本吧  */
	init(input: InputStream) {
		readMatrix(input)
		createHardVars()
	}

	func run() {
		
	}


	/* 阅读矩阵脚本
	 * TODO(20260504-qing): 使用矩阵脚本存储神经网络模型  */
	func readMatrix(file: InputStream){
		let sr = StringReader<InputStream>(file)
		let parser =  MatrixParser(sr)
		while (true) {
			if (let Some(O) <- parser.parse()) {
				let n = O.getName();
				println(n)
				C.addLast(O)
			}
			else {
				break }
		}
	}

	/* -------- 创建硬件变量字典 -------- */
	func createHardVars()
	{
		createI2CVars()
		createPCA9685Vars()
		createSpeakerVars()
	}


	/* -------- 创建I2C机组硬件变量 -------- */
	func createI2CVars()
	{
		let I2CDefs= findMt(C, "I2C").getOrThrow()	/* MatrixObject */
		for (i in 1..I2CDefs.size) {
			let idxName = I2CDefs.indexOfCol(0, "name")
			let name = I2CDefs[i, idxName]
			M[name]  = MtVars_I2C.Factory(I2CDefs, 1, M)
		}
	}

	/* -------- 创建PCA9685硬件变量 -------- */
	func createPCA9685Vars()
	{
		let PCA9685Defs = findMt(C, "PCA9685").getOrThrow()
		for (i in 1..PCA9685Defs.size) {
			let idxName = PCA9685Defs.indexOfCol(0, "name")
			let name = PCA9685Defs[i, idxName]
			M[name]=  MtVars_PCA9685.Factory(PCA9685Defs, 1, M)
		}
	}

	/* -------- 创建扬声器机组硬件变量 -------- */
	func createSpeakerVars()
	{
		let SpeakerDefs = findMt(C, "Speaker").getOrThrow()	/* MatrixObject */
		for (i in 1..SpeakerDefs.size) {
			let idxName = SpeakerDefs.indexOfCol(0, "name")
			let name = SpeakerDefs[i, idxName]
			M[name]= MtVars_Speaker.Factory(SpeakerDefs, i, M)
		}
	}

	/* -------- 在矩阵对象的容器中寻找目标 -------- */
	static func findMt(ll: LinkedList<MatrixObject>, name: String): Option<MatrixObject>
	{
		for (item in ll) {
			var target  = item.getName()
			target = target.trimAscii() // ???
			if (name == target) {
				return item
			}
		}

		//throw IllegalArgumentException("Matrix object not found")
		return None
	}

}


/* 从标准输入中读取动作脚本，并进行随机DRL
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
func run_action_drl(input: InputStream, nns: ArrayList<Nn>) {
	let sr = StringReader<InputStream>(input)
	let parser = ActsParser(sr)
	let i2c = I2C(I2C_PATH)
	let pca9685 = PCA9685(i2c_setAddr: {addr => i2c.setAddr(addr)},
		i2c_readReg: {reg => i2c.readRegFlag(reg)},
		i2c_writeReg:{reg, val => i2c.writeReg(reg, val)})
	let mpu6050s = [
		MPU6050(i2c, MPU6050.ADDR_1),
		MPU6050(i2c, MPU6050.ADDR_0)
	]
	var dataSet = ArrayList<MPU6050_sensorData> ()
	let xSet = ArrayList<Matrix>() /* 输入 */
	let tSet = ArrayList<Matrix>() /* 教师 */
	let rSet = ArrayList<Float32>() /* 奖励 */
	while(true) {
		/* 解析一行，没了就退出循环 */
		match(parser.parse()) {
			case Some(_) => true
			case None => break
		}

		let x = Matrix(1, 41) /* 4个3轴加速度和角速度，16个关节，1个时延 */
		unsafe {

			let r = parser.which(parser.size-1) /* 取尾部 */

			for (j in 0..r.size) {	/* 整理出输入 */
				x.set(0u64, UInt64(j), Float32(r[j]) / 180.0f32)
			}

			for (j in 0..dataSet.size) {	/* 陀螺仪部分 */
				let idx = UInt64(r.size) + UInt64(j) * 6u64
				x.set(0u64, idx + 0u64, Float32(dataSet[j].accel_x) / 65535.0f32)
				x.set(0u64, idx + 1u64, Float32(dataSet[j].accel_y) / 65535.0f32)
				x.set(0u64, idx + 2u64, Float32(dataSet[j].accel_x) / 65535.0f32)
				x.set(0u64, idx + 3u64, Float32(dataSet[j].gyro_x) / 65535.0f32)
				x.set(0u64, idx + 4u64, Float32(dataSet[j].gyro_y) / 65535.0f32)
				x.set(0u64, idx + 5u64, Float32(dataSet[j].gyro_z)  / 65535.0f32)
			}

			/* -------- 推理 -------- */
			let y = forward(nns, x)

			/* -------- 微调 -------- */
			let random = Random(UInt64(DateTime.now().nanosecond))
			for (j in 0..y.getCol()-1) {
				let a = y.get(0, j)
				let fix = (a - 0.5f32 + (random.nextFloat32() - 0.5f32) * 0.2f32)
				r[Int64(j)] += fix * 180.0f32 * 0.4
				y.set(0, j, fix) // 回写
			}

			/* -------- 记录 -------- */
			xSet.add(x)
			tSet.add(y)
			rSet.add(0.0f32)

			/* -------- 解析 -------- */
			let rp = parser[parser.size - 1]
			for (key in rp.keys()) {
				print("${key}\t")
			}
			println()

			/* -------- 做动作 -------- */
			for (jj in 1..=10) { /* fixme: 不写死 */
				let k = "s${jj}"
				let v = rp[k]
				let id = UInt8(jj) - 1u8
				let val = Float32(v.getVal().getOrThrow())
				pca9685.setAngle(id, val)
			}


			println()
			sleep(Duration.millisecond*1)

			/* -------- 读mpu6050 -------- */
			dataSet = ArrayList<MPU6050_sensorData>()
			for (mpu6050 in mpu6050s) {
				dataSet.add(mpu6050.readSensor())
			}

			/* -------- 产生奖励 --------
			 * 前进奖励策略  */
			for (j in 0..dataSet.size) {
				if (xSet.size == 1) { continue }
				let pre = xSet[xSet.size-2].get(0, UInt64(r.size) + UInt64(j) * 6u64 + 0u64)
				let aa = Float32(dataSet[j].accel_x)
				let bb = pre * 65535.0f32
				var rewards = if (j%2==0) { bb - aa } else { aa - bb }
				rewards /= 10000.0f32
				println("rewards:\t${rewards}")
				for (k in rSet.size-1..0:-1) {
					//xSet[k].print()
					rSet[k] += rewards
					rewards *= 0.9
				}
			}

			/* -------- 时延 -------- */
			let freq = rp['spk'].getVal().getOrThrow()
			let tt = rp['delay'].getVal().getOrThrow()
			if (freq == 0.0f32) {
				sleep(Duration.millisecond * Int64(tt * 1000.0f32) / 1000)
			}
			else {
				let spk = Spk(channels: 2i32, sample_rate: 48000i32)
				spk.play(freq, 0.6, Int32(tt))	/* 以发声代替延时 */
			}
		}

	}

	/* -------- 打印损失 -------- */
	for (i in 0..xSet.size) {
		println("----------------------------------")
		println(rSet[i])
		let loss = train(nns, xSet[i], tSet[i], rSet[i])
		println("loss:\t${loss}")
	}

	/* -------- 选择性储存 -------- */
	if (let Some(P) <- NNS_OUT) {
		save_nns(nns, P)
	}
}
