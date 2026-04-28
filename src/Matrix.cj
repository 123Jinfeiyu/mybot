package bot4cj
import std.fs.*
import std.io.*
import std.convert.*
foreign func createMatrix(row: UInt64, col: UInt64): CPointer<Unit>
foreign func freeMatrix(M: CPointer<Unit>): Unit
//foreign func witchMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64): CPointer<Float32>
foreign func getMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64): Float32
foreign func setMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64, v: Float32): Unit
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

// TODO: 将矩阵元数据存储在仓颉部分
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

	//operator func [](r: UInt64, c: UInt64): CPointer<Float32> {
	//	unsafe { return witchMatrix(M, r, c) }
	//}

	func get(r: UInt64, c: UInt64): Float32 {
		unsafe { return getMatrix(M, r, c) }
	}

	func set(r: UInt64, c: UInt64, v: Float32) {
		unsafe { setMatrix(M, r, c, v) }
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
					let a = this.get(i, j)
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

	func save(file: File) {
		unsafe {
			for (i in 0..getRow()) {
				for (j in 0..getCol()) {
					file.write(this.get(i, j).toString().toArray())
					if (j < getCol()-1) { 
						file.write(" ".toArray()) }
					else { file.write("\n".toArray()) }
				} 
			}
		}
	}

	static func loadItem(sr: StringReader<InputStream>) {
		let s = sr.readUntil({r => r==r' ' || r ==r'\n'})
		return Float32.parse(s.getOrThrow())
	}
	static func load(sr: StringReader<InputStream>, row: UInt64, col: UInt64): Matrix {
		let m = Matrix(row, col)
		unsafe {
			for (i in 0..m.getRow()) {
				for (j in 0..m.getCol()) {
					let f  = loadItem(sr)
					m.set(i, j, f)
				}
			}
			m
		}
	}

	// 矩阵的C风格指针
	var M: CPointer<Unit>
}
