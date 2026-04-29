#include "ga_pooling.h"

// ==================== 构造函数 ====================

GlobalAveragePooling::GlobalAveragePooling()
    : input_channels(0),
      input_height(0),
      input_width(0) {
}

// ==================== 设置输入尺寸 ====================

void GlobalAveragePooling::set_input_shape(int channels, int height, int width) {
    input_channels = channels;
    input_height = height;
    input_width = width;
    input_cache.clear();
}

// ==================== 前向传播 ====================

std::vector<float> GlobalAveragePooling::forward(const std::vector<float>& input, int batch_size, bool training) {
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

// ==================== 反向传播 ====================

std::vector<float> GlobalAveragePooling::backward(const std::vector<float>& grad_output, int batch_size) {
    // 创建输入梯度
    int spatial_size = input_height * input_width;
    int grad_input_size = batch_size * input_channels * spatial_size;
    std::vector<float> grad_input(grad_input_size, 0.0f);
    
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

// ==================== 获取输出尺寸 ====================

std::tuple<int, int> GlobalAveragePooling::get_output_shape() const {
    return std::make_tuple(input_channels, 1);
}