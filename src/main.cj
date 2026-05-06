package bot4cj
import std.io.*
import std.fs.*
import std.env.*
import std.argopt.*
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
		ArgumentSpec.Long("help", ArgumentMode.NoValue),  // 拉起帮助菜单
		ArgumentSpec.Long("serve", ArgumentMode.OptionalValue), // 启动http伺服器 
		ArgumentSpec.Long("i2c", ArgumentMode.RequiredValue),  // 选择i2c设备
		ArgumentSpec.Short(r'i', ArgumentMode.RequiredValue),  // 神经网络模型输入
		ArgumentSpec.Short(r'o', ArgumentMode.RequiredValue),  // 神经网络模型输出
		ArgumentSpec.Long("test", ArgumentMode.RequiredValue)  // 运行一个测试分支
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
	// 初始化神经网络 （init nn）
	var nns: ArrayList<Nn> = 
	if (options.keys().contains("i")) {
		let filename = options['i']
		load_nns(filename)  }
	else {
		let nn_in = ArrayList<Nn>()
		nn_in.add(Nn(41, 80, ActivationFuncs.Leaky_ReLU, 0.2f32))
		nn_in.add(Nn(80, 80, ActivationFuncs.Leaky_ReLU, 0.2f32))
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

	else if (options.contains("test")) { // 启动一个测试分支

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
	println()
	println("--i2c=/path/to/device\t选择i2c设备")
	println()
	println("-i  选择输入的模型的路径")
	println("-o  设置输出的模型的路径")

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

	// 发声测试
	else if (key == "sound") {
		let spk = Spk(channels: 2i32, sample_rate: 48000i32)
		spk.play(500.0f32, 0.2, 400)
	}

	// 解析器测试 - 只发声
	else if (key == "p-snd") {
		let file = File("ss/test.bot", OpenMode.Read)
		let sr = StringReader<InputStream>(file)
		let parser =  ActsParser(sr)

		let spk = Spk(channels: 2i32, sample_rate: 48000i32)
		while (true) {
			if (let Some(pl) <- parser.parse()) {
				let freq = pl['spk'].getVal().getOrThrow()
				let time = pl['delay'].getDelay()
				if (freq == 0.0f32) { 
					println("------------------")
					sleep(Duration.millisecond * Int64(time * 1000.0f32) / 1000)
				} else { spk.play(freq, 0.6, Int32(time)) }
				println("freq=${freq}")
				println("time=${time}")
			}
			else {
				break}
		}
		file.close()
		print(parser.serialize())
	}

	// 测试解析矩阵
	else if (key == "matrix") {
		let file = File("ss/test.mt", OpenMode.Read)
		let robot = RobotStruct(file)
		robot.run()
		file.close()
	}
}

/* http伺服器读取动作脚本
 * TODO: 集成更多的前端托管的功能 
 *
 * qing(20260430): 
 * TODO: 在http伺服器中集成smn的功能，即能够托管前端
 * TODO: 在http伺服器中集成phs的功能，即托管进程 */
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

