package bot4cj
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
	
	func forward(x: Matrix): Matrix {
		// x => (inputs, 1)
		let r = x * weights + bias // (1, outputno)
		return r
	}

	func fdo(x: Matrix): Matrix {
		return f.fdo(x)
	}

	func fdot(x: Matrix): Matrix {
		return f.fdot(x)
	}

	func bak_bsgrad(prev_raw_outputs_dotf: Matrix, errs: Matrix): Matrix {
		// prev_raw_outputs_dotf => (1, outputno)
		// errs => (1, outputno)

		//return errs * prev_raw_outputs_dotf
		var delta = Matrix(1, outputno)
		for (idx in 0u64..outputno) {
			let a = errs[0, idx]
			let b = prev_raw_outputs_dotf[0, idx]
			unsafe {
				let c = a.read() * b.read()
				delta[0, idx].write(c)
			}
		}
		
		delta
	}

	func bak_wsgrad(prev_inputs: Matrix, delta: Matrix) {
		// prev_inputs => (1, inputs)
		return prev_inputs.T() * delta // (inputs, outputs)
	}

	func bak_uperrs(delta: Matrix, prev_dotf: Matrix) {
		let first =  (weights * delta.T()).T() // (1, inputs)
		
		var uperrs = Matrix(1, inputno)
		if (!(first == prev_dotf)) {
			println('------------')
			first.print_shape()
			prev_dotf.print_shape()
			println('------------')
			throw IllegalArgumentException("not same shape")
		}
		for (idx in 0u64..inputno) {
			let a = first[0, idx]
			let b = prev_dotf[0, idx]
			unsafe {
				let c = a.read() * b.read()
				uperrs[0, idx].write(c)
			}
		}

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

	let inputno: UInt64
	let outputno: UInt64
	var weights: Matrix // (inputs, outputs)
	var bias: Matrix    // (1, outputs)
	var learning_rate: Float32
 	let f: ActivationFuncs
}
