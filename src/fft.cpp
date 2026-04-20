#include <math.h>
#include <vector>
#define N 65536
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
    double p = 0.1;
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

extern "C" {

int test()
{
    double A[2][N];
    
    printf("生成正弦波数据...\n");
    functf(A);
    
    printf("\n输入数据检查（前5个点）：\n");
    for(int i = 0; i < 5; i++) {
        printf("  x[%d] = %f + %fi\n", i, A[0][i], A[1][i]);
    }
    
    printf("\n执行FFT...\n");
    fft1(A, 1);  // 1 = FFT, -1 = IFFT
    
    printf("\nFFT结果（前10个点）：\n");
    for(int i = 0; i < 10; i++) {
        double mag = sqrt(A[0][i]*A[0][i] + A[1][i]*A[1][i]);
        printf("  F[%d] = %f + %fi (幅度: %f)\n", 
               i, A[0][i], A[1][i], mag);
    }
    
    // 找到峰值
    double max_mag = 0;
    int peak_idx = 0;
    for(int i = 0; i < N/2; i++) {
        double mag = sqrt(A[0][i]*A[0][i] + A[1][i]*A[1][i]);
        if(mag > max_mag) {
            max_mag = mag;
            peak_idx = i;
        }
    }
    
    printf("\n峰值出现在索引 %d，幅度 %f\n", peak_idx, max_mag);
    printf("对应频率: %f Hz\n", peak_idx * 65536.0 / N);  // fs=256Hz
    
    return 0;
}
    // 初始化FFT数组
    //static void fft_init(double A[2][N], Matrx<double>& M)
    //{
    //    for (int i = 0; i < N; i++) {
    //        double a =  M.witch(i, 0);
    //        A[0][i] = a;
    //        A[1][i] = 0;
    //    }
    //}

    static void fft_init(double A[2][N], double M[N])
    {
        for (int i = 0; i < N; i++) {
            A[0][i] = M[i];
            A[1][i] = 0;
        }
    }

    // 计算FFT结果
    static void fft_result(std::vector<double>& R, double A[2][N])
    {
        for (int i = 0; i < N/2; i++) {
            double a = pow(A[0][i], 2) + pow(A[1][i], 2);
            R[i] = sqrt(a) / (N/2);
        }
    }

    // 开始FFT
    void* start_fft(void* M)
    {
        double A[2][N];
	functf(A);

        //std::vector<double> R(N/2);
        fft1(A, 1);
        //fft_result(R, A);
        //return (void*) new Matrx<double>(N/2, 1, R);
	return NULL;
    }

    // 生成正弦波
    void* gen_wave()
    {
        std::vector<double> A(N);
        functf1(A.data());
        return (void *)new Matrx<double>(N, 1, A);
    }
}

