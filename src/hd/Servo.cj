package bot4cj.hd
/* -------- 舵机类 --------
 * 含最大值、最小值、漂移值三个调整项
 * 以及一个设置角度回调函数 */
public class Servo {
	private var max: Float32
	private var min: Float32
	private var offset: Float32
	private var callback_setAngle: (Float32) -> Unit
	public init(max!: Float32, min!: Float32, offset!: Float32,
			callback_setAngle!: (Float32) -> Unit) {
		this.max =max
		this.min = min
		this.offset = offset
		this.callback_setAngle = callback_setAngle
	}

	public func setAngle(angle: Float32) {
		var a = angle
		a += offset
		if (a > max) { a = max}
		if (a < min) { a = min}
		callback_setAngle(a)
	}
}
