package bot4cj
foreign func Sigmoid_do(m: CPointer<Unit>): CPointer<Unit>
foreign func Sigmoid_dot(m: CPointer<Unit>): CPointer<Unit>
foreign func ReLU_do(m: CPointer<Unit>): CPointer<Unit>
foreign func ReLU_dot(m: CPointer<Unit>): CPointer<Unit>
enum ActivationFuncs {
	None | ReLU |Leaky_ReLU |Tanh
	func fdo(m: Matrix): Matrix { // 暂时只设置ReLU
		unsafe {
			let r = Sigmoid_do(m.getData())
			return Matrix(r)
		}
	}
	func fdot(m: Matrix): Matrix {
		unsafe {
			let r = Sigmoid_dot(m.getData())
			return Matrix(r)
		}
	}
}

class Nn {
	init(inputno: UInt64, outputno: UInt64, afunc: ActivationFuncs, learning_rate: Float32)
	{
		this.inputno = inputno
		this.outputno = outputno
		weights = Matrix.initWithXavierNormal(inputno, outputno)
		bias = Matrix(1, outputno)
		this.afunc = afunc
		this.learning_rate = learning_rate
	}
	
	func forward(x: Matrix): Matrix {
		// x => (inputs, 1)
		let r = x * weights + bias // (1, outputno)
		return r
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
	let afunc: ActivationFuncs
	var learning_rate: Float32
}
