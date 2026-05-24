package bot4cj.math
import std.collection.*

/* -------- 张量 --------
 * INFO(20260524-qing): 输入一个维度列表 */
class Tensor<T> {
	private let data: Array<T>
	private let dims: Array<Int64>
	private let size: Int64
	public init(dims: Array<Int64>, val: T) {
		var size = 1
		for (item in dims) {	/* ---- 计算总量 ---- */
			size *= item
		}
		this.data = Array<T>(size, {_ => val})
		this.dims = dims
		this.size = size
	}

	/* -------- 私有构造函数，用于内部创建张量 -------- */
	private init(dims: Array<Int64>, data: Array<T>) {
		this.dims = dims
		this.data = data
		var size = 1
		for (item in dims) {
			size *= item
		}
		this.size = size
	}

	/* -------- 获取维度列表（形状） -------- */
	public func shape(): Array<Int64> {
		return dims
	}

	/* -------- 逐元素进行某种运算 -------- */
	public func oneByOne(S: Tensor<T>, f: (T, T) -> T): Tensor<T> {
		let s1 = S.shape()

		/* ---- 比较维度数量 ---- */
		if (s1 != dims) {
			throw IllegalArgumentException("Tensors' shape are not same")
		}

		/* ---- 比较维度形状 ----*/
		for (i in 0..dims.size) {
			if (s1[i] != dims[i]) {
				throw IllegalArgumentException("Tensors' shape are not same")
			}
		}

		/* ---- 逐个进行计算 ---- */
		let res_data = Array<T>(size, {i => f(data[i], S.data[i])})
		return Tensor<T>(dims, res_data)
	}
}
