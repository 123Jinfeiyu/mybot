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


var I2C_PATH = "/dev/i2c-1" // For Raspberrypi
var HTTP_ADDR = "127.0.0.1"
var HTTP_PORT = 5119u16


func print_help() {
	println("这是一个机器人动作脚本解析器。")
	println("使用管道输入动作脚本，示例：")
	println("cat std.bot | ./bot")
}

/* http伺服器读取动作脚本 */
func run_serve() {
	let serv = ServerBuilder()
	.addr(HTTP_ADDR)
	.port(HTTP_PORT)
	.build()

	// curl -X POST /api/v2/bot_run -d ${cat filepath}
	serv.distributor.register('/api/v2/bot_run', { context =>
		if (context.request.method == 'POST') {
			let actor = Actor(I2C_PATH)
			let parser = ActsParser(context.request.body)
			//println("-------------")
			//actor.print()
			for (i in 0..parser.size) {
				actor.play(parser[i])
			}
			// ---- 响应 ---- 
			let headers = HttpHeaders()
			headers.add('Content-Type', 'text/html; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body('OK')
		}
	})

	println('Bot at ${HTTP_ADDR}:${HTTP_PORT}')
	serv.serve()
}

/* 从标准输入中读取动作脚本 */
func run_action() {
	let consoleReader = getStdIn()
	let parser = ActsParser(consoleReader)
	let actor = Actor(I2C_PATH)
	for (i in 0..parser.size) {
		actor.play(parser[i])
	}
}

main(args: Array<String>): Int64 {

	let help = ArgumentSpec.Long("help", ArgumentMode.NoValue)
	let serve = ArgumentSpec.Long("serve", ArgumentMode.OptionalValue)
	let i2c = ArgumentSpec.Long("i2c", ArgumentMode.RequiredValue)
	let innn = ArgumentSpec.Short(r'i', ArgumentMode.RequiredValue)
	let out = ArgumentSpec.Short(r'o', ArgumentMode.RequiredValue)
	let specs = [help, serve, i2c, innn, out]
	let parsed = parseArguments(args, specs)
	let options = parsed.options

	//for (key in options.keys()) {
	//	println("  ${key}: ${options[key]}")
	//}


	if (options.contains("i2c")) {
		I2C_PATH = options['i2c']
	}

	if (options.contains("serve")) {
		if (options['serve'] != "") {
			let s = options['serve']
			HTTP_PORT = UInt16.parse(s)
		}
	}

	// init nn
	let nns = ArrayList<Nn>()
	if (options.keys().contains("i")) {
		let filename = options['i']
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
	}
	else {
		nns.add(Nn(2, 4, ActivationFuncs.Tanh, 0.5f32))
		nns.add(Nn(4, 2, ActivationFuncs.Sigmoid, 0.1f32))
	}





	if  (options.contains("help")) {
		print_help()
		return 0
	}

	else if (options.contains("serve")) {
        	run_serve()
		return 0
	}

	else {
		run_action()
		return 0
	}


	/* test fft */
	//var fftt = genWave()
	//fftt.print()
	//var fftr = start_fft(fftt)
	//fftr.print()


	/* test nn */
	let batch = 1
	let num = 10000
	let prt_num = 100
	for (i in 0..(num*batch)) {
		let m = Matrix(1, 2)
		let t = Matrix(1, 2)
		prepare_andDoor(m, t)
		let l = train(nns, m, t, 1.0f32)
		if (i % prt_num == prt_num-1) {
			print("${i}\t:")
			print("${l}")
			println()
		}
	}

	if (options.contains("o")) {

		let filename = options['o']
		let file: File
		if (exists(filename)) {
			file = File(filename, OpenMode.Write) }
		else { file= File.create(filename) }
		for (nn in nns) { // 存储全部的层
			nn.save(file)
		}
		file.close()
	}
	return 0
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

/*
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
