package bot4cj
//foreign func play_frequency(frequency: Int32, duration_ms: Int32, sample_rate: Int32): Bool
foreign func play_frequency(frequency: Int32, duration_ms: Int32, volume: Float64, channels: Int32, sample_rate: Int32): Bool

// 扬声器类
class Spk {
	var volume: Float64
	var channels: Int32
	var sample_rate: Int32
	init(volume!: Float64, channels!: Int32, sample_rate!: Int32) {
		this.volume = volume
		this.channels = channels
		this.sample_rate = sample_rate
	}
	func play(frequency: Int32, durations_ms: Int32): Bool {
		unsafe {
			play_frequency(frequency, durations_ms, volume, channels, sample_rate)
		}
	}
}
