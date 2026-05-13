package bot4cj
import std.random.*
import std.time.*
import std.math.*
class Ini {
	static func xavierNormalInit(target: Matrix1, fan_in: Int64, fan_out: Int64) {
		let seed = DateTime.now().nanosecond
		let random = Random(UInt64(seed))
		let sigma = sqrt(2.0f32 / Float32(fan_in + fan_out))	/* 计算标准差 */
		for (r in 0..fan_in) {
			for (c in 0..fan_out) {
				let v = random.nextGaussianFloat32(mean: 0.0f32, sigma:sigma)
				println(v)
				target.set(r, c, FP32(v))
			}
		}
	}

static func xavierUniformInit(target: Matrix1, fan_in: Int64, fan_out: Int64) {
    let seed = DateTime.now().nanosecond
    let random = Random(UInt64(seed))
    let limit = sqrt(6.0f32 / Float32(fan_in + fan_out))
    
    for (r in 0..fan_in) {
        for (c in 0..fan_out) {
            // 均匀分布 [-limit, limit]
            let v = (random.nextFloat32() * 2.0f32 - 1.0f32) * limit * 0.3f32
            println(v)
            target.set(r, c, FP32(v))
        }
    }
}

}
