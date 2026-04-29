#ifndef POOLING_LAYER_H
#define POOLING_LAYER_H

#include <vector>
#include <tuple>
#include <iostream>
#include <limits>

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
    
    // 私有辅助函数
    void max_pool_forward(const float* input, float* output, int batch_size);
    void avg_pool_forward(const float* input, float* output, int batch_size);
    void max_pool_backward(const float* grad_output, float* grad_input, int batch_size);
    void avg_pool_backward(const float* grad_output, float* grad_input, int batch_size);
    
public:
    // 构造函数
    PoolingLayer(int pool_size, int stride = 2, int padding = 0, bool max_pool = true);
    
    // 带不同高度和宽度的构造函数
    PoolingLayer(int pool_height, int pool_width, int stride = 2, int padding = 0, bool max_pool = true);
    
    // 设置输入尺寸
    void set_input_shape(int channels, int height, int width);
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true);
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1);
    
    // 获取输出尺寸
    std::tuple<int, int, int> get_output_shape() const;
    
    // 获取参数信息
    void print_info() const;
    
    // 获取输入输出通道数
    int get_input_channels() const { return input_channels; }
    int get_output_channels() const { return input_channels; }
    
    // 获取输入输出尺寸
    int get_input_height() const { return input_height; }
    int get_input_width() const { return input_width; }
    int get_output_height() const { return output_height; }
    int get_output_width() const { return output_width; }
};

#endif // POOLING_LAYER_H