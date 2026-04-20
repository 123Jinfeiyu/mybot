#include <math.h>
#include <vector>
extern "C" {
    #include "fft.h"
}
#include "Matrix.hh"
using namespace qing;

extern "C" {

    // 初始化FFT数组
    static void fft_init(double A[2][N], Matrx<double>& M)
    {
        for (int i = 0; i < N; i++) {
            A[0][i] = M.get_data()[i];
            A[1][i] = 0;
        }
    }

    // 计算FFT结果
    static void fft_result(std::vector<double>& R, double A[2][N])
    {
        for (int i = 0; i < N/2; i++) {
            double a = pow(A[0][i], 2) + pow(A[1][i], 2);
            R[i] = sqrt(a);
        }
    }

    // 开始FFT
    Matrx<double> start_fft(Matrx<double>& M)
    {
        double A[2][N];
        std::vector<double> R(N/2);
        fft_init(A, M);
        fft1(A, 0);
        fft_result(R, A);
        return Matrx<double>(N/2, 1, R);
    }

    // 生成正弦波
    Matrx<double> gen_wave1()
    {
        std::vector<double> A(N);
        unsigned long int i;
        double dt, tt;
        double freq = 10.0;      // 10Hz
        double fs = 65536.0;     // 采样率65536Hz
        
        dt = 1.0 / fs;
        
        for(i = 0; i < N; i++)
        {
            tt = (double)i * dt;
            A[i] = sin(2.0 * M_PI * freq * tt);  // 正弦波
        }

        return Matrx<double>(N, 1, A);
    }
}

