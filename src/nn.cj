package bot4cj
enum ActivationFuncs {
	None | ReLU |Leaky_ReLU |Tanh
}
class NeuralNetwork {
	init(inputno: UInt64, outputno: UInt64, afunc: ActivationFuncs, learning_rate: Float32) {
		this.inputno = inputno
		this.outputno = outputno
		weights = Matrix(inputno, outputno)
		bias = Matrix(1, outputno)
		this.afunc = afunc
		this.learning_rate = learning_rate
	}
	let inputno: UInt64
	let outputno: UInt64
	let weights: Matrix // (outputs, inputs) 
	let bias: Matrix    // (1, outputs)
	let afunc: ActivationFuncs
	var learning_rate: Float32
}
