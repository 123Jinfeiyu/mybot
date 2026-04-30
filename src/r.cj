/* 机器人构成模块 */

package bot4cj
import std.io.*
import std.time.*
import std.random.*
import std.collection.*



/* 从标准输入中读取动作脚本 */
//func run_action(consoleReader: InputStream, _: ArrayList<Nn>) {
//	let parser = ActsParser(consoleReader)
//	let actor = Actor(I2C_PATH)
//	for (i in 0..parser.size) {
//		actor.play(parser[i])
//	}
//}

/* 从标准输入中读取动作脚本，并进行随机DRL
 * FIXME: 为不同的目标设计不同的奖励策略，
 *
 * TODO(qing-20260430): 使用视觉和cnn进行微调
 * 首先使机器人摄像头云台旋转不同角度（在同一位置）
 * 采集图像，然后训练机器人识别画面偏移方向（自监督学习）
 * 用这个模型去训练微调机器人走直线
 *
 * TODO: 专门设计一个机器人类，记录参数。
 *（可是这样就不通用了）
 * 方案二是对于命名的模式给予固定设置
 *
 * TODO: 发声常伴随着延时，所以现在有两个方案
 *   1. cpp发声线程（可叠加声音）。
 *   2. 以发声替代延时。 */
func run_action_drl(consoleReader: InputStream, nns: ArrayList<Nn>) {
	let parser = ActsParser(consoleReader)
	let actor = Actor(I2C_PATH)
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
	for (i in 0..parser.size) {
		let x = Matrix(1, 41) // 4个3轴加速度和角速度，16个关节，1个时延
		unsafe {

			let r = parser.which(i)

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
			let rp = parser[i]

			// 做动作
			for (jj in 0..10) {
				let k = "s${jj}"
				let v = rp[k]
				let id = UInt8(jj)
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
			let spk = Spk(channels: 2i32, sample_rate: 48000i32)
			spk.play(freq, 0.2, Int32(tt))	// 以发声代替延时
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
