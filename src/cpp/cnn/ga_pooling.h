#ifndef GLOBAL_AVERAGE_POOLING_H
#define GLOBAL_AVERAGE_POOLING_H

#include <vector>
#include <tuple>

class GlobalAveragePooling {
private:
    int input_channels;
    int input_height;
    int input_width;
    std::vector<float> input_cache;
    
public:
    // 构造函数
    GlobalAveragePooling();
    
    // 设置输入尺寸
    void set_input_shape(int channels, int height, int width);
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true);
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1);
    
    // 获取输出尺寸
    std::tuple<int, int> get_output_shape() const;
    
    // 获取输入输出通道数
    int get_input_channels() const { return input_channels; }
    int get_output_channels() const { return input_channels; }
    
    // 获取输入尺寸
    int get_input_height() const { return input_height; }
    int get_input_width() const { return input_width; }
};

#endif // GLOBAL_AVERAGE_POOLING_H