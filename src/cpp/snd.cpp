#include <alsa/asoundlib.h>
#include <cmath>
#include <iostream>
#include <cstdint>
#include <vector>
extern "C" {

// frequency: 频率 (Hz)
// duration_ms: 持续时间 (毫秒)
// volume: 音量 (0.0 ~ 1.0)
// channels: 声道数 (1=单声道, 2=立体声)
// sample_rate: 采样率 (Hz)
bool play_frequency(int32_t frequency, int32_t duration_ms, 
                    double volume = 0.7, int32_t channels = 2, 
                    int32_t sample_rate = 48000) {
    if (frequency <= 0 || duration_ms <= 0 || 
        volume <= 0.0 || volume > 1.0 || 
        channels <= 0 || channels > 8 ||
        sample_rate <= 0) {
        std::cerr << "无效的参数" << std::endl;
        return false;
    }
    
    // 打开设备
    snd_pcm_t *pcm_handle = nullptr;
    const char* device = "hw:0,0";  // USB音频设备
    
    int rc = snd_pcm_open(&pcm_handle, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        std::cerr << "无法打开设备: " << snd_strerror(rc) << std::endl;
        return false;
    }
    
    // 设置硬件参数
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_handle, params);
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_handle, params, channels);
    snd_pcm_hw_params_set_rate_near(pcm_handle, params, 
                                   reinterpret_cast<unsigned int*>(&sample_rate), 0);
    snd_pcm_hw_params(pcm_handle, params);
    
    // 生成正弦波数据
    int32_t num_frames = static_cast<int64_t>(sample_rate) * duration_ms / 1000;
    int32_t total_samples = num_frames * channels;
    std::vector<int16_t> buffer(total_samples);
    
    int32_t amplitude = static_cast<int32_t>(32767.0 * volume);
    
    for (int32_t i = 0; i < num_frames; i++) {
        int16_t sample = static_cast<int16_t>(
            amplitude * sin(2.0 * M_PI * frequency * i / sample_rate)
        );
        
        // 为所有声道填充相同的值
        for (int32_t ch = 0; ch < channels; ch++) {
            buffer[i * channels + ch] = sample;
        }
    }
    
    // 播放
    rc = snd_pcm_writei(pcm_handle, buffer.data(), num_frames);
    if (rc == -EPIPE) {
        snd_pcm_prepare(pcm_handle);
        snd_pcm_writei(pcm_handle, buffer.data(), num_frames);
    }
    
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    
    return true;
}

// 便捷函数：播放特定频率
void play_tone(int32_t frequency, int32_t duration_ms = 1000, 
               double volume = 0.7) {
    play_frequency(frequency, duration_ms, volume, 2, 48000);
}

// 便捷函数：播放音符序列
void play_melody() {
    // A4, B4, C5, D5, E5
    int32_t notes[] = {440, 494, 523, 587, 659};
    int32_t durations[] = {500, 500, 500, 500, 1000};
    
    for (int i = 0; i < 5; i++) {
        std::cout << "播放 " << notes[i] << " Hz, " 
                  << durations[i] << " ms" << std::endl;
        play_tone(notes[i], durations[i], 0.5);
        usleep(50000);  // 50ms间隔
    }
}

}
