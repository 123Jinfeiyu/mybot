#include <math.h>
#include <vector>
#include "fft.h"
#include "Matrix.hh"
using namespace qing;



void functf1d(float A[DN])
{
    unsigned long int i;
    float dt, tt;
    float p = 0.1;
    float freq = 10.0;      // 10Hz
    float fs = DN;     // 采样率65536Hz
    
    dt = 1.0 / fs;
    
    for(i = 0; i < DN; i++)
    {
        tt = (float)i * dt;
        A[i] = sin(2.0 * M_PI * freq * tt) * p;  // 正弦波
        //A[1][i] = 0.0;                           // 虚部为0
    }
}


// 初始化FFT数组
static void fft_init(float A[2][DN], Matrx<float>& M)
{
    for (int i = 0; i < DN; i++) {
        float a =  M.witch(i, 0);
        A[0][i] = a;
        A[1][i] = 0;
    }
}

// 计算FFT结果
static void fft_result(std::vector<float>& R, float A[2][DN])
{
    for (int i = 0; i < DN/2; i++) {
        float a = pow(A[0][i], 2) + pow(A[1][i], 2);
        R[i] = sqrt(a) / (DN/2);
        //std::cout << i << ":\t" << R[i] << std::endl; 
    }
}

extern "C" {

    // 生成正弦波
    void* gen_wave()
    {
        std::vector<float> A(DN);
        functf1d(A.data());
        return (void *)new Matrx<float>(DN, 1, A);
    }

    // 开始FFT
    void* start_fft(void* M)
    {
        float A[2][DN];
        std::vector<float> R(N);
        fft_init(A, *(Matrx<float> *)M);
        //fft1(A, 1);	// 复序列基2 FFT
        fft1r1(A, 1);   // 单实序列
        fft_result(R, A);
        return (void*) new Matrx<float>(N/2, 1, R);
    }

}

