package bot4cj.hd
/* 舵机类，含最大值、最小值、漂移值三个调整项
 * 以及一个设置角度回调函数 */
public class Servo {
	var max: Float32
	var min: Float32
	var offset: Float32
	var setAngle: (UInt8, Float32) -> Unit
	init(max!: Float32, min!: Float32, offset!: Float32,
			setAngle!: (UInt8,Float32) -> Unit)
	{
		this.max =max
		this.min = min
		this.offset = offset
		this.setAngle = setAngle
	}
}
