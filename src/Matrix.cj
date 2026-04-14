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

class Matrix {

	// 获取资源
	init(row: UInt64, col: UInt64) {
		unsafe { M = createMatrix(row, col) }
	}

	Matrix(m: CPointer<Unit>) {
		M = m
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

	// 矩阵的C风格指针
	var M: CPointer<Unit>
}
