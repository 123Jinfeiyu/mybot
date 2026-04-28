#include <cstdint>
#include <stdint.h>
#include <time.h>
#include <random>
#include "error.cuh"
extern "C" {
/* 创建一个设备数组 */
static void* _crtDevArr(const size_t m)
{
    void *d;
    CHECK(cudaMalloc(&d, m));
    return d;
}

/* 释放一个设备数组 */
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_Free(void* d)
#else
void Cu4cj_Free(void* d)
#endif
{
    CHECK(cudaFree(d));
}


/* 创建一个单精度浮点数组 输入数组的长度（不初始化） */
#ifdef _WIN64
__declspec(dllexport) void* Cu4cj_Orig(uint64_t n)
#else
void* Cu4cj_Orig(int64_t n)
#endif
{
    return _crtDevArr(n * sizeof(float));
}


/*
 * 从主机内存拷贝数据到设备内存
 * FIXME: 是否需要做成输入目的地址
 */  
#ifdef _WIN64
__declspec(dllexport) float* Cu4cj_ToDev(float* h, uint64_t n)
#else
float* Cu4cj_ToDev(float* h, int64_t n)
#endif
{
    float* d = (float*)Cu4cj_Orig(n);
    CHECK(cudaMemcpy(d, h, n * sizeof(float), cudaMemcpyHostToDevice));
    return d;
}


/* 从设备内存拷贝数据到主机内存 */
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_ToHost(float* d, float* h, const uint64_t n)
#else
void Cu4cj_ToHost(float* d, float* h, int64_t n)
#endif
{
    CHECK(cudaMemcpy(h, d, n * sizeof(float), cudaMemcpyDeviceToHost));
}


#ifdef _WIN64
__declspec(dllexport) void Cu4cj_Copy(float* d, float* h, const uint64_t n)
#else
void Cu4cj_Copy(float* d, float* h, int64_t n)
#endif
{
    CHECK(cudaMemcpy(h, d, n * sizeof(float), cudaMemcpyDeviceToHost));
}

/* 创建一个单精度浮点数组并将其初始化为零
 * FIXME: 当n == 10000 时进程被杀死 */
#ifdef _WIN64
__declspec(dllexport) float* Cu4cj_Zero(const uint64_t n)
#else
float* Cu4cj_Zero(int64_t n)
#endif
{
    float* d = (float*)Cu4cj_Orig(n);
    CHECK(cudaMemset(d, 0, n*sizeof(float)));
    return d;
}


/* 正态分布初始化（适用于神经网络权重） */
#ifdef _WIN64
__declspec(dllexport) float* Cu4cj_Normal(uint64_t n, uint64_t fan_in, uint64_t fan_out)
#else
float* Cu4cj_Normal(int64_t n, int64_t fan_in, int64_t fan_out)
#endif
{
    // 计算 Xavier/Glorot 初始化的标准差
    float std = sqrt(2.0f / (fan_in + fan_out));
    
    // 先在主机上分配内存并生成正态分布随机数
    float* h = (float*)malloc(n * sizeof(float));

    /* 初始化随机数种子 */
    srand(time(NULL));

    for (size_t i = 0; i < n; i++) {
        // 使用 Box-Muller 变换生成正态分布随机数
        float u1 = (float)rand() / RAND_MAX;
        float u2 = (float)rand() / RAND_MAX;
        float z = sqrt(-2.0f * log(u1)) * cos(2.0f * 3.1415926535f * u2);
        h[i] = z * std;
    }
    
    // 在设备上分配内存
    float* d = (float*)Cu4cj_Orig(n);
    
    // 将主机内存中的数据拷贝到设备内存
    CHECK(cudaMemcpy(d, h, n * sizeof(float), cudaMemcpyHostToDevice));
    
    // 释放主机内存
    free(h);
    
    return d;
}




// 向量加法
static void __global__ _devAdd(float* d1, float* d2, float* r, int64_t n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] + d2[i];
    }   
}

// 向量加法
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_Add(float* d1, float* d2, float* d_r, uint64_t n)
#else
void Cu4cj_Add(float* d1, float* d2, float* d_r, int64_t n)
#endif
{
    _devAdd<<<(n + 255) / 256, 256>>>(d1, d2, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 向量减法
static void __global__ _devSub(float* d1, float* d2, float* r, int64_t n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] - d2[i];
    }   
}

// 向量减法
#ifdef  _WIN64
__declspec(dllexport) void Cu4cj_Sub(float* d1, float* d2, float* d_r, uint64_t n)
#else
void Cu4cj_Sub(float* d1, float* d2, float* d_r, int64_t n)
#endif
{
    _devSub<<<(n + 255) / 256, 256>>>(d1, d2, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 向量依次相乘
static void __global__ _devMul(float* d1, float* d2, float* r, int64_t n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] * d2[i];
    }   
}

// 向量依次相乘
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_Mul(float* d1, float* d2, float* d_r, uint64_t n)
#else
void Cu4cj_Mul(float* d1, float* d2, float* d_r, int64_t n)
#endif
{
    _devMul<<<(n + 255) / 256, 256>>>(d1, d2, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 向量依次相除
static void __global__ _devDiv(float* d1, float* d2, float* r, int64_t n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] / d2[i];
    }   
}

// 向量依次相除
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_Div(float* d1, float* d2, float* d_r, uint64_t n)
#else
void Cu4cj_Div(float* d1, float* d2, float* d_r, int64_t n)
#endif
{
    _devDiv<<<(n + 255) / 256, 256>>>(d1, d2, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 向量加法广播
static void __global__ _devAddBroadcast(float* d1, float a, float* r, int64_t n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] + a;
    }   
}

// 加法广播
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_AddBroadcast(float* d1, float a, float* d_r, uint64_t n)
#else
void Cu4cj_AddBroadcast(float* d1, float a, float* d_r, int64_t n)
#endif
{
    _devAddBroadcast<<<(n + 255) / 256, 255>>>(d1, a, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 减法广播
static void __global__ _devSubBroadcast(float* d1, float a, float* r, int64_t n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] - a;   
    }   
}

// 减法广播
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_SubBroadcast(float* d1, float a, float* d_r, uint64_t n)
#else
void Cu4cj_SubBroadcast(float* d1, float a, float* d_r, int64_t n)
#endif
{
    _devSubBroadcast<<<(n + 255) / 256, 256>>>(d1, a, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 乘法广播
static void __global__ _devMulBroadcast(float* d1, float a, float* r, int64_t n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] * a;   
    }   
}

// 乘法广播
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_MulBroadcast(float* d1, float a, float* d_r, uint64_t n)
#else
void Cu4cj_MulBroadcast(float* d1, float a, float* d_r, int64_t n)
#endif
{
    _devMulBroadcast<<<(n + 255) / 256, 256>>>(d1, a, d_r, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 除法广播
static void __global__ _devDivBroadcast(float* d1, float a, float* r, int64_t n) {   
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        r[i] = d1[i] / a;
    }   
}

// 除法广播
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_DivBroadcast(float* d1, float a, float* d_r, uint64_t n)
#else
void Cu4cj_DivBroadcast(float* d1, float a, float* d_r, int64_t n)
#endif
{
    _devDivBroadcast<<<(n + 255) / 256, 256>>>(d1, a, d_r, n);  
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

// 矩阵乘法
static void __global__ _devMatDot(float* d1, float* d2, float* r, int64_t m, int64_t n, int64_t k) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i < m && j < k) {
        float sum = 0.0f;
        for (int p = 0; p < n; p++) {
            sum += d1[i * n + p] * d2[p * k + j];
        }
        r[i * k + j] = sum;
    }
}

// 矩阵乘法
// m、n、k 分别为矩阵 A、B、C 的行数、列数
// 其中 n 为矩阵 A 的列数，也为矩阵 B 的行数
#ifdef _WIN64
__declspec(dllexport) void Cu4cj_MatDot(float* d1, float* d2, float* d_r, uint64_t m, uint64_t n, uint64_t k)
#else
void Cu4cj_MatDot(float* d1, float* d2, float* d_r, int64_t m, int64_t n, int64_t k)
#endif
{
    dim3 blockDim(16, 16);
    dim3 gridDim((m + blockDim.x - 1) / blockDim.x, (k + blockDim.y - 1) / blockDim.y);
    _devMatDot<<<gridDim, blockDim>>>(d1, d2, d_r, m, n, k);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
}

}

namespace qing {
    class Matrix {
    public:
        Matrix(uint64_t r, uint64_t c) {
            this->r = r;
            this->c = c;
            this->d = (float*)Cu4cj_Zero(r * c * sizeof(float));
        }
        Matrix(uint64_t r, uint64_t c, float* d) {
            this->r = r;
            this->c = c;
            this->d = d;
        }
        Matrix(const Matrix& m) {
            this->r = m.getR();
            this->c = m.getC();
            this->d = (float*)Cu4cj_Zero(m.getR() * m.getC() * sizeof(float));
            Cu4cj_Copy(m.getD(), d, m.getR() * m.getC() * sizeof(float));
        }
        ~Matrix() {
            Cu4cj_Free(d);
        }
        uint64_t getR() const {
            return r;
        }
        uint64_t getC() const {
            return c;
        }
        float* getD() const {
            return d;
        }
        float get(uint64_t r, uint64_t c) const {
            float h;
            CHECK(cudaMemcpy(&h, &d[c * this->r + r], sizeof(float), cudaMemcpyDeviceToHost));
            return h;
        }
        void set(uint64_t r, uint64_t c, float v) {
            CHECK(cudaMemcpy(&d[c * this->r + r], &v, sizeof(float), cudaMemcpyHostToDevice));
        }
        bool hasSameShape(const Matrix& m) {
            return r == m.getR() && c == m.getC();
        }
        bool hasDotShape(const Matrix& m) {
            return c == m.getR();
        }
        Matrix operator+(const Matrix& m) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_Add(d, m.getD(), d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator+(float a) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_AddBroadcast(d, a, d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator-(const Matrix& m) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_Sub(d, m.getD(), d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator-(float a) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_SubBroadcast(d, a, d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator*(const Matrix& m) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_Mul(d, m.getD(), d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator*(float a) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_MulBroadcast(d, a, d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator/(const Matrix& m) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_Div(d, m.getD(), d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix operator/(float a) {
            float *d_r = (float*)Cu4cj_Zero(r * c * sizeof(float));
            Cu4cj_DivBroadcast(d, a, d_r, r * c);
            return Matrix(r, c, d_r);
        }
        Matrix dot(const Matrix& m) {
            //if (!hasDotShape(m)) {
            //    throw std::invalid_argument("Dot product: matrices must have compatible shapes");
            //}
            float *d_r = (float*)Cu4cj_Zero(r * m.getC() * sizeof(float));
            Cu4cj_MatDot(d, m.getD(), d_r, r, c, m.getC());
            return Matrix(r, m.getC(), d_r);
        }  

    private:
        uint64_t r, c;
        float* d;
    };
}

using namespace qing;
extern "C" {
    

#ifdef _WIN64
__declspec(dllexport) void* createMatrix(const uint64_t r, const uint64_t c)
#else
void* createMatrix(int64_t r, int64_t c)
#endif
{
    return(void*) new Matrix(r, c);
}

#ifdef _WIN64
__declspec(dllexport) void freeMatrix(void* m)
#else
void freeMatrix(void* m)
#endif
{
    delete (Matrix*)m;
}

#ifdef _WIN64
__declspec(dllexport) void* xavierNormalInit(uint64_t fan_in, uint64_t fan_out)
#else
void* xavierNormalInit(uint64_t fan_in, uint64_t fan_out)
#endif
{
    float* d = Cu4cj_Normal(fan_in * fan_out, fan_in, fan_out); 
    return (void *)new Matrix(fan_in, fan_out, d);
}

#ifdef _WIN64
__declspec(dllexport) void* xavierUniformInit(uint64_t fan_in, uint64_t fan_out)
#else
void* xavierUniformInit(uint64_t fan_in, uint64_t fan_out)
#endif
{
    uint64_t n = fan_in * fan_out;

    // 先在主机上分配内存并生成正态分布随机数
    float* h = (float*)malloc(n * sizeof(float));

    /* 使用高质量的随机数生成器 */
    static std::mt19937 generator(std::random_device{}());

    /* 计算区间范围：sqrt(2 / (fan_in + fan_out)) */
    float scale = std::sqrt(2.0f / (fan_in + fan_out));

    /* 创建均匀分布对象，范围为[-scale, scale] */
    std::uniform_real_distribution<float> distribution(-scale, scale);

    /* 遍历权重矩阵，进行赋值 */
    uint64_t k=0;
    for (uint64_t i=0; i<fan_in; ++i)
        for (uint64_t j=0; j<fan_out; ++j)
            h[k++] = distribution(generator);

            
    
    // 在设备上分配内存
    float* d = (float*)Cu4cj_Orig(n*sizeof(float));
    
    // 将主机内存中的数据拷贝到设备内存
    CHECK(cudaMemcpy(d, h, n * sizeof(float), cudaMemcpyHostToDevice));
    
    // 释放主机内存
    free(h);
    
    return (void *)new Matrix(fan_in, fan_out, d);
}

#ifdef _WIN64
__declspec(dllexport) uint64_t getrowMatrix(void *m)
#else
uint64_t getrowMatrix(void *m)
#endif
{
    Matrix* m1 = (Matrix*)m;
    return m1->getR();
}
#ifdef _WIN64
__declspec(dllexport) uint64_t getcolMatrix(void *m)
#else
uint64_t getcolMatrix(void *m)
#endif
{
    Matrix* m1 = (Matrix*)m;
    return m1->getC();    
}

#ifdef _WIN64
__declspec(dllexport) float getMatrix(void *m, uint64_t r, uint64_t c)
#else
float getMatrix(void *m, uint64_t r, uint64_t c)
#endif
{
    Matrix* m1 = (Matrix*)m;
    return m1->get(r, c);
}

#ifdef _WIN64
__declspec(dllexport) void setMatrix(void *m, uint64_t r, uint64_t c, float v)
#else
void setMatrix(void *m, uint64_t r, uint64_t c, float v)
#endif
{
    Matrix* m1 = (Matrix*)m;
    m1->set(r, c, v);
}

#ifdef _WIN64
__declspec(dllexport) void* plusMatrix(void *m, void *s)
#endif
#ifdef __linux__
void* plusMatrix(void *m, void *s)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix* s1 = (Matrix*)s;
    Matrix r = *m1 + *s1;
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* plusBroadcastMatrix(void *m, float a)
#else
void* plusBroadcastMatrix(void *m, float a)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix r = *m1 + a;
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* subMatrix(void *m, void *s)
#else
void* subMatrix(void *m, void *s)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix* s1 = (Matrix*)s;
    Matrix r = *m1 - *s1;
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* subBroadcastMatrix(void *m, float a)
#else
void* subBroadcastMatrix(void *m, float a)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix r = *m1 - a;
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* multiMatrix(void *m, void *s)
#else
void* multiMatrix(void *m, void *s)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix* s1 = (Matrix*)s;
    Matrix r = (*m1) * (*s1);
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* multiBroadcastMatrix(void *m, float a)
#else
void* multiBroadcastMatrix(void *m, float a)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix r = (*m1) * a;
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* divMatrix(void *m, void *s)
#else
void* divMatrix(void *m, void *s)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix* s1 = (Matrix*)s;
    Matrix r = (*m1) / (*s1);
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* divBroadcastMatrix(void *m, float a)
#else
void* divBroadcastMatrix(void *m, float a)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix r = (*m1) / a;
    return (void *)new Matrix(r);
}

#ifdef _WIN64
__declspec(dllexport) void* dotMatrix(void *m, void *s)
#else
void* dotMatrix(void *m, void *s)
#endif
{
    Matrix* m1 = (Matrix*)m;
    Matrix* s1 = (Matrix*)s;
    Matrix r = (*m1).dot(*s1);
    return (void *)new Matrix(r);
}



}