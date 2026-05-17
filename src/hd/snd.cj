package bot4cj.hd
//foreign func play_frequency(frequency: Int32, duration_ms: Int32, sample_rate: Int32): Bool
foreign func play_frequency(frequency: Float32, duration_ms: Int32, volume: Float64, channels: Int32, sample_rate: Int32): Bool

/* -------- 扬声器类 --------
 * TODO: 一次打开设备，一段时间后再关闭 */
public class Spk {
	private var channels: Int32
	private var sample_rate: Int32
	public init(channels!: Int32, sample_rate!: Int32) {
		this.channels = channels
		this.sample_rate = sample_rate
	}

	/* -------- 控制扬声器发声 -------- */
	public func play(frequency: Float32, volume: Float64, durations_ms: Int32): Bool {
		unsafe {  play_frequency(frequency, durations_ms, volume, channels, sample_rate)  }
	}
}
