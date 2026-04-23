#ifndef FFT_H
#define FFT_H

#ifndef N
#define N 32768
#endif

#ifndef DN
#define DN (N*2)
#endif

#define eps 1.0e-9

#ifdef __cplusplus
extern "C" {
#endif

void initial(void);
void fft1(float A[2][N], int ifft);
unsigned long int inverseq(unsigned long int i1,
                            unsigned long int imax);
void fft1r2(float A[2][N], float B[2][N], int ifft);
void fft1r1(float A[2][DN], int ifft);

#ifdef __cplusplus
}
#endif

#endif
