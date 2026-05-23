package bot4cj.math
import std.fs.*
import std.io.*
import std.convert.*
//foreign func createMatrix(row: UInt64, col: UInt64): CPointer<Unit>
//foreign func freeMatrix(M: CPointer<Unit>): Unit
//foreign func witchMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64): CPointer<Float32>
//foreign func getMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64): Float32
//foreign func setMatrix(M: CPointer<Unit>, row: UInt64, col: UInt64, v: Float32): Unit
//foreign func getrowMatrix(M: CPointer<Unit>): UInt64
//foreign func getcolMatrix(M: CPointer<Unit>): UInt64
//foreign func sameShapeMatrix(M: CPointer<Unit>, S: CPointer<Unit>): Bool
//foreign func dotShapeMatrix(M: CPointer<Unit>, S: CPointer<Unit>): Bool
//foreign func plusMatrix(M: CPointer<Unit>, S:CPointer<Unit>): CPointer<Unit>
//foreign func plusBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
//foreign func subMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
//foreign func subBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
//foreign func multiMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
//foreign func multiBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
//foreign func divMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
//foreign func divBroadcastMatrix(M: CPointer<Unit>, T: Float32): CPointer<Unit>
//foreign func dotMatrix(M: CPointer<Unit>, S: CPointer<Unit>): CPointer<Unit>
//foreign func transposeMatrix(M: CPointer<Unit>): CPointer<Unit>
//foreign func xavierNormalInit(fan_in: UInt64, fan_out: UInt64): CPointer<Unit>
//foreign func xavierUniformInit(fan_in: UInt64, fan_out: UInt64): CPointer<Unit>


/* -------- 这一坨是32位浮点数 -------- */
public struct FP32 {
	private var val: Float32
	public init(t: Float32) {	/* 以Float32初始化对象 */
		val =  t }
	public func getValue() {	/* 获取值 */
		return val }
	/* -------- 运算符重载 -------- */
	//public operator func =(t: Float32) { /* this can not */
	//	val = t }
	public operator func +(t: Float32) {
		return FP32(val + t)}
	public operator func +(S: FP32) {
		return this + S.getValue() }
	public operator func -(t: Float32) {
		return FP32(val - t) }
	public operator func -(S: FP32) {
		return this - S.getValue() }
	public operator func *(t: Float32) {
		return FP32(val * t) }
	public operator func *(S: FP32) {
		return this * S.getValue() }
	public operator func /(t: Float32) {
		return FP32(val / t) }
	public operator func /(S: FP32) {
		return this / S.getValue() }

}


/*
 *  新版的矩阵对象，仓颉原生实现
 *  TODO(20260517-qing): 设计原生的多维数组类
 *

 ___col___
/         \
+---------+ \
| + + + + | | row - 第一维度
| + + + + | |
+---------+ /

*/
public class Matrix1 {

	private var row: Int64
	private var col: Int64
	private var arr: Array<FP32>

	/* -------- 构造器，默认值为0 -------- */
	public Matrix1(row: Int64, col: Int64) {
		this.row = row
		this.col = col
		this.arr = Array<FP32>(row * col, repeat: FP32(0.0f32))
	}

	/* --------- 这几个方法是访问器 -------- */
	/* ------------------------------------- */
	public func get(r: Int64, c: Int64) {
		return arr.get(r * this.col + c)
	}

	public func set(r: Int64, c: Int64, v: FP32) {
		arr[r * this.col + c] = v
	}

	public func get_row() { return row }

	public func get_col() { return col }

	/* -------- 这两个函数是比较器 -------- */
	/* ------------------------------------ */
	public func has_same_shape(other: Matrix1) {
		let a = this.get_row()  == other.get_row()
		let b = this.get_col()  == other.get_col()
		return a && b
	}

	public func has_dot_shape(other: Matrix1) {
		return this.get_col() == other.get_row()
	}

	/* -------- 这些是运算符重载 -------- */
	/* ---------------------------------- */
	public operator func +(a: FP32) {
		let r  =get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = this.get(ir, ic).getOrThrow()
				M.set(ir, ic, v + a)
			}
		}

		return M
	}

	public operator func +(S: Matrix1) {
		if (!has_same_shape(S)) {
			throw IllegalArgumentException("Matrixes doesn't match")
		}
		let r  =get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = this.get(ir, ic).getOrThrow()
				let vs = S.get(ir, ic).getOrThrow()
				M.set(ir, ic, v + vs)
			}
		}

		return M
	}

	public  operator func -(a: FP32) {
		let r = get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c){
				let v = this.get(ir, ic).getOrThrow()
				M.set(ir, ic, v - a)
			}
		}
		return M
	}

	public operator func -(S: Matrix1) {
		if (!has_same_shape(S)) {
			throw IllegalArgumentException("Matrixes doesn't match")
		}
		let r  =get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = this.get(ir, ic).getOrThrow()
				let vs = S.get(ir, ic).getOrThrow()
				M.set(ir, ic, v - vs)
			}
		}

		return M
	}

	public operator func *(a: FP32) {
		let r = get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c){
				let v = this.get(ir, ic).getOrThrow()
				M.set(ir, ic, v * a)
			}
		}
		return M
	}

	public operator func *(S: Matrix1) {
		if (!has_same_shape(S)) {
			throw IllegalArgumentException("Matrixes doesn't match")
		}
		let r  =get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = this.get(ir, ic).getOrThrow()
				let vs = S.get(ir, ic).getOrThrow()
				M.set(ir, ic, v * vs)
			}
		}

		return M
	}

	public operator func /(a: FP32) {
		let r = get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c){
				let v = this.get(ir, ic).getOrThrow()
				M.set(ir, ic, v / a)
			}
		}
		return M
	}

	public operator func /(S: Matrix1) {
		if (!has_same_shape(S)) {
			throw IllegalArgumentException("Matrixes doesn't match")
		}
		let r  =get_row()
		let c = get_col()
		let M = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = this.get(ir, ic).getOrThrow()
				let vs = S.get(ir, ic).getOrThrow()
				M.set(ir, ic, v / vs)
			}
		}

		return M
	}

	/* -------- 矩阵点积乘法 -------- */
	public func dot(S: Matrix1) {
		if (!has_dot_shape(S)) {
			throw IllegalArgumentException("Matrixes doesn't match")
		}
		let r  =get_row()
		let c = S.get_col()
		let M = Matrix1(r, c)
		for (ir in 0..get_row()) {
			for (ic in 0..S.get_col()) {
				var sum = FP32(0.0f32)
				for (ik in 0..get_col()) {
					let a = get(ir, ik).getOrThrow()
					let b = S.get(ik, ic).getOrThrow()
					sum += a * b
				}
				M.set(ir, ic, sum)
			}
		}
		return M
	}

	/* -------- 克隆一个一模一样的矩阵 -------- */
	public func clone() {
		let r = get_row()
		let c = get_col()
		let N = Matrix1(r, c)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = get(ir, ic).getOrThrow()
				N.set(ir, ic, v)
			}
		}
		return N
	}

	/* -------- 矩阵转置，返回转置后的矩阵 -------- */
	public func T() {
		let r = get_row()
		let c = get_col()
		let N = Matrix1(c, r)
		for (ir in 0..r) {
			for (ic in 0..c) {
				let v = get(ir, ic).getOrThrow()
				N.set(ic, ir, v)
			}
		}
		return N
	}
}



/* -------- 旧版的矩阵类，依赖c++编译的共享动态库 --------
 *  TODO: 将矩阵元数据存储在仓颉部分(done at 202605xx )  */
//public class Matrix {
//
//	/*  -------- 主初始化器，初始化并获取资源 --------  */
//	public init(row: UInt64, col: UInt64) {
//		unsafe { M = createMatrix(row, col) }
//	}
//
//	/* -------- 初始化器，利用已经存在的元数据进行构建 -------- */
//	public Matrix(m: CPointer<Unit>) {  M = m  }
//
//	/* -------- Xavier正态分布初始化 -------- */
//	public static func initWithXavierNormal(fan_in: UInt64, fan_out: UInt64) {
//		unsafe {
//			let m = xavierNormalInit(fan_in, fan_out);
//			return Matrix(m)
//		}
//	}
//
//	/* -------- Glorot均匀初始化 -------- */
//	public static func initWithXavierUniform(fan_in: UInt64, fan_out: UInt64) {
//		unsafe {
//			let m = xavierUniformInit(fan_in, fan_out);
//			return Matrix(m)
//		}
//	}
//
//	/* ========  在析构函数中释放资源 ======== */
//	~init() {
//		unsafe { freeMatrix(M) }
//	}
//
//	/* -------- 取下标，由于不能返回值类型的引用被弃用 -------- */
//	//public operator func [](r: UInt64, c: UInt64): CPointer<Float32> {
//	//	unsafe { return witchMatrix(M, r, c) }
//	//}
//
//	/* -------- 获得某个矩阵单元的值 -------- */
//	public func get(r: UInt64, c: UInt64): Float32 {
//		unsafe { return getMatrix(M, r, c) }
//	}
//
//	/* -------- 设置某个矩阵单元的值 --------*/
//	public func set(r: UInt64, c: UInt64, v: Float32) {
//		unsafe { setMatrix(M, r, c, v) }
//	}
//
//	/* ------- 两个矩阵形状是否相同 -------- 
//	 * FIXME: 这个函数所重载的运算符不贴切  */
//	public operator func ==(s: Matrix): Bool {
//		unsafe { return sameShapeMatrix(M, s.getData()) }
//	}
//
//	/* -------- 获得行数 -------- */
//	public func getRow(): UInt64 {
//		unsafe { return getrowMatrix(M) }
//	}
//
//	/* -------- 获取列数 --------*/
//	public func getCol(): UInt64 {
//		unsafe { return getcolMatrix(M) }
//	}
//
//	/* -------- 获取指向元数据的C风格指针 -------- */
//	public func getData(): CPointer<Unit> { return M; }
//
//	/* -------- 是否两个矩阵具有点积和的形状 -------- */
//	public func hasDotShapeWith(s: Matrix): Bool {
//		unsafe { return dotShapeMatrix(M, s.getData()) }
//	}
//
//	/* -------- 逐元素算数运算 -------- */
//	/* -------------------------------- */
//	public operator func +(s: Matrix): Matrix {
//		unsafe {
//			if (!sameShapeMatrix(M, s.getData())) {
//				println('------------')
//				print_shape()
//				s.print_shape()
//				println('------------')
//				throw IllegalArgumentException("not same shape")
//			}
//			let r = plusMatrix(M, s.getData())
//			return Matrix(r)
//		}
//	}
//
//	public operator func +(t: Float32): Matrix {
//		unsafe {
//			let r = plusBroadcastMatrix(M, t)
//			return Matrix(r);
//		}
//	}
//
//	public operator func -(s: Matrix): Matrix {
//		unsafe {
//			if (!(this == s)) {
//				println('------------')
//				print_shape()
//				s.print_shape()
//				println('------------')
//				throw IllegalArgumentException("not same shape")
//			}
//			let r = subMatrix(M, s.getData())
//			return Matrix(r)
//		}
//	}
//
//	public operator func -(t: Float32): Matrix {
//		unsafe {
//			let r = subBroadcastMatrix(M, t)
//			return Matrix(r);
//		}
//	}
//
//	public operator func *(s: Matrix): Matrix {
//		unsafe {
//			if (!(this == s)) {
//				println('------------')
//				print_shape()
//				s.print_shape()
//				println('------------')
//				throw IllegalArgumentException("not same shape")
//			}
//			let r = multiMatrix(M, s.getData())
//			return Matrix(r)
//		}
//	}
//
//	public operator func *(t: Float32): Matrix {
//		unsafe {
//			let r = multiBroadcastMatrix(M, t)
//			return Matrix(r)
//		}
//	}
//
//	public operator func /(s: Matrix): Matrix {
//		unsafe {
//			if (!(this == s)) {
//				println('------------')
//				print_shape()
//				s.print_shape()
//				println('------------')
//				throw IllegalArgumentException("not same shape")
//			}
//			let r = divMatrix(M, s.getData())
//			return Matrix(r)
//		}
//	}
//
//	public operator func /(t: Float32): Matrix {
//		unsafe {
//			let r = divBroadcastMatrix(M, t)
//			return Matrix(r)
//		}
//	}
//
//	/* -------------------------------- */
//	/* -------------------------------- */
//
//
//	/* -------- 矩阵点积和算法 -------- */
//	public func dot (s: Matrix): Matrix {
//		unsafe {
//			if (!hasDotShapeWith(s)) {
//				println('------------')
//				print_shape()
//				s.print_shape()
//				println('------------')
//				throw IllegalArgumentException("not dot shape")
//			}
//			let r = dotMatrix(M, s.getData())
//			return Matrix(r)
//		}
//	}
//
//	/* -------- 矩阵转置 -------- */
//	public func T(): Matrix {
//		unsafe {
//			let r = transposeMatrix(M)
//			return Matrix(r)
//		}
//	}
//
//	/* -------- 打印矩阵 -------- */
//	public func print(): Unit {
//		unsafe {
//			for (i in 0..getRow()) {
//				for (j in 0..getCol()) {
//					let a = this.get(i, j)
//					print(a)
//					print(" ")
//				}
//			}
//		}
//		println()
//	}
//
//	/* -------- 打印形状 -------- */
//	public func print_shape() {
//		println("${getRow()}, ${getCol()}")
//	}
//
//	/* -------- 保存矩阵 -------- */
//	public func save(file: File) {
//		unsafe {
//			for (i in 0..getRow()) {
//				for (j in 0..getCol()) {
//					file.write(this.get(i, j).toString().toArray())
//					if (j < getCol()-1) { 
//						file.write(" ".toArray()) }
//					else { file.write("\n".toArray()) }
//				} 
//			}
//		}
//	}
//
//	/* -------- 静态方法，加载单独项 -------- */
//	private static func loadItem(sr: StringReader<InputStream>) {
//		let s = sr.readUntil({r => r==r' ' || r ==r'\n'})
//		return Float32.parse(s.getOrThrow())
//	}
//
//	/* -------- 静态方法，加载矩阵 -------- */
//	public static func load(sr: StringReader<InputStream>, row: UInt64, col: UInt64): Matrix {
//		let m = Matrix(row, col)
//		unsafe {
//			for (i in 0..m.getRow()) {
//				for (j in 0..m.getCol()) {
//					let f  = loadItem(sr)
//					m.set(i, j, f)
//				}
//			}
//			m
//		}
//	}
//
//	/* -------- 矩阵的C风格指针 -------- */
//	private var M: CPointer<Unit>
//}
