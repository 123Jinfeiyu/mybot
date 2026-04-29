package bot4cj
//foreign func play_frequency(frequency: Int32, duration_ms: Int32, sample_rate: Int32): Bool
foreign func play_frequency(frequency: Int32, duration_ms: Int32, volume: Float64, channels: Int32, sample_rate: Int32): Bool

// 扬声器类
// TODO: 一次打开，一段时间后再关闭
class Spk {
	var channels: Int32
	var sample_rate: Int32
	init(channels!: Int32, sample_rate!: Int32) {
		this.channels = channels
		this.sample_rate = sample_rate
	}

	// 控制扬声器发声
	func play(frequency: Int32, volume: Float64, durations_ms: Int32): Bool {
		unsafe {
			play_frequency(frequency, durations_ms, volume, channels, sample_rate)
		}
	}
}
