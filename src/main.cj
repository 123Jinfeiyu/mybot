package bot4cj
import std.io.*
import std.env.*
import std.math.*
import std.time.*
import std.random.*
import std.convert.*
import std.collection.*
import stdx.net.http.*


const I2C_PATH = "/dev/i2c-1" // For Raspberrypi
main(args: Array<String>): Int64 {

	if  (args.size == 1 && args[0] == "--help") {
		println("这是一个机器人动作脚本解析器。")
		println("使用管道输入动作脚本，示例：")
		println("cat std.bot | ./bot")
		return 0
	}

	else if (args.size == 1 && args[0] == "--serve") {
        
		let serv = ServerBuilder()
		.addr("127.0.0.1")
		.port(5119)
		.build()

		// curl -X POST /api/v2/bot_run -d ${cat filepath}
		serv.distributor.register('/api/v2/bot_run', { context =>
			if (context.request.method == 'POST') {
			let actor = Actor(I2C_PATH)
			actor.parse(context.request.body)
			//println("-------------")
			//actor.print()
			//actor.play()
			// ---- 响应 ---- 
			let headers = HttpHeaders()
			headers.add('Content-Type', 'text/html; charset=utf-8;')
			context.responseBuilder.setHeaders(headers)
			context.responseBuilder.body('OK')
		}})

		println('Bot at ${"127.0.0.1"}:${5119}')
		serv.serve()
		return 0
	}

	let consoleReader = getStdIn()
	let actor = Actor(I2C_PATH)
	actor.parse(consoleReader)
	//actor.print()
	actor.play()


	/* test m */
	/*let m = Matrix.initWithXavierUniform(10, 20)
	let p = m[1, 2]
	let s1 = Matrix(20, 12)
	let s2 = Matrix(10, 20)
	let s3 = s1 + 2.0f64
	let r1 = m + 12.0f64
	let r2 = r1 + r1;
	let r3 = r2 - 5.0f64;
	let r4 = m + 2.0f64
	let r5 = r3 - r4
	let r6 = r5 * 3.0f64
	let r7 = r6 / 2.0f64
	let dr1 = r7 * s3
	let pdr1 = dr1[1, 2]
	print(m == s1)
	print("\t")
	print(m == s2)
	println()
	print(m.hasDotShapeWith(s1))
	print("\t")
	print(m.hasDotShapeWith(s2))
	println()
	print(dr1.getRow())
	print(',\t')
	print(dr1.getCol())
	println()
	unsafe {
		if (!p.isNull()) {
			println(p.read())
			println(pdr1.read())
		}
	}*/

	/* test fft */
	//var fftt = genWave()
	//fftt.print()
	//var fftr = start_fft(fftt)
	//fftr.print()


	/* test nn */
	/*let batch = 1
	let num = 10000
	let prt_num = 100
	let nns = ArrayList<Nn>()
	nns.add(Nn(2, 4, ActivationFuncs.Tanh, 0.5f64))
	nns.add(Nn(4, 2, ActivationFuncs.Sigmoid, 0.1f64))
	for (i in 0..(num*batch)) {
		let m = Matrix(1, 2)
		let t = Matrix(1, 2)
		prepare_andDoor(m, t)
		let l = train(nns, m, t, 1.0f64)
		if (i % prt_num == prt_num-1) {
			print("${i}\t:")
			print("${l}")
			println()
		}
	}*/


	return 0
}

/* 准备与门的测试用例 */
func prepare_andDoor(m: Matrix, t: Matrix)
{
	let random = Random(UInt64(DateTime.now().nanosecond))
	let a = random.nextInt8(2)
	let b = random.nextInt8(2)
	let t0 = if (a  == b) { 1.0f64 } else { 0.0f64 }    
	let t1 = if (a  != b) { 1.0f64 } else { 0.0f64 }    

	unsafe {
		m[0, 0].write(Float64(a))
		m[0, 1].write(Float64(b))
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
func train(nns: ArrayList<Nn>, m: Matrix, t: Matrix, discount: Float64) {

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
		let loss0 =  -t0 * log(r0) - (1.0f64-t0) * log(1.0f64-r0);
		let loss1 =  -t1 * log(r1) - (1.0f64-t1) * log(1.0f64-r1);
		return 0.5 * (loss0 * loss0 + loss1 * loss1)
	}
}
