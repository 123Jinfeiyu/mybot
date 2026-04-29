#include "pooling.h"
#include <cstring>
#include <algorithm>

// ==================== 构造函数 ====================

PoolingLayer::PoolingLayer(int pool_size, int stride, int padding, bool max_pool)
    : pool_height(pool_size),
      pool_width(pool_size),
      stride(stride),
      padding(padding),
      pool_type(max_pool ? PoolType::MAX : PoolType::AVG),
      input_channels(0),
      input_height(0),
      input_width(0),
      output_height(0),
      output_width(0) {}

PoolingLayer::PoolingLayer(int pool_height, int pool_width, int stride, int padding, bool max_pool)
    : pool_height(pool_height),
      pool_width(pool_width),
      stride(stride),
      padding(padding),
      pool_type(max_pool ? PoolType::MAX : PoolType::AVG),
      input_channels(0),
      input_height(0),
      input_width(0),
      output_height(0),
      output_width(0) {}

// ==================== 设置输入尺寸 ====================

void PoolingLayer::set_input_shape(int channels, int height, int width) {
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

// ==================== 前向传播 ====================

std::vector<float> PoolingLayer::forward(const std::vector<float>& input, int batch_size, bool training) {
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

// ==================== 反向传播 ====================

std::vector<float> PoolingLayer::backward(const std::vector<float>& grad_output, int batch_size) {
    // 创建输入梯度
    int grad_input_size = batch_size * input_channels * input_height * input_width;
    std::vector<float> grad_input(grad_input_size, 0.0f);
    
    // 根据池化类型选择反向传播函数
    if (pool_type == PoolType::MAX) {
        max_pool_backward(grad_output.data(), grad_input.data(), batch_size);
    } else {
        avg_pool_backward(grad_output.data(), grad_input.data(), batch_size);
    }
    
    return grad_input;
}

// ==================== 获取输出尺寸 ====================

std::tuple<int, int, int> PoolingLayer::get_output_shape() const {
    return std::make_tuple(input_channels, output_height, output_width);
}

// ==================== 打印信息 ====================

void PoolingLayer::print_info() const {
    std::cout << "Pooling Layer Info:" << std::endl;
    std::cout << "  Type: " << (pool_type == PoolType::MAX ? "Max Pooling" : "Average Pooling") << std::endl;
    std::cout << "  Pool Size: " << pool_height << "x" << pool_width << std::endl;
    std::cout << "  Stride: " << stride << std::endl;
    std::cout << "  Padding: " << padding << std::endl;
    std::cout << "  Input Shape: " << input_channels << "x" << input_height << "x" << input_width << std::endl;
    std::cout << "  Output Shape: " << input_channels << "x" << output_height << "x" << output_width << std::endl;
}

// ==================== 最大池化前向传播 ====================

void PoolingLayer::max_pool_forward(const float* input, float* output, int batch_size) {
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
                    int end_h = (start_h + pool_height < padded_height) ? (start_h + pool_height) : padded_height;
                    int end_w = (start_w + pool_width < padded_width) ? (start_w + pool_width) : padded_width;
                    
                    // 调整起始位置，确保不超出边界
                    start_h = (start_h > 0) ? start_h : 0;
                    start_w = (start_w > 0) ? start_w : 0;
                    
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

// ==================== 平均池化前向传播 ====================

void PoolingLayer::avg_pool_forward(const float* input, float* output, int batch_size) {
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
                    int end_h = (start_h + pool_height < padded_height) ? (start_h + pool_height) : padded_height;
                    int end_w = (start_w + pool_width < padded_width) ? (start_w + pool_width) : padded_width;
                    
                    // 调整起始位置，确保不超出边界
                    start_h = (start_h > 0) ? start_h : 0;
                    start_w = (start_w > 0) ? start_w : 0;
                    
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

// ==================== 最大池化反向传播 ====================

void PoolingLayer::max_pool_backward(const float* grad_output, float* grad_input, int batch_size) {
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

// ==================== 平均池化反向传播 ====================

void PoolingLayer::avg_pool_backward(const float* grad_output, float* grad_input, int batch_size) {
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
                    int end_h = (start_h + pool_height < padded_height) ? (start_h + pool_height) : padded_height;
                    int end_w = (start_w + pool_width < padded_width) ? (start_w + pool_width) : padded_width;
                    
                    // 调整起始位置，确保不超出边界
                    start_h = (start_h > 0) ? start_h : 0;
                    start_w = (start_w > 0) ? start_w : 0;
                    
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