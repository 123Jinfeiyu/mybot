package bot4cj
/*
// FIXME: 使用Int64作为维度
foreign func Cu4cj_Free(d: CPointer<Unit>): Unit
foreign func Cu4cj_Orig(n: Int64): CPointer<Float32>
foreign func Cu4cj_ToDev(h: CPointer<Float32>, n: Int64): CPointer<Float32>
foreign func Cu4cj_ToHost(d: CPointer<Float32>, h: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_Zero(n: Int64): CPointer<Float32>
foreign func Cu4cj_Normal(n: Int64, fan_in: Int64, fan_out: Int64): CPointer<Float32>
foreign func Cu4cj_Add(d1: CPointer<Float32>, d2: CPointer<Float32>, d_r: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_Sub(d1: CPointer<Float32>, d2: CPointer<Float32>, d_r: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_AddBroadcast(d1: CPointer<Float32>, d2: Float32, d_r: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_SubBroadcast(d1: CPointer<Float32>, d2: Float32, d_r: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_MulBroadcast(d1: CPointer<Float32>, d2: Float32, d_r: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_DivBroadcast(d1: CPointer<Float32>, d2: Float32, d_r: CPointer<Float32>, n: Int64): Unit
foreign func Cu4cj_MatDot(d1: CPointer<Float32>, d2: CPointer<Float32>, d_r: CPointer<Float32>, m: Int64, n: Int64, k: Int64): Unit

// 矩阵初始化模式
enum MatrixInitMode {
    | Zero | Normal
}

// 单精度小数 - 矩阵
class MatrixF32 {
    init(row: Int64, col: Int64, vec: CPointer<Float32>) {
        this.row = row
        this.col = col
        this.vec = vec
        length = row * col
    }
    init(row: Int64, col: Int64, mode: MatrixInitMode) {
        this.row = row
        this.col = col
        this.length = row * col
        unsafe{
            this.vec = match (mode) {
                case Zero   => Cu4cj_Zero(length)
                case Normal => Cu4cj_Normal(length, row, col)

            }
        }
    }
    ~init() {
        unsafe{
            Cu4cj_Free(CPointer<Unit>(vec))
        }
    }
    public func getRow() {
        return row
    }
    public func getCol() {
        return col
    }
    public func getData() {
        return this.vec
    }
    public operator func +(s: Float32) {
        var a = s
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_AddBroadcast(this.vec, inout a, p, length)
            MatrixF32(this.row, this.col, p) // 返回新的矩阵
        }
    }
    public operator func +(s: MatrixF32) {
        let data = s.getData()
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_Add(this.vec, data, p, length)
            MatrixF32(this.row, this.col, p) // 返回新的矩阵
        }
    }
    public operator func -(s: Float32) {
        var a = s
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_SubBroadcast(this.vec, inout a, p, length)
            MatrixF32(this.row, this.col, p) // 返回新的矩阵
        }
    }
    public operator func -(s: MatrixF32) {
        let data = s.getData()
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_Sub(this.vec, data, p, length)
            MatrixF32(this.row, this.col, p) // 返回新的矩阵
        }
    }
    public operator func *(s: Float32) {
        var a = s
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_MulBroadcast(this.vec, inout a, p, length)
            MatrixF32(this.row, this.col, p) // 返回新的矩阵
        }
    }
    public func dot(s: MatrixF32) {
        if (col != s.getRow()) { // FIXME: 定义异常
            throw Exception("matrix dot shape not match")
        }
        let data = s.getData()
        let col_s = s.getCol()
        let length = row * col_s
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_MatDot(this.vec, data, p, row, col, col_s)
            MatrixF32(row, col_s, p) // 返回新的矩阵
        }
    }
    public operator func /(s: Float32) {
        var a = s
        unsafe{
            var p = Cu4cj_Zero(length)
            Cu4cj_DivBroadcast(this.vec, inout a, p, length)
            MatrixF32(this.row, this.col, p) // 返回新的矩阵
        }
    }
    private let row: Int64
    private let col: Int64
    private let length: Int64
    private var vec: CPointer<Float32> 
}

func test_cuda(): Int64 {
    unsafe{
        println("Prepare.")
        let m1 = MatrixF32(4000, 4000, MatrixInitMode.Zero)
        let m2 = MatrixF32(4000, 4000, MatrixInitMode.Zero)
        println("Start.")
        let m_ra = m1 + 2.0f32
        let m_rr = m1 + m2
        let m_r1 = m1 - m2
        let m_r2 = m1 - 1.0f32
        let m_r3 = m1 * 2.0f32
        let m_r4 = m1 / 2.0f32
        println("Closed.")
        let m_r5 = m1.dot(m2)
        println("Finished.")
        return 0
    }
}
*/
