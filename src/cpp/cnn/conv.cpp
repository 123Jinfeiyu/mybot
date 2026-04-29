#include "conv.h"
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <string>

ConvLayer::ConvLayer(int in_channels, int out_channels, 
          int kernel_size, int stride, int padding, 
          float negative_slope)
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

void ConvLayer::set_input_shape(int height, int width) {
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

void ConvLayer::clear_cache() {
    input_cache.clear();
    output_cache.clear();
    activated_cache.clear();
    col_cache.clear();
    cached_batch_size = 0;
}

std::vector<float> ConvLayer::forward(const std::vector<float>& input, int batch_size, bool training) {
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

std::vector<float> ConvLayer::backward(const std::vector<float>& grad_output, int batch_size) {
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

void ConvLayer::update_parameters(float learning_rate, float momentum) {
    for (size_t i = 0; i < weights.size(); i++) {
        weight_momentum[i] = momentum * weight_momentum[i] - learning_rate * d_weights[i];
        weights[i] += weight_momentum[i];
    }
    
    for (size_t i = 0; i < bias.size(); i++) {
        bias_momentum[i] = momentum * bias_momentum[i] - learning_rate * d_bias[i];
        bias[i] += bias_momentum[i];
    }
}

void ConvLayer::update_parameters_adam(float learning_rate, 
                                       float beta1, float beta2,
                                       float epsilon, int t) {
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

void ConvLayer::set_weights(const std::vector<float>& new_weights) {
    if (new_weights.size() != weights.size()) {
        throw std::invalid_argument("Weight size mismatch");
    }
    weights = new_weights;
}

void ConvLayer::set_bias(const std::vector<float>& new_bias) {
    if (new_bias.size() != bias.size()) {
        throw std::invalid_argument("Bias size mismatch");
    }
    bias = new_bias;
}

float ConvLayer::gradient_check(const std::vector<float>& input, 
                                const std::vector<float>& grad_output,
                                int weight_idx, float epsilon) {
    if (weight_idx < 0 || weight_idx >= static_cast<int>(weights.size())) {
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

void ConvLayer::reset_optimizer_state() {
    std::fill(m_weights.begin(), m_weights.end(), 0.0f);
    std::fill(v_weights.begin(), v_weights.end(), 0.0f);
    std::fill(m_bias.begin(), m_bias.end(), 0.0f);
    std::fill(v_bias.begin(), v_bias.end(), 0.0f);
    std::fill(weight_momentum.begin(), weight_momentum.end(), 0.0f);
    std::fill(bias_momentum.begin(), bias_momentum.end(), 0.0f);
}

// ==================== 辅助函数实现 ====================

void ConvLayer::im2col(const float* input) {
    int K = input_channels * kernel_size * kernel_size;
    int N = output_height * output_width;
    int H = input_height;
    int W = input_width;
    int C = input_channels;
    int KH = kernel_size;
    int KW = kernel_size;
    int OH = output_height;
    int OW = output_width;
    
    // 清空col_buffer
    std::fill(col_buffer.begin(), col_buffer.end(), 0.0f);
    
    // 对每个输出位置
    for (int oh = 0; oh < OH; oh++) {
        for (int ow = 0; ow < OW; ow++) {
            // 输入起始位置（考虑padding）
            int h_start = oh * stride - padding;
            int w_start = ow * stride - padding;
            
            // 对每个卷积核位置
            for (int kh = 0; kh < KH; kh++) {
                for (int kw = 0; kw < KW; kw++) {
                    int h_in = h_start + kh;
                    int w_in = w_start + kw;
                    
                    // 对每个输入通道
                    for (int c = 0; c < C; c++) {
                        int col_row = (c * KH * KW + kh * KW + kw);
                        int col_col = oh * OW + ow;
                        
                        // 检查是否在有效范围内
                        if (h_in >= 0 && h_in < H && w_in >= 0 && w_in < W) {
                            col_buffer[col_row * N + col_col] = input[c * H * W + h_in * W + w_in];
                        } else {
                            col_buffer[col_row * N + col_col] = 0.0f; // padding部分填充0
                        }
                    }
                }
            }
        }
    }
}

void ConvLayer::col2im(const float* col, float* im) {
    int K = input_channels * kernel_size * kernel_size;
    int N = output_height * output_width;
    int H = input_height;
    int W = input_width;
    int C = input_channels;
    int KH = kernel_size;
    int KW = kernel_size;
    int OH = output_height;
    int OW = output_width;
    
    // 清空输出图像
    int im_size = C * H * W;
    std::fill(im, im + im_size, 0.0f);
    
    // 对每个输出位置
    for (int oh = 0; oh < OH; oh++) {
        for (int ow = 0; ow < OW; ow++) {
            // 输入起始位置（考虑padding）
            int h_start = oh * stride - padding;
            int w_start = ow * stride - padding;
            
            // 对每个卷积核位置
            for (int kh = 0; kh < KH; kh++) {
                for (int kw = 0; kw < KW; kw++) {
                    int h_in = h_start + kh;
                    int w_in = w_start + kw;
                    
                    // 只处理有效位置
                    if (h_in >= 0 && h_in < H && w_in >= 0 && w_in < W) {
                        // 对每个输入通道
                        for (int c = 0; c < C; c++) {
                            int col_row = (c * KH * KW + kh * KW + kw);
                            int col_col = oh * OW + ow;
                            
                            // 累加梯度
                            im[c * H * W + h_in * W + w_in] += col[col_row * N + col_col];
                        }
                    }
                }
            }
        }
    }
}

void ConvLayer::sgemm(int M, int N, int K, 
                      const float* A, const float* B, float* C,
                      bool transA, bool transB) {
    // 简单的矩阵乘法实现 C = A * B
    // A: M x K (如果transA则为K x M)
    // B: K x N (如果transB则为N x K)
    // C: M x N
    
    std::fill(C, C + M * N, 0.0f);
    
    if (!transA && !transB) {
        // C = A * B (标准矩阵乘法)
        for (int i = 0; i < M; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0.0f;
                for (int k = 0; k < K; k++) {
                    sum += A[i * K + k] * B[k * N + j];
                }
                C[i * N + j] = sum;
            }
        }
    } else if (transA && !transB) {
        // C = A^T * B
        for (int i = 0; i < M; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0.0f;
                for (int k = 0; k < K; k++) {
                    sum += A[k * M + i] * B[k * N + j];
                }
                C[i * N + j] = sum;
            }
        }
    } else if (!transA && transB) {
        // C = A * B^T
        for (int i = 0; i < M; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0.0f;
                for (int k = 0; k < K; k++) {
                    sum += A[i * K + k] * B[j * K + k];
                }
                C[i * N + j] = sum;
            }
        }
    } else {
        // C = A^T * B^T
        for (int i = 0; i < M; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0.0f;
                for (int k = 0; k < K; k++) {
                    sum += A[k * M + i] * B[j * K + k];
                }
                C[i * N + j] = sum;
            }
        }
    }
}

void ConvLayer::sgemm_add(int M, int N, int K,
                          const float* A, const float* B, float* C,
                          float alpha, float beta,
                          bool transA, bool transB) {
    // C = alpha * A * B + beta * C
    
    // 先计算 A * B 到临时数组
    std::vector<float> temp(M * N, 0.0f);
    sgemm(M, N, K, A, B, temp.data(), transA, transB);
    
    // C = alpha * temp + beta * C
    for (int i = 0; i < M * N; i++) {
        C[i] = alpha * temp[i] + beta * C[i];
    }
}