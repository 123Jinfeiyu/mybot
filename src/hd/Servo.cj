package bot4cj.hd
import std.math.*
/* -------- 舵机类 --------
 * 含最大值、最小值、漂移值三个调整项
 * 以及一个设置角度回调函数
 *
 * FIXME(20260618-qing): 在构造函数中设置角度最大值 */
public class Servo {
	private var max: Float32
	private var min: Float32
	private var offset: Float32
	private var callback_setAngle: (Float32) -> Unit
	private static let MAX_ANGLE: Float32 = 180.0f32
	public init(max!: Float32, min!: Float32, offset!: Float32,
			callback_setAngle!: (Float32) -> Unit) {
		this.max =max
		this.min = min
		this.offset = offset
		this.callback_setAngle = callback_setAngle
	}

	public func setAngle(angle: Float32) {
		var a = angle
		a += offset	/* ---- 加上漂移值 ---- */
		if (a > abs(max)) { a = abs(max)}
		if (a < abs(min)) { a = abs(min)}
		if (max < 0.0f32) { a = MAX_ANGLE - a }
		callback_setAngle(a)
	}
}
