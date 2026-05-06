#ifdef _WIN32
    #include <windows.h>
    #include <mmsystem.h>
    #pragma comment(lib, "winmm.lib")
    #pragma comment(lib, "user32.lib")
    #define _USE_MATH_DEFINES
    #include <cmath>
#else
    #include <alsa/asoundlib.h>
    #include <cmath>
#endif

#include <iostream>
#include <cstdint>
#include <vector>
#include <cmath>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

#ifndef M_LN2
    #define M_LN2 0.693147180559945309417
#endif

#ifdef _WIN32
// Windows 回调结构
struct PlaybackContext {
    volatile bool done;
    WAVEHDR* header;
};

// Windows 播放完成回调函数
void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, 
                          DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        PlaybackContext* ctx = reinterpret_cast<PlaybackContext*>(dwInstance);
        ctx->done = true;
    }
}
#endif

extern "C" {

#ifdef _WIN32
#define SND_EXPORT __declspec(dllexport)
#else
#define SND_EXPORT
#endif

SND_EXPORT bool play_frequency(float frequency, int32_t duration_ms, 
                    double volume, int32_t channels, 
                    int32_t sample_rate) {
    // 参数验证
    if (frequency <= 0.0f || duration_ms <= 0 || 
        volume <= 0.0 || volume > 1.0 || 
        channels <= 0 || channels > 8 ||
        sample_rate <= 0) {
        std::cerr << "Invalid parameters" << std::endl;
        return false;
    }
    
    // 精确计算帧数：使用 round 而非截断
    // duration_ms / 1000.0 得到秒数，乘以采样率得到帧数
    int64_t num_frames = static_cast<int64_t>(
        std::round(static_cast<double>(sample_rate) * duration_ms / 1000.0)
    );
    
    if (num_frames <= 0) {
        std::cerr << "Duration too short to generate any frames" << std::endl;
        return false;
    }
    
    // 计算实际播放时长（用于调试）
    double actual_duration_ms = static_cast<double>(num_frames) / sample_rate * 1000.0;
    std::cout << "Requested duration: " << duration_ms << " ms, "
              << "Actual duration: " << actual_duration_ms << " ms, "
              << "Frames: " << num_frames << std::endl;
    
    // 计算总采样点数
    int32_t total_samples = static_cast<int32_t>(num_frames * channels);
    std::vector<int16_t> buffer(total_samples);
    
    // 计算振幅（应用对数音量缩放以获得更自然的音量控制）
    int32_t amplitude = static_cast<int32_t>(32767.0 * volume);
    
    // 生成正弦波数据
    for (int64_t i = 0; i < num_frames; i++) {
        // 使用 double 计算以提高精度
        double t = static_cast<double>(i) / sample_rate;
        double value = sin(2.0 * M_PI * frequency * t);
        
        // 应用淡入淡出以避免咔嗒声
        int64_t fade_samples = static_cast<int64_t>(sample_rate * 0.005); // 5ms 淡入淡出
        double fade_factor = 1.0;
        
        if (i < fade_samples) {
            // 淡入：从 0 到 1
            fade_factor = static_cast<double>(i) / fade_samples;
        } else if (i >= num_frames - fade_samples) {
            // 淡出：从 1 到 0
            fade_factor = static_cast<double>(num_frames - 1 - i) / fade_samples;
        }
        
        int16_t sample = static_cast<int16_t>(
            amplitude * value * fade_factor
        );
        
        // 填充所有声道
        for (int32_t ch = 0; ch < channels; ch++) {
            buffer[i * channels + ch] = sample;
        }
    }
    
#ifdef _WIN32
    // ===== Windows 实现（带回调，精确计时） =====
    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = channels * 2;  // 16-bit = 2 bytes per sample
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
    wfx.cbSize = 0;
    
    PlaybackContext context;
    context.done = false;
    context.header = new WAVEHDR();
    
    // 打开波形设备，使用回调函数
    HWAVEOUT hWaveOut;
    MMRESULT result = waveOutOpen(
        &hWaveOut, 
        WAVE_MAPPER, 
        &wfx, 
        reinterpret_cast<DWORD_PTR>(waveOutProc),  // 回调函数
        reinterpret_cast<DWORD_PTR>(&context),      // 传递给回调的数据
        CALLBACK_FUNCTION
    );
    
    if (result != MMSYSERR_NOERROR) {
        std::cerr << "Cannot open audio device. Error code: " << result << std::endl;
        delete context.header;
        return false;
    }
    
    // 准备 WAVEHDR
    WAVEHDR& header = *context.header;
    ZeroMemory(&header, sizeof(WAVEHDR));
    header.lpData = reinterpret_cast<char*>(buffer.data());
    header.dwBufferLength = static_cast<DWORD>(total_samples * sizeof(int16_t));
    header.dwFlags = 0;
    header.dwLoops = 0;
    
    result = waveOutPrepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        std::cerr << "Failed to prepare audio header. Error code: " << result << std::endl;
        waveOutClose(hWaveOut);
        delete context.header;
        return false;
    }
    
    // 写入音频数据
    result = waveOutWrite(hWaveOut, &header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        std::cerr << "Failed to write audio data. Error code: " << result << std::endl;
        waveOutUnprepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
        waveOutClose(hWaveOut);
        delete context.header;
        return false;
    }
    
    // 等待播放完成（使用事件等待而非轮询）
    auto start_time = timeGetTime();
    while (!context.done) {
        // 使用 MsgWaitForMultipleObjects 等待消息或超时
        // 这样可以在等待的同时处理 Windows 消息
        DWORD wait_result = MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
        
        if (wait_result == WAIT_OBJECT_0) {
            // 处理 Windows 消息
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        
        // 超时检查（防止无限等待）
        DWORD elapsed = timeGetTime() - start_time;
        if (elapsed > static_cast<DWORD>(duration_ms + 1000)) {
            std::cerr << "Playback timeout" << std::endl;
            waveOutReset(hWaveOut);
            break;
        }
    }
    
    auto end_time = timeGetTime();
    DWORD actual_play_time = end_time - start_time;
    std::cout << "Actual playback time: " << actual_play_time << " ms" << std::endl;
    
    // 清理
    waveOutUnprepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
    waveOutClose(hWaveOut);
    delete context.header;
    
#else
    // ===== Linux ALSA 实现 =====
    snd_pcm_t *pcm_handle = nullptr;
    const char* device = "default";
    
    int rc = snd_pcm_open(&pcm_handle, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        std::cerr << "Cannot open device '" << device << "': " 
                  << snd_strerror(rc) << std::endl;
        return false;
    }
    
    // 设置硬件参数
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    
    rc = snd_pcm_hw_params_any(pcm_handle, hw_params);
    if (rc < 0) {
        std::cerr << "Cannot get hardware parameters: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    rc = snd_pcm_hw_params_set_access(pcm_handle, hw_params, 
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        std::cerr << "Cannot set access type: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    rc = snd_pcm_hw_params_set_format(pcm_handle, hw_params, 
                                        SND_PCM_FORMAT_S16_LE);
    if (rc < 0) {
        std::cerr << "Cannot set sample format: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    rc = snd_pcm_hw_params_set_channels(pcm_handle, hw_params, channels);
    if (rc < 0) {
        std::cerr << "Cannot set channel count: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    unsigned int hw_sample_rate = static_cast<unsigned int>(sample_rate);
    rc = snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, 
                                           &hw_sample_rate, 0);
    if (rc < 0) {
        std::cerr << "Cannot set sample rate: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    if (hw_sample_rate != static_cast<unsigned int>(sample_rate)) {
        std::cout << "Requested sample rate " << sample_rate 
                  << " Hz not supported, using " << hw_sample_rate << " Hz" << std::endl;
    }
    
    // 设置缓冲区大小和周期
    snd_pcm_uframes_t buffer_size;
    rc = snd_pcm_hw_params_get_buffer_size_max(hw_params, &buffer_size);
    if (buffer_size > static_cast<snd_pcm_uframes_t>(num_frames)) {
        snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, 
                                                &reinterpret_cast<snd_pcm_uframes_t&>(num_frames));
    }
    
    rc = snd_pcm_hw_params(pcm_handle, hw_params);
    if (rc < 0) {
        std::cerr << "Cannot set hardware parameters: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    // 设置软件参数
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    
    rc = snd_pcm_sw_params_current(pcm_handle, sw_params);
    if (rc < 0) {
        std::cerr << "Cannot get software parameters: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    rc = snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, 
                                                 static_cast<snd_pcm_uframes_t>(num_frames));
    if (rc < 0) {
        std::cerr << "Cannot set start threshold: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    rc = snd_pcm_sw_params(pcm_handle, sw_params);
    if (rc < 0) {
        std::cerr << "Cannot set software parameters: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    // 播放音频（写入数据）
    snd_pcm_sframes_t frames_written = 0;
    snd_pcm_uframes_t frames_to_write = static_cast<snd_pcm_uframes_t>(num_frames);
    
    // 循环写入直到所有数据都播放完毕
    while (frames_written < static_cast<snd_pcm_sframes_t>(num_frames)) {
        snd_pcm_uframes_t remaining = static_cast<snd_pcm_uframes_t>(num_frames - frames_written);
        
        rc = snd_pcm_writei(pcm_handle, 
                            buffer.data() + (frames_written * channels), 
                            remaining);
        
        if (rc == -EPIPE) {
            // 缓冲区下溢，需要恢复
            std::cerr << "Buffer underrun occurred" << std::endl;
            snd_pcm_prepare(pcm_handle);
            rc = snd_pcm_writei(pcm_handle, 
                               buffer.data() + (frames_written * channels), 
                               remaining);
        }
        
        if (rc < 0) {
            std::cerr << "Error writing to PCM device: " << snd_strerror(rc) << std::endl;
            snd_pcm_close(pcm_handle);
            return false;
        }
        
        if (rc > 0) {
            frames_written += rc;
        }
    }
    
    // 等待所有数据播放完成
    rc = snd_pcm_drain(pcm_handle);
    if (rc < 0) {
        std::cerr << "Error draining PCM device: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }
    
    snd_pcm_close(pcm_handle);
#endif
    
    return true;
}

} // extern "C"