package bot4cj
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
enum ActivationFuncs {
	//None | Sigmoid | ReLU |Leaky_ReLU |Tanh
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

class Nn {
	init(inputno: UInt64, outputno: UInt64, f: ActivationFuncs, learning_rate: Float32)
	{
		this.inputno = inputno
		this.outputno = outputno
		weights = Matrix.initWithXavierUniform(inputno, outputno)
		bias = Matrix(1, outputno)
		this.f = f
		this.learning_rate = learning_rate
	}

	init(inputno: UInt64, outputno: UInt64, f: ActivationFuncs, learning_rate: Float32, weights: Matrix, bias: Matrix)
	{
		this.inputno = inputno
		this.outputno = outputno
		this.weights = weights
		this.bias = bias
		this.f = f
		this.learning_rate = learning_rate
	}
	
	func forward(x: Matrix): Matrix {
		// x => (1, inputs)
		let r = x.dot(weights) + bias // (1, outputno)
		return r
	}

	func fdo(x: Matrix): Matrix {
		return f.fdo(x)
	}

	func fdot(x: Matrix): Matrix {
		return f.fdot(x)
	}

 	// This is Wrong
	//func bak_bsgrad(prev_raw_outputs_dotf: Matrix, errs: Matrix): Matrix {
		// prev_raw_outputs_dotf => (1, outputno)
		// errs => (1, outputno)

	//	return errs * prev_raw_outputs_dotf
	//}

	func bak_wsgrad(prev_inputs: Matrix, delta: Matrix) {
		// prev_inputs => (1, inputs)
		return prev_inputs.T().dot(delta) // (inputs, outputs)
	}

	func bak_uperrs(delta: Matrix, prev_dotf: Matrix) {
		let first =  weights.dot(delta.T()).T() // (1, inputs)
		
		var uperrs = first * prev_dotf
		uperrs
	}

	func update(weights_grad: Matrix, bias_grad: Matrix, discount: Float32) {
		weights -= weights_grad * learning_rate * discount
		bias    -= bias_grad    * learning_rate * discount
	}

	func print() {
		weights.print()
		bias.print()
	}

	func print_shape() {
		println("${inputno}, ${outputno}")
	}

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

	static func loadItem<T>(sr: StringReader<InputStream>) where T <: Parsable<T> {
		let s = sr.readUntil({r => r == r" " || r == r"\n"})
		let ss = s.getOrThrow().trimAscii()
		println(ss)
		return T.parse(ss)
	}
	static func load(sr: StringReader<InputStream>) {
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
	var weights: Matrix // (inputs, outputs)
	var bias: Matrix    // (1, outputs)
	var learning_rate: Float32
 	let f: ActivationFuncs
}
