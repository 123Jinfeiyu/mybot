#pragma once

// CImg 配置（必须在包含 CImg.h 之前定义）
#define cimg_use_png   // 需要 -lpng
#define cimg_use_jpeg  // 需要 -ljpeg
#define cimg_display 0 // 禁用显示窗口，用于服务器环境
#include "CImg.h"

// 标准库
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>
#include <cassert>
#include <limits>
#include <tuple>

// 系统库（文件操作）
#include <sys/stat.h>
#include <dirent.h>

// 项目头文件
#include "ConvLayer.hpp"

class ConvLayer {
private:
    // 参数
    int input_channels;
    int output_channels;
    int kernel_size;
    int stride;
    int padding;
    int input_height;
    int input_width;
    int output_height;
    int output_width;
    
    // Leaky ReLU参数
    float negative_slope;
    
    // 多batch缓存支持
    int cached_batch_size;
    
    // 权重和偏置
    std::vector<float> weights;  // shape: [output_channels, input_channels, kernel_size, kernel_size]
    std::vector<float> bias;     // shape: [output_channels]
    
    // 梯度和动量
    std::vector<float> d_weights;
    std::vector<float> d_bias;
    std::vector<float> weight_momentum;
    std::vector<float> bias_momentum;
    
    // Adam优化器状态
    std::vector<float> m_weights;
    std::vector<float> v_weights;
    std::vector<float> m_bias;
    std::vector<float> v_bias;
    
    // 中间变量缓存
    std::vector<float> col_buffer;      // im2col后的矩阵 [K, N]
    std::vector<float> input_cache;     // 缓存的完整输入
    std::vector<float> output_cache;    // 缓存的输出（激活前）
    std::vector<float> activated_cache; // 激活后的输出缓存
    std::vector<float> col_cache;       // 缓存的col矩阵（支持多batch）
    
    // 辅助函数
    void im2col(const float* input);
    void col2im(const float* col, float* im);
    
    void sgemm(int M, int N, int K, 
               const float* A, const float* B, float* C,
               bool transA = false, bool transB = false);
    
    void sgemm_add(int M, int N, int K,
                   const float* A, const float* B, float* C,
                   float alpha = 1.0f, float beta = 1.0f,
                   bool transA = false, bool transB = false);
    
    // Leaky ReLU激活函数及其导数
    inline float leaky_relu(float x) const {
        return x > 0 ? x : negative_slope * x;
    }
    
    inline float leaky_relu_derivative(float x) const {
        return x > 0 ? 1.0f : negative_slope;
    }
    
public:
    // 构造函数
    ConvLayer(int in_channels, int out_channels, 
              int kernel_size, int stride = 1, int padding = 0, 
              float negative_slope = 0.01f)
        : input_channels(in_channels),
          output_channels(out_channels),
          kernel_size(kernel_size),
          stride(stride),
          padding(padding),
          negative_slope(negative_slope),
          cached_batch_size(0),
          input_height(0),
          input_width(0),
          output_height(0),
          output_width(0) {
        
        // 验证参数
        if (in_channels <= 0 || out_channels <= 0 || kernel_size <= 0) {
            throw std::invalid_argument("Invalid ConvLayer parameters");
        }
        
        // 初始化权重和偏置
        int weight_size = out_channels * in_channels * kernel_size * kernel_size;
        weights.resize(weight_size);
        d_weights.resize(weight_size, 0.0f);
        weight_momentum.resize(weight_size, 0.0f);
        
        // 初始化Adam状态
        m_weights.resize(weight_size, 0.0f);
        v_weights.resize(weight_size, 0.0f);
        
        bias.resize(out_channels);
        d_bias.resize(out_channels, 0.0f);
        bias_momentum.resize(out_channels, 0.0f);
        
        // 初始化Adam偏置状态
        m_bias.resize(out_channels, 0.0f);
        v_bias.resize(out_channels, 0.0f);
        
        // He初始化
        float std_dev = sqrt(2.0f / (in_channels * kernel_size * kernel_size));
        std::default_random_engine generator(std::random_device{}());
        std::normal_distribution<float> distribution(0.0f, std_dev);
        
        for (int i = 0; i < weight_size; i++) {
            weights[i] = distribution(generator);
        }
        
        for (int i = 0; i < out_channels; i++) {
            bias[i] = 0.1f;
        }
    }
    
    // 设置输入尺寸
    void set_input_shape(int height, int width) {
        if (height <= 0 || width <= 0) {
            throw std::invalid_argument("Invalid input dimensions");
        }
        
        input_height = height;
        input_width = width;
        
        output_height = (height + 2 * padding - kernel_size) / stride + 1;
        output_width = (width + 2 * padding - kernel_size) / stride + 1;
        
        // 验证输出尺寸
        if (output_height <= 0 || output_width <= 0) {
            throw std::invalid_argument("Invalid output dimensions. Check kernel_size, stride, and padding.");
        }
        
        // 预分配缓冲区
        int K = input_channels * kernel_size * kernel_size;
        int N = output_height * output_width;
        col_buffer.resize(K * N);
        
        // 清空缓存
        clear_cache();
    }
    
    // 清空缓存
    void clear_cache() {
        input_cache.clear();
        output_cache.clear();
        activated_cache.clear();
        col_cache.clear();
        cached_batch_size = 0;
    }
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true) {
        // 验证输入尺寸
        int expected_size = batch_size * input_channels * input_height * input_width;
        if (input.size() != expected_size) {
            throw std::invalid_argument("Input size mismatch");
        }
        
        int M = output_channels;
        int N = output_height * output_width;
        int K = input_channels * kernel_size * kernel_size;
        int output_size = batch_size * M * N;
        
        std::vector<float> output(output_size, 0.0f);
        std::vector<float> pre_activation(output_size, 0.0f);
        
        // 如果是训练模式，缓存完整输入和batch大小
        if (training) {
            input_cache = input;
            cached_batch_size = batch_size;
            col_cache.resize(K * N * batch_size);
        }
        
        // 处理每个batch
        for (int b = 0; b < batch_size; b++) {
            const float* input_ptr = input.data() + b * input_channels * input_height * input_width;
            
            // 1. im2col转换
            im2col(input_ptr);
            
            // 2. 缓存col矩阵
            if (training) {
                std::copy(col_buffer.begin(), col_buffer.end(), 
                         col_cache.begin() + b * K * N);
            }
            
            // 3. 矩阵乘法 weights [M, K] * col [K, N] = result [M, N]
            std::vector<float> gemm_result(M * N, 0.0f);
            sgemm(M, N, K, weights.data(), col_buffer.data(), gemm_result.data());
            
            // 4. 加偏置并激活
            float* pre_act_ptr = pre_activation.data() + b * M * N;
            float* out_ptr = output.data() + b * M * N;
            
            for (int oc = 0; oc < M; oc++) {
                float bias_val = bias[oc];
                for (int i = 0; i < N; i++) {
                    float val = gemm_result[oc * N + i] + bias_val;
                    pre_act_ptr[oc * N + i] = val;
                    out_ptr[oc * N + i] = leaky_relu(val);
                }
            }
        }
        
        // 缓存激活前后的输出
        if (training) {
            output_cache = std::move(pre_activation);    // 激活前
            activated_cache = output;                     // 激活后
        }
        
        return output;
    }
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = -1) {
        // 如果没有指定batch_size，使用缓存的batch_size
        if (batch_size <= 0) {
            batch_size = cached_batch_size;
        }
        
        int N = output_height * output_width;
        int K = input_channels * kernel_size * kernel_size;
        int M = output_channels;
        
        // 验证梯度尺寸
        int expected_size = batch_size * M * N;
        if (grad_output.size() != expected_size) {
            throw std::invalid_argument("Gradient size mismatch: expected " + 
                                       std::to_string(expected_size) + " but got " + 
                                       std::to_string(grad_output.size()));
        }
        
        if (output_cache.empty() || col_cache.empty()) {
            throw std::runtime_error("No cached data. Run forward with training=true first.");
        }
        
        // 验证缓存的batch_size和当前batch_size一致
        if (batch_size != cached_batch_size) {
            throw std::runtime_error("Batch size mismatch with cached data. Expected " +
                                    std::to_string(cached_batch_size) + " but got " +
                                    std::to_string(batch_size));
        }
        
        // 1. 计算激活函数的梯度
        std::vector<float> grad_pre_activation = grad_output;
        
        for (int b = 0; b < batch_size; b++) {
            const float* pre_act_ptr = output_cache.data() + b * M * N;
            float* grad_ptr = grad_pre_activation.data() + b * M * N;
            
            for (int i = 0; i < M * N; i++) {
                grad_ptr[i] *= leaky_relu_derivative(pre_act_ptr[i]);
            }
        }
        
        // 2. 计算偏置梯度
        std::fill(d_bias.begin(), d_bias.end(), 0.0f);
        for (int b = 0; b < batch_size; b++) {
            const float* grad_ptr = grad_pre_activation.data() + b * M * N;
            for (int oc = 0; oc < M; oc++) {
                float sum = 0.0f;
                for (int i = 0; i < N; i++) {
                    sum += grad_ptr[oc * N + i];
                }
                d_bias[oc] += sum;
            }
        }
        // 平均偏置梯度
        for (int oc = 0; oc < M; oc++) {
            d_bias[oc] /= batch_size;
        }
        
        // 3. 计算权重梯度
        std::fill(d_weights.begin(), d_weights.end(), 0.0f);
        
        // 将grad_pre_activation reshape为矩阵 [M, N*batch_size]
        std::vector<float> grad_mat(M * N * batch_size, 0.0f);
        for (int b = 0; b < batch_size; b++) {
            const float* grad_ptr = grad_pre_activation.data() + b * M * N;
            for (int oc = 0; oc < M; oc++) {
                for (int i = 0; i < N; i++) {
                    grad_mat[oc * (N * batch_size) + b * N + i] = grad_ptr[oc * N + i];
                }
            }
        }
        
        // d_weights = grad_mat * col_cache^T / batch_size
        // grad_mat: [M, N*batch_size], col_cache: [K, N*batch_size]
        sgemm_add(M, K, N * batch_size, 
                 grad_mat.data(), col_cache.data(), d_weights.data(),
                 1.0f / batch_size, 0.0f, false, true);
        
        // 4. 计算输入梯度
        std::vector<float> d_input(batch_size * input_channels * input_height * input_width, 0.0f);
        
        // d_input_col = weights^T * grad_mat
        // weights: [M, K], grad_mat: [M, N*batch_size], d_input_col: [K, N*batch_size]
        std::vector<float> d_input_col(K * N * batch_size, 0.0f);
        sgemm(K, N * batch_size, M, 
              weights.data(), grad_mat.data(), d_input_col.data(),
              true, false);
        
        // 将d_input_col通过col2im转换回输入形状
        for (int b = 0; b < batch_size; b++) {
            float* d_input_ptr = d_input.data() + b * input_channels * input_height * input_width;
            const float* d_input_col_ptr = d_input_col.data() + b * K * N;
            
            // 注意：这里创建临时缓冲区，避免修改col_buffer影响col_cache
            std::vector<float> temp_col_buffer(K * N);
            std::copy(d_input_col_ptr, d_input_col_ptr + K * N, temp_col_buffer.begin());
            col2im(temp_col_buffer.data(), d_input_ptr);
        }
        
        return d_input;
    }
    
    // 更新参数（SGD with momentum）
    void update_parameters(float learning_rate = 0.01f, float momentum = 0.9f) {
        for (size_t i = 0; i < weights.size(); i++) {
            weight_momentum[i] = momentum * weight_momentum[i] - learning_rate * d_weights[i];
            weights[i] += weight_momentum[i];
        }
        
        for (size_t i = 0; i < bias.size(); i++) {
            bias_momentum[i] = momentum * bias_momentum[i] - learning_rate * d_bias[i];
            bias[i] += bias_momentum[i];
        }
    }
    
    // Adam优化器更新参数
    void update_parameters_adam(float learning_rate = 0.001f, 
                               float beta1 = 0.9f, float beta2 = 0.999f,
                               float epsilon = 1e-8f, int t = 1) {
        for (size_t i = 0; i < weights.size(); i++) {
            m_weights[i] = beta1 * m_weights[i] + (1 - beta1) * d_weights[i];
            v_weights[i] = beta2 * v_weights[i] + (1 - beta2) * d_weights[i] * d_weights[i];
            
            float m_hat = m_weights[i] / (1 - powf(beta1, t));
            float v_hat = v_weights[i] / (1 - powf(beta2, t));
            
            weights[i] -= learning_rate * m_hat / (sqrtf(v_hat) + epsilon);
        }
        
        for (size_t i = 0; i < bias.size(); i++) {
            m_bias[i] = beta1 * m_bias[i] + (1 - beta1) * d_bias[i];
            v_bias[i] = beta2 * v_bias[i] + (1 - beta2) * d_bias[i] * d_bias[i];
            
            float m_hat = m_bias[i] / (1 - powf(beta1, t));
            float v_hat = v_bias[i] / (1 - powf(beta2, t));
            
            bias[i] -= learning_rate * m_hat / (sqrtf(v_hat) + epsilon);
        }
    }
    
    // Getters
    const std::vector<float>& get_weights() const { return weights; }
    const std::vector<float>& get_bias() const { return bias; }
    const std::vector<float>& get_weight_gradients() const { return d_weights; }
    const std::vector<float>& get_bias_gradients() const { return d_bias; }
    
    // Setters
    void set_weights(const std::vector<float>& new_weights) {
        if (new_weights.size() != weights.size()) {
            throw std::invalid_argument("Weight size mismatch");
        }
        weights = new_weights;
    }
    
    void set_bias(const std::vector<float>& new_bias) {
        if (new_bias.size() != bias.size()) {
            throw std::invalid_argument("Bias size mismatch");
        }
        bias = new_bias;
    }
    
    void set_leaky_relu_slope(float slope) { 
        negative_slope = slope; 
    }
    
    // 获取输出尺寸
    std::tuple<int, int> get_output_shape() const {
        return std::make_tuple(output_height, output_width);
    }
    
    // 获取输入输出通道数
    int get_input_channels() const { return input_channels; }
    int get_output_channels() const { return output_channels; }
    
    // 梯度检查
    float gradient_check(const std::vector<float>& input, 
                        const std::vector<float>& grad_output,
                        int weight_idx, float epsilon = 1e-5f) {
        if (weight_idx < 0 || weight_idx >= weights.size()) {
            throw std::out_of_range("Weight index out of range");
        }
        
        // 保存原始权重
        float original_weight = weights[weight_idx];
        
        // 先进行一次正常的前向+反向传播来计算解析梯度
        forward(input, 1, true);
        backward(grad_output, 1);
        float analytical_grad = d_weights[weight_idx];
        
        // 计算数值梯度
        // f(x + epsilon)
        weights[weight_idx] = original_weight + epsilon;
        auto output_plus = forward(input, 1, false);
        
        // f(x - epsilon)
        weights[weight_idx] = original_weight - epsilon;
        auto output_minus = forward(input, 1, false);
        
        // 恢复权重
        weights[weight_idx] = original_weight;
        
        // 计算数值梯度
        float numerical_grad = 0.0f;
        for (size_t i = 0; i < output_plus.size(); i++) {
            numerical_grad += grad_output[i] * (output_plus[i] - output_minus[i]) / (2.0f * epsilon);
        }
        
        return fabs(numerical_grad - analytical_grad);
    }
    
    // 重置优化器状态
    void reset_optimizer_state() {
        std::fill(m_weights.begin(), m_weights.end(), 0.0f);
        std::fill(v_weights.begin(), v_weights.end(), 0.0f);
        std::fill(m_bias.begin(), m_bias.end(), 0.0f);
        std::fill(v_bias.begin(), v_bias.end(), 0.0f);
        std::fill(weight_momentum.begin(), weight_momentum.end(), 0.0f);
        std::fill(bias_momentum.begin(), bias_momentum.end(), 0.0f);
    }
    
    // 获取缓存的batch size
    int get_cached_batch_size() const { return cached_batch_size; }
};

// im2col实现（去除未使用的batch_size参数）
void ConvLayer::im2col(const float* input) {
    int K = input_channels * kernel_size * kernel_size;
    int N = output_height * output_width;
    
    int input_h_padded = input_height + 2 * padding;
    int input_w_padded = input_width + 2 * padding;
    
    // 创建填充后的输入
    std::vector<float> padded_input;
    const float* input_data = input;
    
    if (padding > 0) {
        padded_input.resize(input_channels * input_h_padded * input_w_padded, 0.0f);
        for (int c = 0; c < input_channels; c++) {
            for (int h = 0; h < input_height; h++) {
                for (int w = 0; w < input_width; w++) {
                    int padded_idx = c * input_h_padded * input_w_padded + 
                                     (h + padding) * input_w_padded + (w + padding);
                    int orig_idx = c * input_height * input_width + h * input_width + w;
                    padded_input[padded_idx] = input[orig_idx];
                }
            }
        }
        input_data = padded_input.data();
    }
    
    // 执行im2col
    // col_buffer shape: [K, N] = [input_channels * kernel_size * kernel_size, output_height * output_width]
    for (int c = 0; c < input_channels; c++) {
        for (int kh = 0; kh < kernel_size; kh++) {
            for (int kw = 0; kw < kernel_size; kw++) {
                int channel_kernel_idx = (c * kernel_size * kernel_size + kh * kernel_size + kw);
                for (int h = 0; h < output_height; h++) {
                    for (int w = 0; w < output_width; w++) {
                        int input_h = h * stride + kh;
                        int input_w = w * stride + kw;
                        int spatial_idx = h * output_width + w;
                        
                        // 计算col_buffer的线性索引: row = channel_kernel_idx, col = spatial_idx
                        int col_idx = channel_kernel_idx * N + spatial_idx;
                        
                        if (input_h >= 0 && input_h < input_h_padded && 
                            input_w >= 0 && input_w < input_w_padded) {
                            int input_idx = c * input_h_padded * input_w_padded + 
                                           input_h * input_w_padded + input_w;
                            col_buffer[col_idx] = input_data[input_idx];
                        } else {
                            col_buffer[col_idx] = 0.0f;
                        }
                    }
                }
            }
        }
    }
}

// col2im实现（去除未使用的batch_size参数）
void ConvLayer::col2im(const float* col, float* im) {
    int K = input_channels * kernel_size * kernel_size;
    int N = output_height * output_width;
    
    int input_h_padded = input_height + 2 * padding;
    int input_w_padded = input_width + 2 * padding;
    
    // 创建填充后的图像缓冲区（初始化为0）
    std::vector<float> padded_im(input_channels * input_h_padded * input_w_padded, 0.0f);
    
    // 执行col2im：将col矩阵的梯度累加回图像空间
    for (int c = 0; c < input_channels; c++) {
        for (int kh = 0; kh < kernel_size; kh++) {
            for (int kw = 0; kw < kernel_size; kw++) {
                int channel_kernel_idx = (c * kernel_size * kernel_size + kh * kernel_size + kw);
                for (int h = 0; h < output_height; h++) {
                    for (int w = 0; w < output_width; w++) {
                        int input_h = h * stride + kh;
                        int input_w = w * stride + kw;
                        int spatial_idx = h * output_width + w;
                        
                        int col_idx = channel_kernel_idx * N + spatial_idx;
                        
                        if (input_h >= 0 && input_h < input_h_padded && 
                            input_w >= 0 && input_w < input_w_padded) {
                            int padded_idx = c * input_h_padded * input_w_padded + 
                                           input_h * input_w_padded + input_w;
                            padded_im[padded_idx] += col[col_idx];
                        }
                    }
                }
            }
        }
    }
    
    // 去除padding
    if (padding > 0) {
        for (int c = 0; c < input_channels; c++) {
            for (int h = 0; h < input_height; h++) {
                for (int w = 0; w < input_width; w++) {
                    int padded_idx = c * input_h_padded * input_w_padded + 
                                     (h + padding) * input_w_padded + (w + padding);
                    int orig_idx = c * input_height * input_width + h * input_width + w;
                    im[orig_idx] = padded_im[padded_idx];
                }
            }
        }
    } else {
        std::copy(padded_im.begin(), padded_im.end(), im);
    }
}

// 矩阵乘法（简化版）
void ConvLayer::sgemm(int M, int N, int K, 
                      const float* A, const float* B, float* C,
                      bool transA, bool transB) {
    std::memset(C, 0, M * N * sizeof(float));
    sgemm_add(M, N, K, A, B, C, 1.0f, 0.0f, transA, transB);
}

// 矩阵乘法（完整版）
void ConvLayer::sgemm_add(int M, int N, int K,
                          const float* A, const float* B, float* C,
                          float alpha, float beta,
                          bool transA, bool transB) {
    if (beta == 0.0f) {
        std::memset(C, 0, M * N * sizeof(float));
    } else if (beta != 1.0f) {
        for (int i = 0; i < M * N; i++) {
            C[i] *= beta;
        }
    }
    
    const int BLOCK_SIZE = 8;
    
    if (transA && !transB) {
        // C = alpha * A^T * B + beta * C
        // A: [K, M], B: [K, N], C: [M, N]
        for (int i = 0; i < M; i++) {
            for (int k = 0; k < K; k++) {
                float a = A[k * M + i] * alpha;
                if (a == 0.0f) continue;
                
                const float* B_ptr = B + k * N;
                float* C_ptr = C + i * N;
                
                int j = 0;
                for (; j <= N - BLOCK_SIZE; j += BLOCK_SIZE) {
                    C_ptr[j] += a * B_ptr[j];
                    C_ptr[j+1] += a * B_ptr[j+1];
                    C_ptr[j+2] += a * B_ptr[j+2];
                    C_ptr[j+3] += a * B_ptr[j+3];
                    C_ptr[j+4] += a * B_ptr[j+4];
                    C_ptr[j+5] += a * B_ptr[j+5];
                    C_ptr[j+6] += a * B_ptr[j+6];
                    C_ptr[j+7] += a * B_ptr[j+7];
                }
                for (; j < N; j++) {
                    C_ptr[j] += a * B_ptr[j];
                }
            }
        }
    } else if (!transA && transB) {
        // C = alpha * A * B^T + beta * C
        // A: [M, K], B: [N, K], C: [M, N]
        for (int i = 0; i < M; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0.0f;
                for (int k = 0; k < K; k++) {
                    sum += A[i * K + k] * B[j * K + k];
                }
                C[i * N + j] += alpha * sum;
            }
        }
    } else if (transA && transB) {
        // C = alpha * A^T * B^T + beta * C
        // A: [K, M], B: [N, K], C: [M, N]
        for (int i = 0; i < M; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0.0f;
                for (int k = 0; k < K; k++) {
                    sum += A[k * M + i] * B[j * K + k];
                }
                C[i * N + j] += alpha * sum;
            }
        }
    } else {
        // C = alpha * A * B + beta * C
        // A: [M, K], B: [K, N], C: [M, N]
        for (int i = 0; i < M; i++) {
            for (int k = 0; k < K; k++) {
                float a = A[i * K + k] * alpha;
                if (a == 0.0f) continue;
                
                const float* B_ptr = B + k * N;
                float* C_ptr = C + i * N;
                
                int j = 0;
                for (; j <= N - BLOCK_SIZE; j += BLOCK_SIZE) {
                    C_ptr[j] += a * B_ptr[j];
                    C_ptr[j+1] += a * B_ptr[j+1];
                    C_ptr[j+2] += a * B_ptr[j+2];
                    C_ptr[j+3] += a * B_ptr[j+3];
                    C_ptr[j+4] += a * B_ptr[j+4];
                    C_ptr[j+5] += a * B_ptr[j+5];
                    C_ptr[j+6] += a * B_ptr[j+6];
                    C_ptr[j+7] += a * B_ptr[j+7];
                }
                for (; j < N; j++) {
                    C_ptr[j] += a * B_ptr[j];
                }
            }
        }
    }
}


class ImageLoader {
private:
    // 图像参数
    int target_channels;     // 目标通道数（如RGB=3, 灰度=1）
    int target_height;       // 目标高度
    int target_width;        // 目标宽度
    bool normalize_to_01;    // 是否归一化到[0,1]
    bool normalize_mean_std; // 是否使用均值标准差归一化
    float mean_val;          // 均值（归一化用）
    float std_val;           // 标准差（归一化用）
    bool auto_mean_std;      // 是否自动计算均值标准差
    
    // 预计算的均值和标准差（针对每个通道）
    std::vector<float> channel_means;
    std::vector<float> channel_stds;
    
    // 用于调试的verbose标志
    bool verbose;
    
    // 辅助函数：检查文件是否存在（POSIX兼容）
    bool file_exists(const std::string& filename) {
        struct stat buffer;
        return (stat(filename.c_str(), &buffer) == 0);
    }
    
    // 辅助函数：检查是否为普通文件
    bool is_regular_file(const std::string& path) {
        struct stat path_stat;
        if (stat(path.c_str(), &path_stat) != 0) {
            return false;
        }
        return S_ISREG(path_stat.st_mode);
    }
    
    // 辅助函数：获取文件扩展名
    std::string get_file_extension(const std::string& filename) {
        size_t dot_pos = filename.find_last_of(".");
        if (dot_pos == std::string::npos) {
            return "";
        }
        return filename.substr(dot_pos);
    }
    
    // 辅助函数：转换为小写
    std::string to_lower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
    
public:
    // 构造函数
    ImageLoader(int channels = 3, int height = 224, int width = 224, 
                bool norm_01 = true, bool norm_mean_std = false,
                float mean = 0.0f, float std = 1.0f, bool auto_compute = false)
        : target_channels(channels),
          target_height(height),
          target_width(width),
          normalize_to_01(norm_01),
          normalize_mean_std(norm_mean_std),
          mean_val(mean),
          std_val(std),
          auto_mean_std(auto_compute),
          verbose(false) {
        
        // 初始化通道统计量
        channel_means.resize(target_channels, 0.0f);
        channel_stds.resize(target_channels, 1.0f);
    }
    
    // 从文件加载单张图片
    std::vector<float> load_image(const std::string& filepath, bool verbose = false) {
        this->verbose = verbose;
        
        try {
            // 检查文件是否存在
            if (!file_exists(filepath)) {
                throw std::runtime_error("File not found: " + filepath);
            }
            
            // 加载图像
            cimg_library::CImg<unsigned char> img(filepath.c_str());
            
            if (verbose) {
                std::cout << "Loaded image: " << filepath << std::endl;
                std::cout << "Original size: " << img.width() << "x" << img.height() 
                          << "x" << img.spectrum() << std::endl;
            }
            
            // 转换为目标格式并预处理
            return preprocess_image(img, verbose);
            
        } catch (const std::exception& e) {
            std::cerr << "Error loading image " << filepath << ": " << e.what() << std::endl;
            return {};
        }
    }
    
    // 从CImg对象加载图片
    std::vector<float> load_from_cimg(const cimg_library::CImg<unsigned char>& input_img, bool verbose = false) {
        this->verbose = verbose;
        
        try {
            // 创建图像的副本（因为预处理函数可能会修改图像）
            cimg_library::CImg<unsigned char> img = input_img;
            
            if (verbose) {
                std::cout << "Loading from CImg object" << std::endl;
                std::cout << "Original size: " << img.width() << "x" << img.height() 
                          << "x" << img.spectrum() << std::endl;
            }
            
            // 检查图像是否为空
            if (img.is_empty()) {
                throw std::runtime_error("Input CImg object is empty");
            }
            
            // 转换为目标格式并预处理
            return preprocess_image(img, verbose);
            
        } catch (const std::exception& e) {
            std::cerr << "Error loading image from CImg object: " << e.what() << std::endl;
            return {};
        }
    }
    
    // 从CImg对象加载图片（移动语义版本）
    std::vector<float> load_from_cimg(cimg_library::CImg<unsigned char>&& input_img, bool verbose = false) {
        this->verbose = verbose;
        
        try {
            // 直接使用移动过来的图像对象
            cimg_library::CImg<unsigned char> img = std::move(input_img);
            
            if (verbose) {
                std::cout << "Loading from CImg object (move semantics)" << std::endl;
                std::cout << "Original size: " << img.width() << "x" << img.height() 
                          << "x" << img.spectrum() << std::endl;
            }
            
            // 检查图像是否为空
            if (img.is_empty()) {
                throw std::runtime_error("Input CImg object is empty");
            }
            
            // 转换为目标格式并预处理
            return preprocess_image(img, verbose);
            
        } catch (const std::exception& e) {
            std::cerr << "Error loading image from CImg object: " << e.what() << std::endl;
            return {};
        }
    }
    
    // 批量加载图片
    std::vector<float> load_batch(const std::vector<std::string>& filepaths, bool verbose = false) {
        this->verbose = verbose;
        
        std::vector<float> batch_data;
        int loaded_count = 0;
        
        for (const auto& filepath : filepaths) {
            auto img_data = load_image(filepath, verbose);
            if (!img_data.empty()) {
                if (batch_data.empty()) {
                    batch_data = img_data;
                } else {
                    // 追加到batch数据中
                    batch_data.insert(batch_data.end(), img_data.begin(), img_data.end());
                }
                loaded_count++;
            }
        }
        
        if (verbose) {
            std::cout << "Successfully loaded " << loaded_count << " images out of " 
                      << filepaths.size() << std::endl;
        }
        
        return batch_data;
    }
    
    
    // 从文件夹加载所有图片
    std::vector<float> load_from_directory(const std::string& dir_path, 
                                           const std::vector<std::string>& extensions = {".jpg", ".jpeg", ".png", ".bmp"},
                                           bool verbose = false) {
        this->verbose = verbose;
        
        std::vector<std::string> image_files;
        
        try {
            // 打开目录
            DIR* dir = opendir(dir_path.c_str());
            if (!dir) {
                throw std::runtime_error("Cannot open directory: " + dir_path);
            }
            
            // 读取目录项
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string filename = entry->d_name;
                
                // 跳过"."和".."
                if (filename == "." || filename == "..") {
                    continue;
                }
                
                // 构建完整路径
                std::string full_path = dir_path;
                if (dir_path.back() != '/') {
                    full_path += "/";
                }
                full_path += filename;
                
                // 检查是否为普通文件
                if (is_regular_file(full_path)) {
                    // 检查文件扩展名
                    std::string ext = get_file_extension(filename);
                    std::string ext_lower = to_lower(ext);
                    
                    // 检查是否在允许的扩展名列表中
                    bool valid_extension = false;
                    for (const auto& allowed_ext : extensions) {
                        if (ext_lower == to_lower(allowed_ext)) {
                            valid_extension = true;
                            break;
                        }
                    }
                    
                    if (valid_extension) {
                        image_files.push_back(full_path);
                    }
                }
            }
            
            closedir(dir);
            
            if (verbose) {
                std::cout << "Found " << image_files.size() << " images in directory: " 
                          << dir_path << std::endl;
            }
            
            return load_batch(image_files, verbose);
            
        } catch (const std::exception& e) {
            std::cerr << "Error reading directory " << dir_path << ": " << e.what() << std::endl;
            return {};
        }
    }
    
    // 计算数据集统计量（用于归一化）
    void compute_dataset_statistics(const std::vector<std::string>& filepaths, 
                                    int max_images = 1000) {
        std::cout << "Computing dataset statistics..." << std::endl;
        
        int num_images = std::min(static_cast<int>(filepaths.size()), max_images);
        std::vector<std::vector<float>> channel_sums(target_channels, std::vector<float>());
        
        // 采样计算
        for (int i = 0; i < num_images; i++) {
            auto img_data = load_image(filepaths[i], false);
            if (!img_data.empty()) {
                // 计算每个通道的像素值
                int pixels_per_channel = target_height * target_width;
                for (int c = 0; c < target_channels; c++) {
                    const float* channel_start = img_data.data() + c * pixels_per_channel;
                    channel_sums[c].insert(channel_sums[c].end(), 
                                           channel_start, 
                                           channel_start + pixels_per_channel);
                }
            }
            
            if (i % 100 == 0) {
                std::cout << "Processed " << i << " images..." << std::endl;
            }
        }
        
        // 计算均值和标准差
        for (int c = 0; c < target_channels; c++) {
            if (!channel_sums[c].empty()) {
                // 计算均值
                float sum = std::accumulate(channel_sums[c].begin(), channel_sums[c].end(), 0.0f);
                channel_means[c] = sum / channel_sums[c].size();
                
                // 计算标准差
                float variance = 0.0f;
                for (float val : channel_sums[c]) {
                    variance += (val - channel_means[c]) * (val - channel_means[c]);
                }
                variance /= channel_sums[c].size();
                channel_stds[c] = std::sqrt(variance);
                
                std::cout << "Channel " << c << ": mean = " << channel_means[c] 
                          << ", std = " << channel_stds[c] << std::endl;
            }
        }
        
        std::cout << "Statistics computation completed." << std::endl;
    }
    
    // 获取批次数（根据总像素数计算）
    int get_batch_size(const std::vector<float>& data) const {
        int single_image_size = target_channels * target_height * target_width;
        if (single_image_size == 0) return 0;
        return data.size() / single_image_size;
    }
    
    // 设置归一化参数
    void set_normalization_params(const std::vector<float>& means, 
                                  const std::vector<float>& stds) {
        if (means.size() == target_channels && stds.size() == target_channels) {
            channel_means = means;
            channel_stds = stds;
        } else {
            std::cerr << "Error: Means and stds must have size " << target_channels << std::endl;
        }
    }
    
    // 获取归一化参数
    std::pair<std::vector<float>, std::vector<float>> get_normalization_params() const {
        return {channel_means, channel_stds};
    }
    
    // 显示图像信息（用于调试）
    void display_image_info(const std::vector<float>& image_data, int channels_to_show = 3) const {
        int single_image_size = target_channels * target_height * target_width;
        
        std::cout << "Image data info:" << std::endl;
        std::cout << "  Dimensions: " << target_channels << " x " << target_height 
                  << " x " << target_width << std::endl;
        std::cout << "  Total values: " << image_data.size() << std::endl;
        
        if (!image_data.empty()) {
            // 计算统计信息
            float min_val = *std::min_element(image_data.begin(), image_data.end());
            float max_val = *std::max_element(image_data.begin(), image_data.end());
            float sum_val = std::accumulate(image_data.begin(), image_data.end(), 0.0f);
            float mean_val = sum_val / image_data.size();
            
            std::cout << "  Min value: " << min_val << std::endl;
            std::cout << "  Max value: " << max_val << std::endl;
            std::cout << "  Mean value: " << mean_val << std::endl;
            
            // 显示每个通道的前几个像素
            int pixels_per_channel = target_height * target_width;
            for (int c = 0; c < std::min(target_channels, channels_to_show); c++) {
                std::cout << "  Channel " << c << " (first 5 pixels): ";
                for (int i = 0; i < std::min(5, pixels_per_channel); i++) {
                    std::cout << image_data[c * pixels_per_channel + i] << " ";
                }
                std::cout << std::endl;
            }
        }
    }
    
    // 获取目标尺寸（公开访问器）
    int get_target_height() const { return target_height; }
    int get_target_width() const { return target_width; }
    int get_target_channels() const { return target_channels; }
    
    // 设置verbose模式
    void set_verbose(bool v) { verbose = v; }
    
private:
    // 图像预处理主函数
    std::vector<float> preprocess_image(cimg_library::CImg<unsigned char>& img, bool verbose = false) {
        // 1. 调整通道数
        adjust_channels(img);
        
        // 2. 调整尺寸
        resize_image(img);
        
        // 3. 转换为浮点数
        std::vector<float> float_data = convert_to_float(img);
        
        // 4. 重新排列维度为 [channels, height, width]
        std::vector<float> reordered_data = reorder_dimensions(float_data, img);
        
        // 5. 归一化
        if (normalize_to_01) {
            normalize_0_1(reordered_data);
        }
        
        if (normalize_mean_std) {
            normalize_mean_std_channels(reordered_data);
        }
        
        if (verbose) {
            std::cout << "Processed image size: " << target_channels << "x" 
                      << target_height << "x" << target_width << std::endl;
        }
        
        return reordered_data;
    }
    
    // 调整通道数
    void adjust_channels(cimg_library::CImg<unsigned char>& img) {
        int current_channels = img.spectrum();
        
        if (current_channels == 1 && target_channels == 3) {
            // 灰度转RGB：复制单通道到三个通道
            cimg_library::CImg<unsigned char> rgb_img(img.width(), img.height(), img.depth(), 3);
            cimg_forXYZ(img, x, y, z) {
                rgb_img(x, y, z, 0) = img(x, y, z, 0);  // R
                rgb_img(x, y, z, 1) = img(x, y, z, 0);  // G
                rgb_img(x, y, z, 2) = img(x, y, z, 0);  // B
            }
            img = rgb_img;
            
        } else if (current_channels == 3 && target_channels == 1) {
            // RGB转灰度：使用标准公式
            img = img.get_channel(0) * 0.299f + 
                  img.get_channel(1) * 0.587f + 
                  img.get_channel(2) * 0.114f;
                  
        } else if (current_channels == 4 && target_channels == 3) {
            // RGBA转RGB：移除alpha通道
            img.channels(0, 2);
            
        } else if (current_channels > target_channels) {
            // 保留前target_channels个通道
            img.channels(0, target_channels - 1);
            
        } else if (current_channels < target_channels) {
            // 重复最后一个通道来补足
            int channels_to_add = target_channels - current_channels;
            for (int i = 0; i < channels_to_add; i++) {
                img.append(img.get_channel(current_channels - 1), 'c');
            }
        }
    }
    
    // 调整图像尺寸
    void resize_image(cimg_library::CImg<unsigned char>& img) {
        if (img.width() != target_width || img.height() != target_height) {
            if (verbose) {
                std::cout << "Resizing from " << img.width() << "x" << img.height() 
                          << " to " << target_width << "x" << target_height << std::endl;
            }
            
            // 使用CImg的resize函数
            img.resize(target_width, target_height, -100, -100, 3); // 3 = 三次样条插值
        }
    }
    
    // 转换为浮点数
    std::vector<float> convert_to_float(const cimg_library::CImg<unsigned char>& img) {
        std::vector<float> float_data;
        float_data.reserve(img.width() * img.height() * img.spectrum());
        
        cimg_forC(img, c) {
            cimg_forXY(img, x, y) {
                float_data.push_back(static_cast<float>(img(x, y, 0, c)));
            }
        }
        
        return float_data;
    }
    
    // 重新排列维度为 [channels, height, width]
    std::vector<float> reorder_dimensions(const std::vector<float>& data, 
                                          const cimg_library::CImg<unsigned char>& img) {
        int channels = img.spectrum();
        int height = img.height();
        int width = img.width();
        
        std::vector<float> reordered(channels * height * width);
        
        // CImg默认布局: [width, height, depth, spectrum]
        // 我们需要: [channels, height, width]
        for (int c = 0; c < channels; c++) {
            for (int h = 0; h < height; h++) {
                for (int w = 0; w < width; w++) {
                    // CImg索引: (x, y, z, c) = (w, h, 0, c)
                    int src_idx = c * (height * width) + h * width + w;
                    int dst_idx = c * (height * width) + h * width + w;
                    reordered[dst_idx] = data[src_idx];
                }
            }
        }
        
        return reordered;
    }
    
    // 归一化到[0,1]
    void normalize_0_1(std::vector<float>& data) {
        for (auto& val : data) {
            val /= 255.0f;
        }
    }
    
    // 使用均值标准差归一化（逐通道）
    void normalize_mean_std_channels(std::vector<float>& data) {
        int pixels_per_channel = target_height * target_width;
        
        for (int c = 0; c < target_channels; c++) {
            float mean = auto_mean_std ? channel_means[c] : mean_val;
            float std = auto_mean_std ? channel_stds[c] : std_val;
            
            for (int i = 0; i < pixels_per_channel; i++) {
                int idx = c * pixels_per_channel + i;
                data[idx] = (data[idx] - mean) / std;
            }
        }
    }
};


class PoolingLayer {
private:
    // 池化类型枚举
    enum class PoolType {
        MAX,
        AVG
    };
    
    // 参数
    int pool_height;
    int pool_width;
    int stride;
    int padding;
    PoolType pool_type;
    
    // 输入输出尺寸
    int input_channels;
    int input_height;
    int input_width;
    int output_height;
    int output_width;
    
    // 中间变量缓存（用于反向传播）
    std::vector<int> max_idx_cache;  // 最大池化时缓存最大值位置
    std::vector<float> input_cache;  // 缓存的输入
    std::vector<float> output_cache; // 缓存的输出
    
    // 辅助函数
    //void max_pool_forward(const float* input, float* output, int batch_size);
    //void avg_pool_forward(const float* input, float* output, int batch_size);
    //void max_pool_backward(const float* grad_output, float* grad_input, int batch_size);
    //void avg_pool_backward(const float* grad_output, float* grad_input, int batch_size);
    
public:
    // 构造函数
    PoolingLayer(int pool_size, int stride = 2, int padding = 0, bool max_pool = true)
        : pool_height(pool_size),
          pool_width(pool_size),
          stride(stride),
          padding(padding),
          pool_type(max_pool ? PoolType::MAX : PoolType::AVG) {}
    
    // 带不同高度和宽度的构造函数
    PoolingLayer(int pool_height, int pool_width, int stride = 2, int padding = 0, bool max_pool = true)
        : pool_height(pool_height),
          pool_width(pool_width),
          stride(stride),
          padding(padding),
          pool_type(max_pool ? PoolType::MAX : PoolType::AVG) {}
    
    // 设置输入尺寸
    void set_input_shape(int channels, int height, int width) {
        input_channels = channels;
        input_height = height;
        input_width = width;
        
        // 计算输出尺寸
        output_height = (height + 2 * padding - pool_height) / stride + 1;
        output_width = (width + 2 * padding - pool_width) / stride + 1;
        
        // 清空缓存
        max_idx_cache.clear();
        input_cache.clear();
        output_cache.clear();
    }
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true) {
        // 计算输出大小
        int output_size = batch_size * input_channels * output_height * output_width;
        std::vector<float> output(output_size, 0.0f);
        
        // 如果训练模式，缓存输入
        if (training) {
            input_cache = input;
        }
        
        // 根据池化类型选择前向传播函数
        if (pool_type == PoolType::MAX) {
            max_pool_forward(input.data(), output.data(), batch_size);
        } else {
            avg_pool_forward(input.data(), output.data(), batch_size);
        }
        
        // 如果训练模式，缓存输出
        if (training) {
            output_cache = output;
        }
        
        return output;
    }
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1) {
        // 创建输入梯度
        std::vector<float> grad_input(batch_size * input_channels * input_height * input_width, 0.0f);
        
        // 根据池化类型选择反向传播函数
        if (pool_type == PoolType::MAX) {
            max_pool_backward(grad_output.data(), grad_input.data(), batch_size);
        } else {
            avg_pool_backward(grad_output.data(), grad_input.data(), batch_size);
        }
        
        return grad_input;
    }
    
    // 获取输出尺寸
    std::tuple<int, int, int> get_output_shape() const {
        return std::make_tuple(input_channels, output_height, output_width);
    }
    
    // 获取参数信息
    void print_info() const {
        std::cout << "Pooling Layer Info:" << std::endl;
        std::cout << "  Type: " << (pool_type == PoolType::MAX ? "Max Pooling" : "Average Pooling") << std::endl;
        std::cout << "  Pool Size: " << pool_height << "x" << pool_width << std::endl;
        std::cout << "  Stride: " << stride << std::endl;
        std::cout << "  Padding: " << padding << std::endl;
        std::cout << "  Input Shape: " << input_channels << "x" << input_height << "x" << input_width << std::endl;
        std::cout << "  Output Shape: " << input_channels << "x" << output_height << "x" << output_width << std::endl;
    }
    
private:
    // 最大池化前向传播实现
    void max_pool_forward(const float* input, float* output, int batch_size) {
        // 计算填充后的尺寸
        int padded_height = input_height + 2 * padding;
        int padded_width = input_width + 2 * padding;
        
        // 清空最大值位置缓存
        max_idx_cache.clear();
        max_idx_cache.resize(batch_size * input_channels * output_height * output_width, -1);
        
        // 对每个批次、每个通道进行处理
        for (int b = 0; b < batch_size; b++) {
            for (int c = 0; c < input_channels; c++) {
                // 当前通道的输入和输出指针
                const float* input_ptr = input + b * input_channels * input_height * input_width + 
                                         c * input_height * input_width;
                float* output_ptr = output + b * input_channels * output_height * output_width + 
                                    c * output_height * output_width;
                int* max_idx_ptr = max_idx_cache.data() + b * input_channels * output_height * output_width + 
                                   c * output_height * output_width;
                
                // 对每个输出位置进行计算
                for (int oh = 0; oh < output_height; oh++) {
                    for (int ow = 0; ow < output_width; ow++) {
                        // 计算池化窗口的起始位置
                        int start_h = oh * stride - padding;
                        int start_w = ow * stride - padding;
                        
                        // 计算池化窗口的结束位置
                        int end_h = std::min(start_h + pool_height, padded_height);
                        int end_w = std::min(start_w + pool_width, padded_width);
                        
                        // 调整起始位置，确保不超出边界
                        start_h = std::max(start_h, 0);
                        start_w = std::max(start_w, 0);
                        
                        // 在池化窗口内寻找最大值
                        float max_val = -std::numeric_limits<float>::max();
                        int max_idx = -1;
                        
                        for (int ph = start_h; ph < end_h; ph++) {
                            for (int pw = start_w; pw < end_w; pw++) {
                                // 计算原始输入中的索引（考虑填充）
                                int orig_ph = ph - padding;
                                int orig_pw = pw - padding;
                                
                                // 检查是否在有效范围内
                                if (orig_ph >= 0 && orig_ph < input_height && 
                                    orig_pw >= 0 && orig_pw < input_width) {
                                    int idx = orig_ph * input_width + orig_pw;
                                    float val = input_ptr[idx];
                                    
                                    if (val > max_val) {
                                        max_val = val;
                                        max_idx = idx;
                                    }
                                }
                            }
                        }
                        
                        // 存储输出和最大值位置
                        output_ptr[oh * output_width + ow] = max_val;
                        max_idx_ptr[oh * output_width + ow] = max_idx;
                    }
                }
            }
        }
    }
    
    // 平均池化前向传播实现
    void avg_pool_forward(const float* input, float* output, int batch_size) {
        // 计算填充后的尺寸
        int padded_height = input_height + 2 * padding;
        int padded_width = input_width + 2 * padding;
        
        // 对每个批次、每个通道进行处理
        for (int b = 0; b < batch_size; b++) {
            for (int c = 0; c < input_channels; c++) {
                // 当前通道的输入和输出指针
                const float* input_ptr = input + b * input_channels * input_height * input_width + 
                                         c * input_height * input_width;
                float* output_ptr = output + b * input_channels * output_height * output_width + 
                                    c * output_height * output_width;
                
                // 对每个输出位置进行计算
                for (int oh = 0; oh < output_height; oh++) {
                    for (int ow = 0; ow < output_width; ow++) {
                        // 计算池化窗口的起始位置
                        int start_h = oh * stride - padding;
                        int start_w = ow * stride - padding;
                        
                        // 计算池化窗口的结束位置
                        int end_h = std::min(start_h + pool_height, padded_height);
                        int end_w = std::min(start_w + pool_width, padded_width);
                        
                        // 调整起始位置，确保不超出边界
                        start_h = std::max(start_h, 0);
                        start_w = std::max(start_w, 0);
                        
                        // 在池化窗口内计算平均值
                        float sum = 0.0f;
                        int count = 0;
                        
                        for (int ph = start_h; ph < end_h; ph++) {
                            for (int pw = start_w; pw < end_w; pw++) {
                                // 计算原始输入中的索引（考虑填充）
                                int orig_ph = ph - padding;
                                int orig_pw = pw - padding;
                                
                                // 检查是否在有效范围内
                                if (orig_ph >= 0 && orig_ph < input_height && 
                                    orig_pw >= 0 && orig_pw < input_width) {
                                    int idx = orig_ph * input_width + orig_pw;
                                    sum += input_ptr[idx];
                                    count++;
                                }
                            }
                        }
                        
                        // 计算平均值
                        output_ptr[oh * output_width + ow] = (count > 0) ? sum / count : 0.0f;
                    }
                }
            }
        }
    }
    
    // 最大池化反向传播实现
    void max_pool_backward(const float* grad_output, float* grad_input, int batch_size) {
        // 初始化输入梯度为0
        std::memset(grad_input, 0, batch_size * input_channels * input_height * input_width * sizeof(float));
        
        // 对每个批次、每个通道进行处理
        for (int b = 0; b < batch_size; b++) {
            for (int c = 0; c < input_channels; c++) {
                // 当前通道的梯度指针
                const float* grad_output_ptr = grad_output + b * input_channels * output_height * output_width + 
                                               c * output_height * output_width;
                float* grad_input_ptr = grad_input + b * input_channels * input_height * input_width + 
                                        c * input_height * input_width;
                const int* max_idx_ptr = max_idx_cache.data() + b * input_channels * output_height * output_width + 
                                         c * output_height * output_width;
                
                // 对每个输出位置进行反向传播
                for (int oh = 0; oh < output_height; oh++) {
                    for (int ow = 0; ow < output_width; ow++) {
                        // 获取最大值位置索引
                        int max_idx = max_idx_ptr[oh * output_width + ow];
                        
                        // 如果最大值位置有效，将梯度传递到该位置
                        if (max_idx >= 0) {
                            grad_input_ptr[max_idx] += grad_output_ptr[oh * output_width + ow];
                        }
                    }
                }
            }
        }
    }
    
    // 平均池化反向传播实现
    void avg_pool_backward(const float* grad_output, float* grad_input, int batch_size) {
        // 初始化输入梯度为0
        std::memset(grad_input, 0, batch_size * input_channels * input_height * input_width * sizeof(float));
        
        // 计算填充后的尺寸
        int padded_height = input_height + 2 * padding;
        int padded_width = input_width + 2 * padding;
        
        // 对每个批次、每个通道进行处理
        for (int b = 0; b < batch_size; b++) {
            for (int c = 0; c < input_channels; c++) {
                // 当前通道的梯度指针
                const float* grad_output_ptr = grad_output + b * input_channels * output_height * output_width + 
                                               c * output_height * output_width;
                float* grad_input_ptr = grad_input + b * input_channels * input_height * input_width + 
                                        c * input_height * input_width;
                
                // 对每个输出位置进行反向传播
                for (int oh = 0; oh < output_height; oh++) {
                    for (int ow = 0; ow < output_width; ow++) {
                        // 计算池化窗口的起始位置
                        int start_h = oh * stride - padding;
                        int start_w = ow * stride - padding;
                        
                        // 计算池化窗口的结束位置
                        int end_h = std::min(start_h + pool_height, padded_height);
                        int end_w = std::min(start_w + pool_width, padded_width);
                        
                        // 调整起始位置，确保不超出边界
                        start_h = std::max(start_h, 0);
                        start_w = std::max(start_w, 0);
                        
                        // 计算有效池化窗口的大小
                        int count = 0;
                        for (int ph = start_h; ph < end_h; ph++) {
                            for (int pw = start_w; pw < end_w; pw++) {
                                // 计算原始输入中的索引（考虑填充）
                                int orig_ph = ph - padding;
                                int orig_pw = pw - padding;
                                
                                // 检查是否在有效范围内
                                if (orig_ph >= 0 && orig_ph < input_height && 
                                    orig_pw >= 0 && orig_pw < input_width) {
                                    count++;
                                }
                            }
                        }
                        
                        // 如果池化窗口内有有效元素，计算平均梯度
                        if (count > 0) {
                            float avg_grad = grad_output_ptr[oh * output_width + ow] / count;
                            
                            // 将平均梯度分配到池化窗口内的每个位置
                            for (int ph = start_h; ph < end_h; ph++) {
                                for (int pw = start_w; pw < end_w; pw++) {
                                    // 计算原始输入中的索引（考虑填充）
                                    int orig_ph = ph - padding;
                                    int orig_pw = pw - padding;
                                    
                                    // 检查是否在有效范围内
                                    if (orig_ph >= 0 && orig_ph < input_height && 
                                        orig_pw >= 0 && orig_pw < input_width) {
                                        int idx = orig_ph * input_width + orig_pw;
                                        grad_input_ptr[idx] += avg_grad;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
};

// 全局平均池化层（常用于CNN分类网络的最后）
class GlobalAveragePooling {
private:
    int input_channels;
    int input_height;
    int input_width;
    std::vector<float> input_cache;
    
public:
    // 设置输入尺寸
    void set_input_shape(int channels, int height, int width) {
        input_channels = channels;
        input_height = height;
        input_width = width;
    }
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true) {
        // 缓存输入（如果需要）
        if (training) {
            input_cache = input;
        }
        
        // 计算输出大小：每个通道一个值
        int output_size = batch_size * input_channels;
        std::vector<float> output(output_size, 0.0f);
        
        // 对每个批次、每个通道计算平均值
        int spatial_size = input_height * input_width;
        for (int b = 0; b < batch_size; b++) {
            for (int c = 0; c < input_channels; c++) {
                const float* input_ptr = input.data() + b * input_channels * spatial_size + c * spatial_size;
                float sum = 0.0f;
                
                // 计算该通道所有空间位置的平均值
                for (int i = 0; i < spatial_size; i++) {
                    sum += input_ptr[i];
                }
                
                output[b * input_channels + c] = sum / spatial_size;
            }
        }
        
        return output;
    }
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1) {
        // 创建输入梯度
        int spatial_size = input_height * input_width;
        std::vector<float> grad_input(batch_size * input_channels * spatial_size, 0.0f);
        
        // 对每个批次、每个通道分配梯度
        for (int b = 0; b < batch_size; b++) {
            for (int c = 0; c < input_channels; c++) {
                float grad = grad_output[b * input_channels + c] / spatial_size;
                float* grad_input_ptr = grad_input.data() + b * input_channels * spatial_size + c * spatial_size;
                
                // 将梯度均匀分配到所有空间位置
                for (int i = 0; i < spatial_size; i++) {
                    grad_input_ptr[i] = grad;
                }
            }
        }
        
        return grad_input;
    }
    
    // 获取输出尺寸
    std::tuple<int, int> get_output_shape() const {
        return std::make_tuple(input_channels, 1);
    }
};

/*

// 创建池化层
PoolingLayer pool(2, 2, 0, true);  // 2x2最大池化，步长2，无填充
pool.set_input_shape(64, 28, 28);  // 64通道，28x28输入

// 前向传播
auto output = pool.forward(input, batch_size, true);

// 反向传播
auto grad_input = pool.backward(grad_output, batch_size);

*/


class FlattenLayer {
private:
    // 输入输出尺寸
    int batch_size;
    int input_channels;
    int input_height;
    int input_width;
    int output_size;
    
    // 缓存输入形状（用于反向传播）
    std::vector<int> input_shape_cache;
    std::vector<float> input_cache;  // 缓存输入数据（可选）
    
    // 辅助函数
    //void flatten_forward(const float* input, float* output);
    //void flatten_backward(const float* grad_output, float* grad_input);
    
public:
    // 构造函数
    FlattenLayer() : batch_size(0), input_channels(0), 
                     input_height(0), input_width(0), output_size(0) {}
    
    // 设置输入尺寸（自动计算输出尺寸）
    void set_input_shape(int channels, int height, int width, int batch = 1) {
        input_channels = channels;
        input_height = height;
        input_width = width;
        batch_size = batch;
        
        // 计算输出大小
        output_size = channels * height * width;
        
        // 缓存输入形状
        input_shape_cache = {channels, height, width, batch};
    }
    
    // 设置输入尺寸（从卷积/池化层获取）
    template<typename... Args>
    void set_input_shape(std::tuple<Args...> shape_tuple) {
        set_input_shape(std::get<0>(shape_tuple), 
                       std::get<1>(shape_tuple), 
                       std::get<2>(shape_tuple));
    }
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true) {
        // 如果之前没有设置输入尺寸，自动推断
        if (input_channels == 0 && input_height == 0 && input_width == 0) {
            // 这里假设输入是完整的batch数据，需要外部调用set_input_shape
            // 或者根据输入大小推断（但不知道高度和宽度）
            std::cerr << "Warning: Input shape not set for FlattenLayer. "
                      << "Please call set_input_shape() first." << std::endl;
            return input;  // 安全返回，但可能不正确
        }
        
        // 检查输入大小是否匹配
        int expected_input_size = batch_size * input_channels * input_height * input_width;
        if (input.size() != static_cast<size_t>(expected_input_size)) {
            std::cerr << "Error: Input size mismatch in FlattenLayer. "
                      << "Expected: " << expected_input_size 
                      << ", Got: " << input.size() << std::endl;
            return std::vector<float>();
        }
        
        // 如果训练模式，缓存输入形状
        if (training) {
            this->batch_size = batch_size;
            input_shape_cache = {input_channels, input_height, input_width, batch_size};
            input_cache = input;  // 可选：缓存输入数据
        }
        
        // 计算输出大小
        int output_total_size = batch_size * output_size;
        std::vector<float> output(output_total_size, 0.0f);
        
        // 执行展平操作
        flatten_forward(input.data(), output.data());
        
        return output;
    }
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1) {
        // 检查梯度大小是否匹配
        int expected_grad_size = batch_size * output_size;
        if (grad_output.size() != static_cast<size_t>(expected_grad_size)) {
            std::cerr << "Error: Gradient size mismatch in FlattenLayer. "
                      << "Expected: " << expected_grad_size 
                      << ", Got: " << grad_output.size() << std::endl;
            return std::vector<float>();
        }
        
        // 恢复输入形状
        if (input_shape_cache.size() >= 4) {
            input_channels = input_shape_cache[0];
            input_height = input_shape_cache[1];
            input_width = input_shape_cache[2];
            this->batch_size = input_shape_cache[3];
        }
        
        // 计算输入梯度大小
        int input_total_size = this->batch_size * input_channels * input_height * input_width;
        std::vector<float> grad_input(input_total_size, 0.0f);
        
        // 执行反向展平操作
        flatten_backward(grad_output.data(), grad_input.data());
        
        return grad_input;
    }
    
    // 获取输出尺寸
    int get_output_size() const {
        return output_size;
    }
    
    // 获取输出尺寸（返回元组）
    std::tuple<int> get_output_shape() const {
        return std::make_tuple(output_size);
    }
    
    // 获取输入尺寸
    std::tuple<int, int, int> get_input_shape() const {
        return std::make_tuple(input_channels, input_height, input_width);
    }
    
    // 获取完整输入尺寸（包含批次）
    std::tuple<int, int, int, int> get_full_input_shape() const {
        return std::make_tuple(input_channels, input_height, input_width, batch_size);
    }
    
    // 打印层信息
    void print_info() const {
        std::cout << "Flatten Layer Info:" << std::endl;
        std::cout << "  Input Shape: " << input_channels << "x" 
                  << input_height << "x" << input_width;
        if (batch_size > 0) {
            std::cout << " (batch: " << batch_size << ")";
        }
        std::cout << std::endl;
        std::cout << "  Output Size: " << output_size;
        if (batch_size > 0) {
            std::cout << " (total: " << batch_size * output_size << ")";
        }
        std::cout << std::endl;
    }
    
    // 重置层状态
    void reset() {
        input_shape_cache.clear();
        input_cache.clear();
        batch_size = 0;
        input_channels = 0;
        input_height = 0;
        input_width = 0;
        output_size = 0;
    }
    
    // 检查是否已初始化
    bool is_initialized() const {
        return input_channels > 0 && input_height > 0 && input_width > 0;
    }
    
private:
    // 展平前向传播实现
    void flatten_forward(const float* input, float* output) {
        // 简单地将多维数据复制到一维数组
        int elements_per_sample = input_channels * input_height * input_width;
        
        for (int b = 0; b < batch_size; b++) {
            const float* input_ptr = input + b * elements_per_sample;
            float* output_ptr = output + b * output_size;
            
            // 直接内存复制
            std::copy(input_ptr, input_ptr + elements_per_sample, output_ptr);
            
            // 或者使用循环（等效）
            /*
            for (int i = 0; i < elements_per_sample; i++) {
                output_ptr[i] = input_ptr[i];
            }
            */
        }
    }
    
    // 展平反向传播实现
    void flatten_backward(const float* grad_output, float* grad_input) {
        // 反向传播只是将梯度重新整形为输入形状
        int elements_per_sample = input_channels * input_height * input_width;
        
        for (int b = 0; b < batch_size; b++) {
            const float* grad_output_ptr = grad_output + b * output_size;
            float* grad_input_ptr = grad_input + b * elements_per_sample;
            
            // 直接内存复制
            std::copy(grad_output_ptr, grad_output_ptr + elements_per_sample, grad_input_ptr);
            
            // 或者使用循环（等效）
            /*
            for (int i = 0; i < elements_per_sample; i++) {
                grad_input_ptr[i] = grad_output_ptr[i];
            }
            */
        }
    }
};

// 动态展平层（自动推断输入形状）
class DynamicFlattenLayer {
private:
    int batch_size;
    int input_channels;
    int input_height;
    int input_width;
    int output_size;
    
public:
    DynamicFlattenLayer() : batch_size(0), input_channels(0), 
                           input_height(0), input_width(0), output_size(0) {}
    
    // 前向传播（自动推断形状）
    std::vector<float> forward(const std::vector<float>& input, 
                               std::tuple<int, int, int> input_shape,
                               int batch_size = 1, 
                               bool training = true) {
        // 从元组获取输入形状
        input_channels = std::get<0>(input_shape);
        input_height = std::get<1>(input_shape);
        input_width = std::get<2>(input_shape);
        this->batch_size = batch_size;
        
        // 验证输入大小
        int expected_size = batch_size * input_channels * input_height * input_width;
        if (input.size() != static_cast<size_t>(expected_size)) {
            std::cerr << "Error: Input size mismatch in DynamicFlattenLayer. "
                      << "Expected: " << expected_size 
                      << ", Got: " << input.size() 
                      << " (shape: " << input_channels << "x" 
                      << input_height << "x" << input_width 
                      << ", batch: " << batch_size << ")" << std::endl;
            return std::vector<float>();
        }
        
        // 计算输出大小
        output_size = input_channels * input_height * input_width;
        int total_output_size = batch_size * output_size;
        std::vector<float> output(total_output_size);
        
        // 执行展平
        const float* input_ptr = input.data();
        float* output_ptr = output.data();
        
        int elements_per_sample = output_size;
        for (int b = 0; b < batch_size; b++) {
            std::copy(input_ptr, input_ptr + elements_per_sample, output_ptr);
            input_ptr += elements_per_sample;
            output_ptr += elements_per_sample;
        }
        
        return output;
    }
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1) {
        // 检查是否已初始化
        if (output_size == 0) {
            std::cerr << "Error: DynamicFlattenLayer not initialized. "
                      << "Call forward() first." << std::endl;
            return std::vector<float>();
        }
        
        // 验证梯度大小
        int expected_grad_size = batch_size * output_size;
        if (grad_output.size() != static_cast<size_t>(expected_grad_size)) {
            std::cerr << "Error: Gradient size mismatch in DynamicFlattenLayer. "
                      << "Expected: " << expected_grad_size 
                      << ", Got: " << grad_output.size() << std::endl;
            return std::vector<float>();
        }
        
        // 恢复输入形状
        int total_input_size = batch_size * input_channels * input_height * input_width;
        std::vector<float> grad_input(total_input_size);
        
        // 执行反向展平
        const float* grad_output_ptr = grad_output.data();
        float* grad_input_ptr = grad_input.data();
        
        int elements_per_sample = output_size;
        for (int b = 0; b < batch_size; b++) {
            std::copy(grad_output_ptr, grad_output_ptr + elements_per_sample, grad_input_ptr);
            grad_output_ptr += elements_per_sample;
            grad_input_ptr += elements_per_sample;
        }
        
        return grad_input;
    }
    
    // 获取输出尺寸
    int get_output_size() const { return output_size; }
    
    // 获取输入形状
    std::tuple<int, int, int> get_input_shape() const {
        return std::make_tuple(input_channels, input_height, input_width);
    }
};
