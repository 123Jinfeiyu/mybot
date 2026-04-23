package bot4cj
foreign func createMatrix(row: UInt64, col: UInt64): CPointer<Unit>
foreign func freeMatrix(M: CPointer<Unit>): Unit
foreign func witchMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64): CPointer<Float32>
foreign func getrowMatrix(M: CPointer<Unit>): UInt64
foreign func getcolMatrix(M: CPointer<Unit>): UInt64
foreign func sameShapeMatrix(M: CPointer<Unit>, S: CPointer<Unit>): Bool
foreign func dotShapeMatrix(M: CPointer<Unit>, S: CPointer<Unit>): Bool
foreign func plusMatrix(M: CPointer<Unit>, S:CPointer<Unit>): CPointer<Unit>
foreign func plusBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
foreign func subMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
foreign func subBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
foreign func multiMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
foreign func multiBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
foreign func divMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
foreign func divBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
foreign func dotMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
foreign func transposeMatrix(M: CPointer<Unit>): CPointer<Unit>
foreign func xavierNormalInit(fan_in: UInt64, fan_out: UInt64): CPointer<Unit>
foreign func xavierUniformInit(fan_in: UInt64, fan_out: UInt64): CPointer<Unit>

class Matrix {

	// 获取资源
	init(row: UInt64, col: UInt64) {
		unsafe { M = createMatrix(row, col) }
	}

	Matrix(m: CPointer<Unit>) {
		M = m
	}

	static func initWithXavierNormal(fan_in: UInt64, fan_out: UInt64) {
		unsafe {
			let m = xavierNormalInit(fan_in, fan_out);
			return Matrix(m)
		}
	}

	static func initWithXavierUniform(fan_in: UInt64, fan_out: UInt64) {
		unsafe {
			let m = xavierUniformInit(fan_in, fan_out);
			return Matrix(m)
		}
	}

	// 释放资源
	~init() {
		unsafe { freeMatrix(M) }
	}

	operator func [](r: UInt64, c: UInt64): CPointer<Float32> {
		unsafe { return witchMatrix(M, r, c) }
	}

	operator func ==(s: Matrix): Bool {
		unsafe { return sameShapeMatrix(M, s.getData()) }
	}

	func getRow(): UInt64 {
		unsafe { return getrowMatrix(M) }
	}

	func getCol(): UInt64 {
		unsafe { return getcolMatrix(M) }
	}

	func getData(): CPointer<Unit> {
		return M;
	}

	func hasDotShapeWith(s: Matrix): Bool {
		unsafe { return dotShapeMatrix(M, s.getData()) }
	}

	operator func +(s: Matrix): Matrix {
		unsafe {
			if (!sameShapeMatrix(M, s.getData())) {
				println('------------')
				print_shape()
				s.print_shape()
				println('------------')
				throw IllegalArgumentException("not same shape")
			}
			let r = plusMatrix(M, s.getData())
			return Matrix(r)
		}
	}

	operator func +(t: Float32): Matrix {
		unsafe {
			let r = plusBroadcastMatrix(M, t)
			return Matrix(r);
		}
	}

	operator func -(s: Matrix): Matrix {
		unsafe {
			if (!(this == s)) {
				println('------------')
				print_shape()
				s.print_shape()
				println('------------')
				throw IllegalArgumentException("not same shape")
			}
			let r = subMatrix(M, s.getData())
			return Matrix(r)
		}
	}

	operator func -(t: Float32): Matrix {
		unsafe {
			let r = subBroadcastMatrix(M, t)
			return Matrix(r);
		}
	}

	operator func *(s: Matrix): Matrix {
		unsafe {
			if (!(this == s)) {
				println('------------')
				print_shape()
				s.print_shape()
				println('------------')
				throw IllegalArgumentException("not same shape")
			}
			let r = multiMatrix(M, s.getData())
			return Matrix(r)
		}
	}

	operator func *(t: Float32): Matrix {
		unsafe {
			let r = multiBroadcastMatrix(M, t)
			return Matrix(r)
		}
	}

	operator func /(s: Matrix): Matrix {
		unsafe {
			if (!(this == s)) {
				println('------------')
				print_shape()
				s.print_shape()
				println('------------')
				throw IllegalArgumentException("not same shape")
			}
			let r = divMatrix(M, s.getData())
			return Matrix(r)
		}
	}

	operator func /(t: Float32): Matrix {
		unsafe {
			let r = divBroadcastMatrix(M, t)
			return Matrix(r)
		}
	}

	func dot (s: Matrix): Matrix {
		unsafe {
			if (!hasDotShapeWith(s)) {
				println('------------')
				print_shape()
				s.print_shape()
				println('------------')
				throw IllegalArgumentException("not dot shape")
			}
			let r = dotMatrix(M, s.getData())
			return Matrix(r)
		}
	}

	func T(): Matrix {
		unsafe {
			let r = transposeMatrix(M)
			return Matrix(r)
		}
	}

	func print(): Unit {
		unsafe {
			for (i in 0..getRow()) {
				for (j in 0..getCol()) {
					let a = this[i, j].read()
					print(a)
					print(" ")
				}
			}
		}
		println()
	}

	func print_shape() {
		println("${getRow()}, ${getCol()}")
	}

	// 矩阵的C风格指针
	var M: CPointer<Unit>
}
