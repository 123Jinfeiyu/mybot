package bot4cj.math
import std.random.*
import std.time.*
import std.math.*

/* -------- 神经网络使用的权重矩阵初始化类 --------
 * TODO(20260517-qing): 将该函数改为枚举类，统一一个接口，根据模式匹配执行初始化
 * - 这样在矩阵式中可以定义初始化方式  */
public class Ini {

	/* -------- xavier正态初始化 -------- */
	public static func xavierNormalInit(target: Matrix1, fan_in: Int64, fan_out: Int64) {
		let seed = DateTime.now().nanosecond
		let random = Random(UInt64(seed))
		let sigma = sqrt(2.0f32 / Float32(fan_in + fan_out))	/* 计算标准差 */
		for (r in 0..fan_in) {
			for (c in 0..fan_out) {
				let v = random.nextGaussianFloat32(mean: 0.0f32, sigma:sigma)
				target.set(r, c, FP32(v))
			}
		}
	}

	/* -------- Glorot均匀初始化 -------- */
	public static func xavierUniformInit(target: Matrix1, fan_in: Int64, fan_out: Int64) {
		let limit = sqrt(6.0f32 / Float32(fan_in + fan_out))

		for (r in 0..fan_in) {
			for (c in 0..fan_out) {
				let seed = DateTime.now().nanosecond
				let random = Random(UInt64(seed))
				let v = (random.nextFloat32() * 2.0f32 - 1.0f32) * limit	/* 均匀分布 [-limit, limit] */
				target.set(r, c, FP32(v))  }
		}

	}
}
