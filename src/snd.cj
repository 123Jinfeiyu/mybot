package bot4cj
//foreign func play_frequency(frequency: Int32, duration_ms: Int32, sample_rate: Int32): Bool
foreign func play_frequency(frequency: Int32, duration_ms: Int32, volume: Float64, channels: Int32, sample_rate: Int32): Bool
