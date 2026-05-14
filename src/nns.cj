package bot4cj
import std.io.*
import std.fs.*
import std.math.*
import std.time.*
import std.random.*
import std.collection.*

// 加载多层神经网络
// TODO: 也许用类封装
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

func prepare_andDoor(m: Matrix1, t: Matrix1)	/* 准备与门的测试用例 */
{
	let random = Random(UInt64(DateTime.now().nanosecond))
	let a = random.nextInt8(2)
	let b = random.nextInt8(2)
	let t0 = if ((a == 0 &&  b == 0) || (a ==1 && b == 1)) { 1.0f32 } else { 0.0f32 }    
	//let t1 = if (a  != b) { 1.0f32 } else { 0.0f32 }    
	m.set(0, 0, FP32(Float32(a)))
	m.set(0, 1, FP32(Float32(b)))
	t.set(0, 0, FP32(t0))
	//t.set(0, 1, FP32(t1))
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
func forward1(nns: ArrayList<Nn1>, m: Matrix1) {

	// 正向传播
	var out_dof = Matrix1(1, 1)
	for (idx in 0..nns.size) {
		let x = if (idx == 0) { m } else { out_dof }
		out_dof= nns[idx].forward(x)
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
func train1(nns: ArrayList<Nn1>, m: Matrix1, t: Matrix1, discount: Float32) {

	// train

	var res = Matrix1(1, 1)
	// 正向传播
	for (idx in 0..nns.size) {
		let x = if (idx == 0) { m } else { res }
		res = nns[idx].forward(x)
	}

	// 计算误差
        var errs =  t - res

	// 反向传播
	for (idx in (nns.size-1)..=0:-1) {
		errs = nns[idx].backward(errs)
	}

	// 梯度更新
	for (idx in 0..nns.size) {
 		nns[idx].update(discount)
	}

	// 计算MSE损失
	let t0 = t.get(0, 0).getOrThrow().getValue()
	//let t1 = t.get(0, 1).getOrThrow().getValue()
	let r0 = res.get(0, 0).getOrThrow().getValue()
	//let r1 = res.get(0, 1).getOrThrow().getValue()
	let loss0 = 0.5 * (t0 - r0) * (t0 - r0)
	//let loss1 = 0.5 * (t1 - r1) * (t1 - r1)
	//return (loss0 + loss1) / 2.0
	return loss0
}
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
		let t0 = t.get(0, 0)
		let t1 = t.get(0, 1)
		let r0 = outs_dof[nns.size-1].get(0, 0)
		let r1 = outs_dof[nns.size-1].get(0, 1)
		let loss0 =  -t0 * log(r0) - (1.0f32-t0) * log(1.0f32-r0);
		let loss1 =  -t1 * log(r1) - (1.0f32-t1) * log(1.0f32-r1);
		return 0.5 * (loss0 * loss0 + loss1 * loss1)
	}
}
