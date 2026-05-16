package bot4cj.math
import std.math.*

/* -------- Activation Functions 激活函数族 --------
 * TODO(20260516-qing): 增加序列化、反序列化工厂函数  */
public enum Af {

	None | Sigmoid | ReLU | Leaky_ReLU(FP32) | Tanh | Softmax

	/* -------- 执行激活函数 -------- */	
	func forward(x: Matrix1) {
		match(this) {
			case None => none(x)
			case Sigmoid => sigmoid(x)
			case ReLU => relu(x)
			case Leaky_ReLU(alpha) => leaky_relu(x, alpha)
			case Tanh => tanh(x)
			case Softmax => softmax(x)
		}
	}

	/* -------- 激活函数的导数 -------- */
	func backward(y: Matrix1) {
		match(this) {
			case None => none_dot(y)
			case Sigmoid => sigmoid_dot(y)
			case ReLU => relu_dot(y)
			case Leaky_ReLU(alpha) => leaky_relu_dot(y, alpha)
			case Tanh => tanh_dot(y)
			case Softmax => softmax_jacobian(y)
		}
	}

	/* -------- 空的激活函数 -------- */
	static func none(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				N.set(r, c, FP32(x1))  }
		}
		return N
	}

	/* -------- 空激活函数的导数 -------- */
	static func none_dot(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				N.set(r, c, FP32(1.0f32))  }
		}
		return N
	}

	/* -------- S型生长曲线 -------- */
	static func sigmoid(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 = 1.0f32 / (1.0f32 + exp(-x1))
				N.set(r, c, FP32(x2))  }
		}
		return N
	}

	/* -------- S型生长曲线的导数 -------- */
	static func sigmoid_dot(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 = 1.0f32 / (1.0f32 + exp(-x1))
				let x3 = x2 * (1.0f32 - x2)
				N.set(r, c, FP32(x3))  }
		}
		return N
	}
	
	/* -------- 线性整流单元 -------- */
	static func relu(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 =if (x1 > 0.0f32) {x1} else{0.0f32}
				N.set(r, c, FP32(x2))
			}
		}
		return N
	}

	/* -------- 线性整流单元的导数 -------- */
	static func relu_dot(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 =if (x1 > 0.0f32) {1.0f32} else{0.0f32}
				N.set(r, c, FP32(x2))
			}
		}
		return N
	}

	/* -------- 带泄露的线性整流单元 -------- */
	static func leaky_relu(M: Matrix1, alpha: FP32) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 =if (x1 > 0.0f32) {x1} else{x1 * alpha.getValue()}
				N.set(r, c, FP32(x2))
			}
		}
		return N
	}

	/* -------- 带泄露的整流单元导数 -------- */
	static func leaky_relu_dot(M: Matrix1, alpha: FP32) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 =if (x1 > 0.0f32) { 1.0f32 } else{alpha.getValue()}
				N.set(r, c, FP32(x2))  }
		}
		return N
	}

	/* -------- 双曲正切函数 --------*/
	static func tanh(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 = tanh(x1)
				N.set(r, c, FP32(x2))  }
		}
		return N
	}

	/* -------- 双曲正切函数的导数 -------- */
	static func tanh_dot(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 = 1.0f32 - x1 * x1
				N.set(r, c, FP32(x2))  }
		}
		return N
	}

	/* -------- 归一化指数函数 -------- */
	static func softmax(M: Matrix1) {
		let row = M.get_row()
		let col = M.get_col()
		let N = Matrix1(row, col)
		var max_val = 0.0f32
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				if (x1 > max_val) {
					max_val  = x1 }
			}
		}
		var sum = 0.0f32
		for (r in 0..row) {
			for (c in 0..col) {
				let x = M.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 = exp(x1 - max_val)
				N.set(r, c, FP32(x2))
				sum += x2
			}
		}
		for (r in 0..row) {
			for (c in 0..col) {
				let x = N.get(r, c).getOrThrow()
				let x1= x.getValue()
				let x2 = x1 / sum
				N.set(r, c, FP32(x2))
			}
		}
		return N
	}

	/* -------- 计算Softmax的雅可比矩阵 --------
	 * 输入：已计算好的softmax输出（概率分布）
	 * 输出：雅可比矩阵 J[i][j] = ∂y_i/∂x_j  */
	func softmax_jacobian(softmax_output: Matrix1) {
		let row = softmax_output.get_row()
		let col = softmax_output.get_col()
		let n = row * col
		var jacobian = Matrix1(n, n)

		for (i in 0..n) {
			for (j in 0..n) {

				let row_i = i / col	/*  需要将一维索引转换为二维索引来访问Matrix1  */
				let col_i = i % col
				let row_j = j / col
				let col_j = j % col

				let val_i = softmax_output.get(row_i, col_i).getOrThrow().getValue()
				let val_j = softmax_output.get(row_j, col_j).getOrThrow().getValue()

				if (i == j) {	/*  对角线元素: y_i * (1 - y_i)  */
					jacobian.set(i, j, FP32(val_i * (1.0f32 - val_i)))   }
				else {		/*  非对角线元素: -y_i * y_j  */
					jacobian.set(i, j, FP32(-val_i * val_j))  }
			}
		}
    
		return jacobian
	}

}
