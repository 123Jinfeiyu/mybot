package bot4cj
import std.env.*
import std.argopt.*
import std.fs.*
import std.io.*
import std.time.*
import std.random.*
import std.convert.*
import std.collection.*
import stdx.net.http.*
import stdx.encoding.json.*

var I2C_PATH = "/dev/i2c-1" // For Raspberrypi
var HTTP_ADDR = "127.0.0.1"
var HTTP_PORT = 5119u16
var NNS_OUT: Option<String> = None
main(args: Array<String>): Int64 {
	
	//---------------------------
	// 解析命令行参数
	let specs = [
		ArgumentSpec.Long("help", ArgumentMode.NoValue),
		ArgumentSpec.Long("serve", ArgumentMode.OptionalValue),
		ArgumentSpec.Long("i2c", ArgumentMode.RequiredValue),
		ArgumentSpec.Short(r'i', ArgumentMode.RequiredValue),
		ArgumentSpec.Short(r'o', ArgumentMode.RequiredValue),
		ArgumentSpec.Long("test", ArgumentMode.RequiredValue)
	]
	let parsed = parseArguments(args, specs)
	let options = parsed.options

	//---------------------------
	// 读取设置项
	if (options.contains("i2c")) {
		I2C_PATH = options['i2c']
	}

	if (options.contains("serve")) {
		if (options['serve'] != "") {
			let s = options['serve']
			HTTP_PORT = UInt16.parse(s)
		}
	}

	//---------------------------
	// 初始化神经网络 init nn
	var nns: ArrayList<Nn> = 
	if (options.keys().contains("i")) {
		let filename = options['i']
		load_nns(filename)  }
	else {
		let nn_in = ArrayList<Nn>()
		nn_in.add(Nn(41, 80, ActivationFuncs.Leaky_ReLU, 0.2f32))
		nn_in.add(Nn(80, 40, ActivationFuncs.Leaky_ReLU, 0.2f32))
		nn_in.add(Nn(40, 17, ActivationFuncs.Sigmoid, 0.2f32)) // 16个舵机加1个时延
		nn_in  }

	if (options.contains("o")) {

		NNS_OUT = Option<String>.Some(options['o'])
	}


	//---------------------------
	// 分支选择

	if  (options.contains("help")) {	// 拉起帮助页面

		print_help()
		return 0
	}

	else if (options.contains("test")) {

		run_test(options["test"])
		return 0
	}

	else if (options.contains("serve")) {	// 启动web伺服器
		let savepath  = if (options.contains('o')) {
			options['o']
		} else  { "./nn.txt" } // 获取储存路径
        	run_serve(nns, savepath)
	}

	else {	// 从标准输入获取动作脚本

		let consoleReader = getStdIn()
		run_action_drl(consoleReader, nns)
		if (options.contains("o")) {
			save_nns(nns, options['o'])
		}
	}


	return 0
}

func print_help() {
	println("这是一个机器人动作脚本解析器。")
	println("使用管道输入动作脚本，示例：")
	println("cat std.bot | ./bot")
}

/* 集成多种常规测试、临时测试 */
func run_test(key: String) {

	if (key == "fft") {

		println('FFT test start...')
		var fftt = genWave()
		var fftr = start_fft(fftt)
		println('FFT test completed.')
	}

	else if (key == "ann") {

		let nn_t = ArrayList<Nn>()
		nn_t.add(Nn(2, 4, ActivationFuncs.Leaky_ReLU, 0.4f32))
		nn_t.add(Nn(4, 2, ActivationFuncs.Sigmoid, 0.4f32))
		let batch = 1
		let num = 1000
		let prt_num = 100
		for (i in 0..(num*batch)) {
			let m = Matrix(1, 2)
			let t = Matrix(1, 2)
			prepare_andDoor(m, t)
			let l = train(nn_t, m, t, 1.0f32)
			if (i % prt_num == prt_num-1) {
				print("${i}\t:")
				print("${l}")
				println()
			}
		}
	}

	// 对单个mpu6050进行持续测试
	else if (key == "mpu6050") {

		let i2c = I2C(I2C_PATH)
		let mpu6050 = MPU6050(i2c, MPU6050.ADDR_0)
		while (true) {
			let data = mpu6050.readSensor()
			print("accel_x: ${data.accel_x}\t")
			print("accel_y: ${data.accel_y}\t")
			print("accel_z: ${data.accel_z}\t")
			print("gyro_x: ${data.gyro_x}\t")
			print("gyro_y: ${data.gyro_y}\t")
			println("gyro_z: ${data.gyro_z}")
			sleep(Duration.millisecond * 10)
		}
	}
}

/* http伺服器读取动作脚本
 * TODO: 集成更多的前端托管的功能 */
func run_serve(nns: ArrayList<Nn>, save_path: String) {
	let serv = ServerBuilder()
	.addr(HTTP_ADDR)
	.port(HTTP_PORT)
	.build()

	// curl -X POST /api/v2/bot_run -d ${cat filepath}
	serv.distributor.register('/api/v2/bot_run', { context =>
		if (context.request.method == 'POST') {
			// ---- 动作 ----
			run_action_drl(context.request.body, nns)
			save_nns(nns, save_path)
			// ---- 响应 ---- 
			let headers = HttpHeaders()
			headers.add('Content-Type', 'text/html; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body('OK')
		}
	})

	// curl -X POST /api/v1/fs -d '{"mode": "list", "path": "/path/to/dirent"}'
	serv.distributor.register('/api/v1/list', { context =>
	
		try {
			if (context.request.method != 'POST') {
				throw IllegalArgumentException("Method does not supportted.")
			}
			let json = StringReader(context.request.body).readToEnd()
			let obj = JsonValue.fromStr(json).asObject()
			let mode = obj.get('mode').getOrThrow().asString().getValue()
			if (mode == "list") {
				let path = obj.get('path').getOrThrow().asString().getValue()
				let ret = JsonArray()
				Directory.walk(path, {fileinfo =>
					let obj = JsonObject()
					obj.put("name", JsonString(fileinfo.name))
					if (fileinfo.isDirectory()) {
						obj.put("type", JsonString("Directory"))
					}
					else if (fileinfo.isSymbolicLink()) {
						obj.put("type", JsonString("SymbolicLink"))
					}
					else if (fileinfo.isRegular()) {
						obj.put("type", JsonString("Regular"))
					}
					ret.add(obj)
					return true
				})
				let headers = HttpHeaders()
				headers.add('Content-Type', 'application/json; charset=utf-8;')
				context.responseBuilder.setHeaders(headers)
				context.responseBuilder.body(ret.toJsonString().toArray())
			}
			else {
				throw IllegalArgumentException("Invalid mode.")
			}
		}

		catch (exp: Exception) {
			let headers = HttpHeaders()
			headers.add('Content-Type', 'application/json; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body(exp.toString()) // FIXME: http code
		}

	})

	// curl -X GET / 
	serv.distributor.register('/', { context =>
		try {
			if (context.request.method != 'GET') {
				throw IllegalArgumentException("Method does not supportted.")
			}

			let html = String.fromUtf8(File.readFrom("./public/index.html"))
			let headers = HttpHeaders()
			headers.add("Content-Type", "text/html; charset=utf-8;")
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body(html)  // FIXME: 不读取出全部的字符串，直接放入输入流可不可以
		}

		catch (exp: Exception) {
			let headers = HttpHeaders()
			headers.add('Content-Type', 'application/json; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body(exp.toString()) // FIXME: http code
		}
	})

	println('Bot at ${HTTP_ADDR}:${HTTP_PORT}')
	serv.serve()
}

/* 从标准输入中读取动作脚本，并进行感知训练 */
func run_action(consoleReader: InputStream, _: ArrayList<Nn>) {
	let parser = ActsParser(consoleReader)
	let actor = Actor(I2C_PATH)
	for (i in 0..parser.size) {
		actor.play(parser[i])
	}
}

/* 从标准输入中读取动作脚本，并进行随机DRL
 * FIXME: 为不同的目标设计不同的奖励策略， */
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

			// 做动作
			let rp = parser[i]
			for (jj in 0..rp.size-1) {
				print(rp[jj])
				print(" ")
				pca9685.setAngle(UInt8(jj), Float32(rp[jj]))
			}
			println()
			sleep(Duration.millisecond*1)

			// 读mpu6050
			dataSet = ArrayList<MPU6050_sensorData>()
			dataSet.add(mpu6050s[0].readSensor())
			dataSet.add(mpu6050s[1].readSensor())

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
					print(" ")
					rSet[k] += rewards
					rewards *= 0.9
				}
				println()
			}

			// 时延
			let tt = r.last.getOrThrow()
			sleep(Duration.millisecond * Int64(tt)) /* FIXME: 小数不准*/
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


