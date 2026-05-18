package bot4cj
import std.io.*
import std.fs.*
import std.env.*
import std.argopt.*
import std.convert.*
import std.collection.*
import stdx.net.http.*
import stdx.encoding.json.*
import bot4cj.hd.*
import bot4cj.math.*

var I2C_PATH = "/dev/i2c-1"	/*  For Raspberrypi  */
var HTTP_ADDR = "127.0.0.1"
var HTTP_PORT = 5119u16
var NNS_OUT: Option<String> = None	/* 神经网络输出路径 */
main(args: Array<String>): Int64 {
	
	/* --------------------------- */
	/* 解析命令行参数 */

	let specs = [
		ArgumentSpec.Long("help", ArgumentMode.NoValue),	/* 拉起帮助菜单 */
		ArgumentSpec.Long("serve", ArgumentMode.OptionalValue),	/* 启动http伺服器 */
		ArgumentSpec.Long("i2c", ArgumentMode.RequiredValue),	/* 选择i2c设备 */
		ArgumentSpec.Short(r'i', ArgumentMode.RequiredValue),	/* 神经网络模型输入 */
		ArgumentSpec.Short(r'o', ArgumentMode.RequiredValue),	/* 神经网络模型输出 */
		ArgumentSpec.Long("test", ArgumentMode.RequiredValue)	/* 运行一个测试分支 */
	]


	let parsed = parseArguments(args, specs)	/* 执行解析 */
	let options = parsed.options

	//---------------------------
	/* 读取设置项 */


	if (options.contains("i2c")) {
		I2C_PATH = options['i2c']
	}

	if (options.contains("serve")) {
		if (options['serve'] != "") {
			let s = options['serve']
			HTTP_PORT = UInt16.parse(s)
		}
	}

	/*---------------------------*/
	/* 初始化神经网络 （init nn）
	 * TODO: 可以去掉内部初始化，使用矩阵式模板进行初始化 */


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


	/*---------------------------*/
	/* 分支选择 */


	/* -------- 拉起帮助手册 -------- */
	if  (options.contains("help")) {
		print_help()
		return 0
	}

	/* -------- 启动一个测试分支 -------- */
	else if (options.contains("test")) {
		run_test(options["test"])
		return 0
	}

	/* -------- 启动web伺服器 --------
	 * INFO: 伺服器主要能够接收动作脚本，我们还准备植入托管前端以及文件系统操作 */
	else if (options.contains("serve")) {
		let savepath  = if (options.contains('o')) {
			options['o']
		} else  { "./nn.txt" } /* 获取储存路径 */
        	run_serve(nns, savepath)
	}

	/* -------- 缺省行为：从标准输入获取动作脚本 -------- */
	else {
		let consoleReader = getStdIn()
		run_action_drl(consoleReader, nns)
		if (options.contains("o")) {
			save_nns(nns, options['o'])  }
	}


	return 0
}

/* -------- 打印帮助手册 -------- */
func print_help()
{
	println("这是一个机器人动作脚本解析器。")
	println("使用管道输入动作脚本，示例：")
	println("cat std.bot | ./bot")
	println()
	println("--i2c=/path/to/device\t选择i2c设备")
	println()
	println("-i  选择输入的模型的路径")
	println("-o  设置输出的模型的路径")
}

/* -------- 集成多种常规测试、临时测试 -------- */
func run_test(key: String) {

	/* ----------快速傅里叶变换测试---------- */
	if (key == "fft") {
		println('FFT test start...')
		var fftt = genWave()
		var fftr = start_fft(fftt)
		println('FFT test completed.')
	}

	/* -----------神经网络与门测试------------
	 * INFO: 我们使用随机生成的动态数据集，这样会导致一些后果 */
	else if (key == "nn") {
		let nn_t = ArrayList<Nn1>()
		nn_t.add(Nn1(2, 4, 0.1f32, Af.Leaky_ReLU(FP32(0.01))))
		nn_t.add(Nn1(4, 1, 0.1f32, Af.Sigmoid))
		let batch = 1
		let num =2000
		let prt_num = 100
		for (i in 0..(num*batch)) {
			let m = Matrix1(1, 2)
			let t = Matrix1(1, 1)
			prepare_andDoor(m, t)
			let l = train1(nn_t, m, t, 1.0f32) // 这个1.0是学习率的折扣，而非学习率本身
			if (i % prt_num == prt_num-1) {
				print("${i}\t:")
				print("${l}")
				println()
			}
		}
	}

	/* -------------对单个mpu6050进行持续测试--------------
	 * 这里主要是用于测试该硬件是否可用 */
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

	/* --------------发声测试--------------- */
	else if (key == "sound") {
		let spk = Spk(channels: 2i32, sample_rate: 48000i32)
		spk.play(500.0f32, 0.2, 400)
	}


	/* -------------测试解析矩阵------------ */
	else if (key == "matrix") {
		let file = File("ss/test.mt", OpenMode.Read)
		let robot = RobotStruct(file)
		file.close()
		try {
			robot.active()
			robot.run()
		}
		finally {
			robot.deactive()
		}
	}

	/* -------- 测试解析矩阵，并且输出 -------- */
	else if (key == "matrix_o") {
		let file = File("ss/test.mt", OpenMode.Read)
		let robot = RobotStruct(file)
		file.close()
		let outf = File("ss/output.mt", OpenMode.Write)
		robot.save(outf)
		outf.close()
	}

	/* -------- 解析测试输出的矩阵 -------- */
	else if (key == "matrix_2") {
		let file = File("ss/output.mt", OpenMode.Read)
		let robot = RobotStruct(file)
		file.close()
		robot.active()
		robot.run()
		robot.deactive()
		let outf = File("ss/output_1.mt", OpenMode.Write)
		robot.save(outf)
		outf.close()
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

	/*  curl -X POST http://127.0.0.1:5119/api/v2/bot_run -d ${cat filepath}  */
	serv.distributor.register('/api/v2/bot_run', { context =>
		if (context.request.method != 'POST') {				
			let headers = HttpHeaders()
			headers.add('Content-Type', 'text/plain; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body("Method not supportted") // FIXME: http code
			context.responseBuilder.status(400)
		}
		/* ---- 动作 ---- */
		run_action_drl(context.request.body, nns)
		save_nns(nns, save_path)
		/* ---- 响应 ---- */
		let headers = HttpHeaders()
		headers.add('Content-Type', 'text/html; charset=utf-8;')
		context.responseBuilder.setHeaders(headers)
		context.responseBuilder.body('OK')
	})

	/* curl -X GET / */
	serv.distributor.register('/', { context =>
		try {
			if (context.request.method != 'GET') {
				let headers = HttpHeaders()
				headers.add('Content-Type', 'text/plain; charset=utf-8;')
				context.responseBuilder.setHeaders(headers)
				context.responseBuilder.body("Method not supportted")
				context.responseBuilder.status(400)
			}

			let file = File("public/index.html", OpenMode.Read)
			let headers = HttpHeaders()
			headers.add('Content-Type', 'text/html; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body(file)
			context.responseBuilder.status(200)
		}

		catch (exp: Exception) {
			let headers = HttpHeaders()
			headers.add('Content-Type', 'text/plain; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body(exp.toString())
			context.responseBuilder.status(500)
		}
	})

	/*  curl -X POST http://127.0.0.1:5119/api/v1/fs -d '{"mode": "list", "path": "/path/to/dirent"}'
	 *  python test/test_api_fs.py  */
	serv.distributor.register('/api/v1/fs', { context =>
	
		try {
			if (context.request.method != 'POST') {				
				let headers = HttpHeaders()
				headers.add('Content-Type', 'text/plain; charset=utf-8;')
				context.responseBuilder.setHeaders(headers)
				context.responseBuilder.body("Method not supportted")
				context.responseBuilder.status(400)
			}
			let json = StringReader(context.request.body).readToEnd()
			let obj = JsonValue.fromStr(json).asObject()
			let mode = obj.get('mode').getOrThrow().asString().getValue()
			if (mode == "list") {
				let path = obj.get('path').getOrThrow().asString().getValue()
				let ret = getDirList(path)
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
			headers.add('Content-Type', 'text/plain; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body(exp.toString()) // FIXME: http code
			context.responseBuilder.status(500)
		}

	})


	println('Bot at ${HTTP_ADDR}:${HTTP_PORT}')
	serv.serve()
}

/*  取得json格式的目录内容的列表，显示目录、符号链接、普通文件
 *  附带文件是否可读可写，如果是普通文件，还会添加文件大小项  */
func getDirList(path: String): JsonValue {
	let ret = JsonArray()
	Directory.walk(path, {fileinfo =>
		let obj = JsonObject()
		obj.put("name", JsonString(fileinfo.name))
		if (fileinfo.isDirectory()) {
			obj.put("type", JsonString("Directory")) }
		else if (fileinfo.isSymbolicLink()) {
			obj.put("type", JsonString("SymbolicLink")) }
		else if (fileinfo.isRegular()) {
			obj.put("type", JsonString("Regular"))
			obj.put("size", JsonInt(fileinfo.size))
		}
		obj.put("writable", JsonBool(fileinfo.canWrite()))
		obj.put("readable", JsonBool(fileinfo.canRead()))
		ret.add(obj)
		return true
	})
	return ret
}
