#include <stdio.h>
#include <math.h>

#define N 32768  // 必须与fft1.c中的N一致
#include "fft1.c"


void functf(double A[2][N]);

int main()
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
    printf("对应频率: %f Hz\n", peak_idx * 32768.0 / N);  // fs=256Hz
    
    return 0;
}
#include <math.h>

void functf(double A[2][N])
{
    unsigned long int i;
    double dt, tt;
    double freq = 10.0;      // 10Hz
    double fs = 32768.0;     // 采样率32768Hz
    
    dt = 1.0 / fs;
    
    for(i = 0; i < N; i++)
    {
        tt = (double)i * dt;
        A[0][i] = sin(2.0 * M_PI * freq * tt);  // 正弦波
        A[1][i] = 0.0;                           // 虚部为0
    }
}