#include <math.h>
#include <vector>
#include "fft.h"
#include "Matrix.hh"
using namespace qing;


void functf1(double A[N])
{
    unsigned long int i;
    double dt, tt;
    double p = 0.1;
    double freq = 10.0;      // 10Hz
    double fs = N;     // 采样率65536Hz
    
    dt = 1.0 / fs;
    
    for(i = 0; i < N; i++)
    {
        tt = (double)i * dt;
        A[i] = sin(2.0 * M_PI * freq * tt) * p;  // 正弦波
        //A[1][i] = 0.0;                           // 虚部为0
    }
}

void functf(double A[2][N])
{
    unsigned long int i;
    double dt, tt;
    double p = 1;
    double freq = 10.0;      // 10Hz
    double fs = N;     // 采样率65536Hz
    
    
    dt = 1.0 / fs;
    
    for(i = 0; i < N; i++)
    {
        tt = (double)i * dt;
        A[0][i] = sin(2.0 * M_PI * freq * tt) * p;  // 正弦波
        A[1][i] = 0.0;                           // 虚部为0
    }
}

// 初始化FFT数组
static void fft_init(double A[2][N], Matrx<double>& M)
{
    for (int i = 0; i < N; i++) {
        double a =  M.witch(i, 0);
        A[0][i] = a;
        A[1][i] = 0;
    }
}

// 计算FFT结果
static void fft_result(std::vector<double>& R, double A[2][N])
{
    for (int i = 0; i < N/2; i++) {
        //double a = pow(A[0][i], 2) + pow(A[1][i], 2);
        //R[i] = sqrt(a) / (N/2);
    }
}

extern "C" {

    // 生成正弦波
    void* gen_wave()
    {
        std::vector<double> A(N);
        functf1(A.data());
        return (void *)new Matrx<double>(N, 1, A);
    }

    // 开始FFT
    void* start_fft(void* M)
    {
        double A[2][N];
        std::vector<double> R(N/2);
        fft_init(A, *(Matrx<double> *)M);
        fft1(A, 1);
        fft_result(R, A);
        return (void*) new Matrx<double>(N/2, 1, R);
    }

}

