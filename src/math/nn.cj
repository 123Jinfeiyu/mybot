package bot4cj.math
import std.fs.*
import std.io.*
import std.convert.*
foreign func Sigmoid_do(m: CPointer<Unit>): CPointer<Unit>
foreign func Sigmoid_dot(m: CPointer<Unit>): CPointer<Unit>
foreign func ReLU_do(m: CPointer<Unit>): CPointer<Unit>
foreign func ReLU_dot(m: CPointer<Unit>): CPointer<Unit>
foreign func Leaky_ReLU_do(m: CPointer<Unit>, alpha: Float32): CPointer<Unit>
foreign func Leaky_ReLU_dot(m: CPointer<Unit>, alpha: Float32): CPointer<Unit>
foreign func Tanh_do(m: CPointer<Unit>): CPointer<Unit>
foreign func Tanh_dot(m: CPointer<Unit>): CPointer<Unit>
/* -------- 旧版激活函数 --------*/
public enum ActivationFuncs {
	Sigmoid | ReLU | Leaky_ReLU | Tanh
	func fdo(m: Matrix): Matrix { // 暂时只设置ReLU
		unsafe {
                        match(this) {
				case Sigmoid => Matrix(Sigmoid_do(m.getData()))
				case ReLU  => Matrix(ReLU_do(m.getData()))
				case Leaky_ReLU => Matrix(Leaky_ReLU_do(m.getData(), 0.1f32))
				case Tanh  => Matrix(Tanh_do(m.getData()))
			}
		}
	}
	func fdot(m: Matrix): Matrix {
		unsafe {
			match(this) {
				case Sigmoid => Matrix(Sigmoid_dot(m.getData()))
				case ReLU => Matrix(ReLU_dot(m.getData()))
				case Leaky_ReLU => Matrix(Leaky_ReLU_dot(m.getData(), 0.1f32))
				case Tanh => Matrix(Tanh_dot(m.getData()))
			}
		}
	}
	func toString(): String {
		match(this) {
			case Sigmoid => "Sigmoid"
			case ReLU => "ReLU"
			case Leaky_ReLU => "Leaky_ReLU"
			case Tanh => "Tanh"
		}
	}
	static func parse(s: String) {
		match(s) {
			case "Sigmoid" => ActivationFuncs.Sigmoid
			case "ReLU" => ActivationFuncs.ReLU
			case "Leaky_ReLU" => ActivationFuncs.Leaky_ReLU
			case "Tanh" => ActivationFuncs.Tanh
			case _ => throw IllegalArgumentException()
		}
	}
}

/* -------- 旧版的神经网络类 --------
 * 这个实现依赖C++语言的动态分享库，特点是速度快 
 * 这个实现可能是有问题的  */
public class Nn {

	/* -------- 主初始化器 -------- */
	public init(
			inputno: UInt64, outputno: UInt64, f: ActivationFuncs, learning_rate: Float32) {
		this.inputno = inputno
		this.outputno = outputno
		weights = Matrix.initWithXavierUniform(inputno, outputno)
		bias = Matrix(1, outputno)
		this.f = f
		this.learning_rate = learning_rate
	}

	/* -------- 提供了权重矩阵的初始化器 -------- */
	public init(
			inputno: UInt64, outputno: UInt64, f: ActivationFuncs, learning_rate: Float32, weights: Matrix, bias: Matrix) {
		this.inputno = inputno
		this.outputno = outputno
		this.weights = weights
		this.bias = bias
		this.f = f
		this.learning_rate = learning_rate
	}
	
	/* -------- 神经网络推理 -------- */
	func forward(x: Matrix): Matrix {
		/*  x => (1, inputs)  */
		let r = x.dot(weights) + bias	/*  (1, outputno)  */
		return r
	}

	/* -------- 实施激活函数 -------- */
	func fdo(x: Matrix): Matrix {
		return f.fdo(x)  }

	/* -------- 激活函数导数 -------- */
	func fdot(x: Matrix): Matrix {
		return f.fdot(x)  }

 	/*  This is Wrong  */
	//func bak_bsgrad(prev_raw_outputs_dotf: Matrix, errs: Matrix): Matrix {
		/*  prev_raw_outputs_dotf => (1, outputno)
		 *  errs => (1, outputno)  */

	//	return errs * prev_raw_outputs_dotf
	//}

	/* -------- 权重梯度的传播 -------- */
	func bak_wsgrad(prev_inputs: Matrix, delta: Matrix) {
		/*  prev_inputs => (1, inputs)  */
		return prev_inputs.T().dot(delta)	/*  (inputs, outputs)  */
	}

	/* -------- 计算上一层的误差 -------- */
	func bak_uperrs(delta: Matrix, prev_dotf: Matrix) {
		let first =  weights.dot(delta.T()).T() // (1, inputs)
		
		var uperrs = first * prev_dotf
		uperrs
	}

	/* -------- 更新权重、偏爱因子 -------- */
	func update(weights_grad: Matrix, bias_grad: Matrix, discount: Float32) {
		weights -= weights_grad * learning_rate * discount
		bias    -= bias_grad    * learning_rate * discount
	}

	/* -------- 打印权重、偏爱因子 -------- */
	func print() {
		weights.print()
		bias.print()
	}

	/* -------- 打印神经网络全连接层的形状 -------- */
	func print_shape() {
		println("${inputno}, ${outputno}") }

	/* -------- 储存神经网络 -------- */
	func save(file: File) {
		file.write("FullConnectLayer ".toArray())
		file.write(inputno.toString().toArray())
		file.write(" ".toArray())
		file.write(outputno.toString().toArray())
		file.write(" ".toArray())
		file.write(learning_rate.toString().toArray())
		file.write(" ".toArray())
		file.write(f.toString().toArray())
		file.write("\n".toArray())
		weights.save(file)
		//file.write("\n".toArray())
		bias.save(file)
		//file.write("\n".toArray())
	}

	/* -------- 静态方法，加载单个项目 -------- */
	static func loadItem<T>(sr: StringReader<InputStream>) where T <: Parsable<T>
	{
		let s = sr.readUntil({r => r == r" " || r == r"\n"})
		let ss = s.getOrThrow().trimAscii()
		println(ss)
		return T.parse(ss)
	}

	/* -------- 静态方法，加载神经网络 -------- */
	static func load(sr: StringReader<InputStream>)
	{
		let inputno = loadItem<UInt64>(sr)
		let outputno = loadItem<UInt64>(sr)
		let lr = loadItem<Float32>(sr)
		let fna = sr.readUntil(r'\n')
		let f = ActivationFuncs.parse(fna.getOrThrow().trimAscii())
		let weights = Matrix.load(sr, inputno, outputno)
		let bias = Matrix.load(sr, 1, outputno)
		Nn(inputno, outputno, f, lr, weights, bias)
	}

	let inputno: UInt64
	let outputno: UInt64
	var weights: Matrix	/*  (inputs, outputs)  */
	var bias: Matrix	/* (1, outputs)  */
	var learning_rate: Float32
 	let f: ActivationFuncs
}

/* -------- 新版的神经网络，使用原生矩阵对象实现 -------- */
public class Nn1 {
	let inputno: Int64	/* 神经网络规格 */
	let outputno: Int64
	var weights: Matrix1	/* (inputs, outputs) */
	var bias: Matrix1	/* (1, output) */
	let learning_rate: FP32	/* 学习率 */
	let f: Af		/* 激活函数 */
	/*------------------*/
	var prev_inputs: Option<Matrix1> = None
	var prev_raw_inputs: Option<Matrix1> = None
	var prev_outputs: Option<Matrix1> = None
	/*------------------*/
	var weights_grad: Option<Matrix1> = None
	var bias_grad: Option<Matrix1> = None
	public init(i: Int64, o: Int64, lr: Float32, f: Af) {
		inputno = i
		outputno =o
		learning_rate=FP32(lr)
		this.f = f
		weights = Matrix1(inputno, outputno)
		bias = Matrix1(1, outputno)
		Ini.xavierUniformInit(weights, inputno, outputno)
	}
	func forward(x: Matrix1) {
		prev_inputs = x
		var r = x.dot(weights)
		r = r + bias
		prev_raw_inputs = r
		let r1 = f.forward(r)
		prev_outputs = r1
		return r1
	}

	func backward(errs: Matrix1) {
		let first = errs  * f.backward(prev_raw_inputs.getOrThrow()) /* 逐元素相乘 */
		let mx = prev_inputs.getOrThrow().T()
		weights_grad = mx.dot(first)
		bias_grad = first
		let errs_up = first.dot(weights.T())
		return errs_up
	}

	/* NOTE(20260514-qing):当前震荡是正常的SGD行为，但通过动量或学习率衰减可以显著改善。 */
	/* NOTE: discount（折扣）是为了方便深度强化学习给出奖励值 */
	func update(discount: Float32) {
		weights = weights +  weights_grad.getOrThrow() * learning_rate * FP32(discount)
		bias = bias + bias_grad.getOrThrow() * learning_rate * FP32(discount)
	}
}
