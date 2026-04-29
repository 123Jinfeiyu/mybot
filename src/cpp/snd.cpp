#ifdef _WIN32
    #include <windows.h>
    #include <mmsystem.h>
    #pragma comment(lib, "winmm.lib")
    #define _USE_MATH_DEFINES
    #include <cmath>
#else
    #include <alsa/asoundlib.h>
    #include <cmath>
#endif

#include <iostream>
#include <cstdint>
#include <vector>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

extern "C" {

// frequency: Frequency in Hz
// duration_ms: Duration in milliseconds
// volume: Volume level (0.0 ~ 1.0)
// channels: Number of channels (1=mono, 2=stereo)
// sample_rate: Sample rate in Hz
#ifdef _WIN32
#define SND_EXPORT __declspec(dllexport)
#endif
SND_EXPORT bool play_frequency(float frequency, int32_t duration_ms, 
                    double volume, int32_t channels, 
                    int32_t sample_rate) {
    if (frequency <= 0.0f || duration_ms <= 0 || 
        volume <= 0.0 || volume > 1.0 || 
        channels <= 0 || channels > 8 ||
        sample_rate <= 0) {
        std::cerr << "Invalid parameters" << std::endl;
        return false;
    }
    
    // Generate sine wave data
    int32_t num_frames = static_cast<int64_t>(sample_rate) * duration_ms / 1000;
    int32_t total_samples = num_frames * channels;
    std::vector<int16_t> buffer(total_samples);
    
    int32_t amplitude = static_cast<int32_t>(32767.0 * volume);
    
    for (int32_t i = 0; i < num_frames; i++) {
        int16_t sample = static_cast<int16_t>(
            amplitude * sin(2.0 * M_PI * frequency * i / sample_rate)
        );
        
        // Fill all channels with the same value
        for (int32_t ch = 0; ch < channels; ch++) {
            buffer[i * channels + ch] = sample;
        }
    }
    
#ifdef _WIN32
    // Windows implementation
    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = channels * 2;
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
    wfx.cbSize = 0;
    
    HWAVEOUT hWaveOut;
    MMRESULT result = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        std::cerr << "Cannot open audio device" << std::endl;
        return false;
    }
    
    WAVEHDR header;
    header.lpData = reinterpret_cast<char*>(buffer.data());
    header.dwBufferLength = total_samples * sizeof(int16_t);
    header.dwFlags = 0;
    header.dwLoops = 0;
    
    result = waveOutPrepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        waveOutClose(hWaveOut);
        std::cerr << "Failed to prepare audio header" << std::endl;
        return false;
    }
    
    result = waveOutWrite(hWaveOut, &header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
        waveOutClose(hWaveOut);
        std::cerr << "Failed to write audio data" << std::endl;
        return false;
    }
    
    // Wait for playback to complete
    while (!(header.dwFlags & WHDR_DONE)) {
        Sleep(10);
    }
    
    waveOutUnprepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
    waveOutClose(hWaveOut);
    
#else
    // Linux ALSA implementation
    snd_pcm_t *pcm_handle = nullptr;
    const char* device = "hw:0,0";  // USB audio device
    
    int rc = snd_pcm_open(&pcm_handle, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        std::cerr << "Cannot open device: " << snd_strerror(rc) << std::endl;
        return false;
    }
    
    // Set hardware parameters
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_handle, params);
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_handle, params, channels);
    
    unsigned int sr = sample_rate;
    snd_pcm_hw_params_set_rate_near(pcm_handle, params, &sr, 0);
    snd_pcm_hw_params(pcm_handle, params);
    
    // Play audio
    rc = snd_pcm_writei(pcm_handle, buffer.data(), num_frames);
    if (rc == -EPIPE) {
        snd_pcm_prepare(pcm_handle);
        snd_pcm_writei(pcm_handle, buffer.data(), num_frames);
    }
    
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
#endif
    
    return true;
}
}