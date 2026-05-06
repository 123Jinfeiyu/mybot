/* 机器人构成模块 */

package bot4cj
import std.io.*
import std.time.*
import std.random.*
import std.collection.*
import std.convert.*
import stdx.encoding.json.*

/* 阅读矩阵脚本
 * TODO(20260504-qing): 使用矩阵脚本存储神经网络模型 */
func readMatrix(file: InputStream): LinkedList<MatrixObject> {
	let sr = StringReader<InputStream>(file)
	let parser =  MatrixParser(sr)
	let C = LinkedList<MatrixObject>()
	while (true) {
		if (let Some(O) <- parser.parse()) {
			let n = O.getName();
			println(n)
			C.addLast(O)
		}
		else { break }
	}

	return C
}

// 在矩阵对象的容器中寻找目标
func findMt(objs: LinkedList<MatrixObject>, name: String) {
	for (item in objs) {
		var target  = item.getName()
		target = target.trimAscii() // ???
		if (name == target) {
			return item
		}
	}

	// 什么都没找到，抛出异常
	throw IllegalArgumentException("Matrix object not found")
}

// 解析项目值
func parseMt<T>(obj: MatrixObject, row: Int64, col: Int64) where T <: Parsable<T>
{
	let raw = obj[row, col]
	if (raw == "-" && row > 1) {
		let prev = obj[row-1, col]
		return T.parse(prev)
	}
	else if (raw == "-" && !row > 1) {
		return T.parse("0")
	}
	
	// else if (!raw == "-")
	return T.parse(raw)
}


// 创建硬件变量字典
func createHardVars(objs: LinkedList<MatrixObject>): HashMap<String, IHardware> {
	let M = HashMap<String, IHardware> ()
	createSpeakerVars(objs, M)
	return M
}

// 创建舵机机组硬件变量
func createSpeakerVars(objs: LinkedList<MatrixObject>, M: HashMap<String, IHardware>)
{
	let SpeakerDefs = findMt(objs, "Speaker")	// MatrixObject
	let idxName = SpeakerDefs.indexOfCol(0, "name")
	let idxChannels = SpeakerDefs.indexOfCol(0, "channels")
	let idxSampleRate = SpeakerDefs.indexOfCol(0, "sample_rate")
	let idxVolumn = SpeakerDefs.indexOfCol(0, "volumn")
	let idxMax_f = SpeakerDefs.indexOfCol(0, "max_f")
	let idxMin_f = SpeakerDefs.indexOfCol(0, "min_f")
	for (i in 1..SpeakerDefs.size) {
		let name = SpeakerDefs[i, idxName]
		let channels = parseMt<Int32>(SpeakerDefs, i, idxChannels)
		let sample_rate = parseMt<Int32>(SpeakerDefs, i, idxSampleRate)
		let volume = parseMt<Float32>(SpeakerDefs, i, idxVolumn)
		let max_f = parseMt<Float32>(SpeakerDefs, i, idxMax_f)
		let min_f = parseMt<Float32>(SpeakerDefs, i, idxMin_f)
		let spk = Spk(channels: channels, sample_rate: sample_rate)
		let interf_spk = Interf_Speaker({json => 
			let json_obj = json.asObject()
			var freq  = Float32(json_obj.get("frequency").getOrThrow().asFloat().getValue())
			if (freq > max_f) { freq = max_f }
			if (freq < min_f) { freq = min_f }
			let volu  = Float64(json_obj.get("volume").getOrThrow().asFloat().getValue())
			let delay  = Int32(json_obj.get("delay").getOrThrow().asFloat().getValue())
			spk.play(freq, volu, delay)
			0.0f32
		})
		M[name]= interf_spk
		println("Create a Speaker: ${name}")
	}
}

/* 使用矩阵脚本驱动机器人 */
func runWithMt(input: InputStream) {
	let ll = readMatrix(input)
	let map = createHardVars(ll)
} 

// 硬件接口
interface IHardware {
	public func play(input: JsonValue): Float32
}

// 舵机类
class Interf_Servo <: IHardware {
	init(things: (JsonValue) -> Float32) {
		this.things = things
	}
	let things: (JsonValue) -> Float32

	public func play(input: JsonValue): Float32 {
		return things(input)
	}
}

// 扬声器类
class Interf_Speaker <: IHardware {
	init(things: (JsonValue) -> Float32) {
		this.things = things
	}
	let things: (JsonValue) -> Float32

	public func play(input: JsonValue): Float32 {
		return things(input)
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
	let pca9685 = PCA9685(i2c)
	let mpu6050s = [
		MPU6050(i2c, MPU6050.ADDR_1),
		MPU6050(i2c, MPU6050.ADDR_0)
	]
	var dataSet = ArrayList<MPU6050_sensorData> ()
	let xSet = ArrayList<Matrix>() // 输入
	let tSet = ArrayList<Matrix>() // 教师
	let rSet = ArrayList<Float32>() // 奖励
	while(true) {
		// 解析一行，没了就退出循环
		match(parser.parse()) {
			case Some(_) => true
			case None => break
		}

		let x = Matrix(1, 41) // 4个3轴加速度和角速度，16个关节，1个时延
		unsafe {

			let r = parser.which(parser.size-1) // 取尾部

			// 整理出输入
			for (j in 0..r.size) {
				x.set(0u64, UInt64(j), Float32(r[j]) / 180.0f32)
			}

			// 陀螺仪部分
			for (j in 0..dataSet.size) {
				let idx = UInt64(r.size) + UInt64(j) * 6u64
				x.set(0u64, idx + 0u64, Float32(dataSet[j].accel_x) / 65535.0f32)
				x.set(0u64, idx + 1u64, Float32(dataSet[j].accel_y) / 65535.0f32)
				x.set(0u64, idx + 2u64, Float32(dataSet[j].accel_x) / 65535.0f32)
				x.set(0u64, idx + 3u64, Float32(dataSet[j].gyro_x) / 65535.0f32)
				x.set(0u64, idx + 4u64, Float32(dataSet[j].gyro_y) / 65535.0f32)
				x.set(0u64, idx + 5u64, Float32(dataSet[j].gyro_z)  / 65535.0f32)
			}

			// 推理
			let y = forward(nns, x)

			// 微调		
			let random = Random(UInt64(DateTime.now().nanosecond))
			for (j in 0..y.getCol()-1) {
				let a = y.get(0, j)
				let fix = (a - 0.5f32 + (random.nextFloat32() - 0.5f32) * 0.2f32)
				r[Int64(j)] += fix * 180.0f32 * 0.4
				y.set(0, j, fix) // 回写
			}

			// 记录
			xSet.add(x)
			tSet.add(y)
			rSet.add(0.0f32)

			// 解析
			let rp = parser[parser.size - 1]
			for (key in rp.keys()) {
				print("${key}\t")
			}
			println()

			// 做动作
			for (jj in 1..=10) { // fixme: 不写死
				let k = "s${jj}"
				let v = rp[k]
				let id = UInt8(jj) - 1u8
				let val = Float32(v.getVal().getOrThrow())
				pca9685.setAngle(id, val)
			}


			println()
			sleep(Duration.millisecond*1)

			// 读mpu6050
			dataSet = ArrayList<MPU6050_sensorData>()
			for (mpu6050 in mpu6050s) {
				dataSet.add(mpu6050.readSensor())
			}

			// 产生奖励
			// 前进奖励策略
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

			// 时延
			let freq = rp['spk'].getVal().getOrThrow()
			let tt = rp['delay'].getVal().getOrThrow()
			if (freq == 0.0f32) {
				sleep(Duration.millisecond * Int64(tt * 1000.0f32) / 1000)
			}
			else {
				let spk = Spk(channels: 2i32, sample_rate: 48000i32)
				spk.play(freq, 0.6, Int32(tt))	// 以发声代替延时
			}
		}

	}

	// 打印损失
	for (i in 0..xSet.size) {
		println("----------------------------------")
		println(rSet[i])
		let loss = train(nns, xSet[i], tSet[i], rSet[i])
		println("loss:\t${loss}")
	}

	// 选择性储存
	if (let Some(P) <- NNS_OUT) {
		save_nns(nns, P)
	}
}
