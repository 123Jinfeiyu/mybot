package bot4cj
import std.env.*
import std.argopt.*
import std.fs.*
import std.io.*
import std.math.*
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
		nn_in.add(Nn(41, 80, ActivationFuncs.Tanh, 0.1f32))
		nn_in.add(Nn(80, 40, ActivationFuncs.Tanh, 0.1f32))
		nn_in.add(Nn(40, 17, ActivationFuncs.Tanh, 0.1f32)) // 16个舵机加1个时延
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

	else if (key == "mpu6050") {

		
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

/* 从标准输入中读取动作脚本，并进行随机DRL */
func run_action_drl(consoleReader: InputStream, nns: ArrayList<Nn>) {
	let parser = ActsParser(consoleReader)
	let actor = Actor(I2C_PATH)
	let i2c = I2C(I2C_PATH)
	let mpu6050s = [
		MPU6050(i2c, MPU6050.ADDR_0),
		MPU6050(i2c, MPU6050.ADDR_1)
	]
	var dataSet = [
		mpu6050s[0].readSensor(),
		mpu6050s[1].readSensor()
	]
	for (i in 0..parser.size) {
		let x = Matrix(1, 41) // 4个3轴加速度和角速度，16个关节，1个时延
		unsafe {
			let r = parser[i].clone()
			for (j in 0..r.size) {
				x[0u64, UInt64(j)].write(Float32(r[j]) / 180.0f32)
			}

			for (j in 0..dataSet.size) {
				let idx = UInt64(r.size) + UInt64(j) * 6u64
				x[0u64, idx + 0u64].write(Float32(dataSet[j].accel_x) / 65535.0f32)
				x[0u64, idx + 1u64].write(Float32(dataSet[j].accel_y) / 65535.0f32)
				x[0u64, idx + 2u64].write(Float32(dataSet[j].accel_x) / 65535.0f32)
				x[0u64, idx + 3u64].write(Float32(dataSet[j].gyro_x) / 65535.0f32)
				x[0u64, idx + 4u64].write(Float32(dataSet[j].gyro_y) / 65535.0f32)
				x[0u64, idx + 5u64].write(Float32(dataSet[j].gyro_z)  / 65535.0f32)
			}

			println("----------------------------------")
			x.print()
			let y = forward(nns, x)

			for (j in 0..y.getCol()-1) {
				let a = y[0, j].read()
				r[Int64(j)] = (Float32(r[Int64(j)]) + 180.0f32 * 0.5f32) * (a + 1.0f32) - 180.0f32 * 0.5f32
			}

		      	actor.play(r)
			dataSet = [
				mpu6050s[0].readSensor(),
				mpu6050s[1].readSensor()
			]
		}
	}

	// 选择性储存
	if (let Some(P) <- NNS_OUT) {
		save_nns(nns, P)
	}
}


// 加载多层神经网络
func load_nns(filename: String): ArrayList<Nn> {
	let nns = ArrayList<Nn>()
	let file = File(filename, OpenMode.Read)
	let sr = StringReader<InputStream>(file)
	while (true) {
		if (let Some(S) <- sr.readUntil(r' ')) {
			var S2 = S.trimAscii()
			if (S2 == "FullConnectLayer") {
				nns.add(Nn.load(sr))
			}
			else if (S2 == "") { break }
			else { throw IllegalArgumentException() }
		}
		else { break }
	}

	file.close()
	return nns
}

// 储存多层神经网络
func save_nns(nns: ArrayList<Nn>, filename: String) {

		let file: File
		if (exists(filename)) {
			file = File(filename, OpenMode.Write) }
		else { file= File.create(filename) }
		for (nn in nns) { // 存储全部的层
			nn.save(file)
		}
		file.close()
}

/* 准备与门的测试用例 */
func prepare_andDoor(m: Matrix, t: Matrix)
{
	let random = Random(UInt64(DateTime.now().nanosecond))
	let a = random.nextInt8(2)
	let b = random.nextInt8(2)
	let t0 = if (a  == b) { 1.0f32 } else { 0.0f32 }    
	let t1 = if (a  != b) { 1.0f32 } else { 0.0f32 }    

	unsafe {
		m[0, 0].write(Float32(a))
		m[0, 1].write(Float32(b))
		t[0, 0].write(t0)
		t[0, 1].write(t1)
	}
}

// 神经网络推理
func forward(nns: ArrayList<Nn>, m: Matrix) {

	// 正向传播
	var out_dof = Matrix(1, 1)
	for (idx in 0..nns.size) {
		let x = if (idx == 0) { m } else { out_dof }
		let res = nns[idx].forward(x)
		out_dof = nns[idx].fdo(res)
	}
	return out_dof
}

/* 【神经网络训练】
 * description: 输入一组原始数据和教师数据，进行神经网络训练
 * @param nns:  输入神经网络结构
 * @param m:    输入一组原始数据
 * @param t:    输入一组教师数据
 * @param discount: 输入本次训练的折扣系数
 */
func train(nns: ArrayList<Nn>, m: Matrix, t: Matrix, discount: Float32) {

	// train
	let outs_dof = ArrayList<Matrix> ()
	let outs_dotf = ArrayList<Matrix> ()

	// 正向传播
	for (idx in 0..nns.size) {
		let x = if (idx == 0) { m } else { outs_dof[idx-1] }
		let res = nns[idx].forward(x)
		let out_dof = nns[idx].fdo(res)
		let out_dotf = nns[idx].fdot(out_dof)
		outs_dof.add(out_dof)
		outs_dotf.add(out_dotf)
	}

	// 计算误差
        var errs = outs_dof[nns.size-1] - t

	// 反向传播
	for (idx in (nns.size-1)..=0:-1) {
		let up = if (idx == 0) { m} else { outs_dof[idx-1] }
		let bias_grad = errs
		//let delta = nns[idx].bak_bsgrad(errs, outs_dotf[idx])
		let weights_grad = nns[idx].bak_wsgrad(up, bias_grad)
		if (idx > 0) {
			errs = nns[idx].bak_uperrs(bias_grad, outs_dotf[idx-1])
		}
 		nns[idx].update(weights_grad, bias_grad, discount)
	}

	// 输出损失
	unsafe {
		let t0 = t[0, 0].read()
		let t1 = t[0, 1].read()
		let r0 = outs_dof[nns.size-1][0, 0].read()
		let r1 = outs_dof[nns.size-1][0, 1].read()
		let loss0 =  -t0 * log(r0) - (1.0f32-t0) * log(1.0f32-r0);
		let loss1 =  -t1 * log(r1) - (1.0f32-t1) * log(1.0f32-r1);
		return 0.5 * (loss0 * loss0 + loss1 * loss1)
	}
}
