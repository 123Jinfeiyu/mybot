#ifndef CONV_LAYER_H
#define CONV_LAYER_H

#include <vector>
#include <tuple>
#include <random>

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
              float negative_slope = 0.01f);
    
    // 设置输入尺寸
    void set_input_shape(int height, int width);
    
    // 清空缓存
    void clear_cache();
    
    // 前向传播
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true);
    
    // 反向传播
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = -1);
    
    // 更新参数（SGD with momentum）
    void update_parameters(float learning_rate = 0.01f, float momentum = 0.9f);
    
    // Adam优化器更新参数
    void update_parameters_adam(float learning_rate = 0.001f, 
                               float beta1 = 0.9f, float beta2 = 0.999f,
                               float epsilon = 1e-8f, int t = 1);
    
    // Getters
    const std::vector<float>& get_weights() const { return weights; }
    const std::vector<float>& get_bias() const { return bias; }
    const std::vector<float>& get_weight_gradients() const { return d_weights; }
    const std::vector<float>& get_bias_gradients() const { return d_bias; }
    
    // Setters
    void set_weights(const std::vector<float>& new_weights);
    void set_bias(const std::vector<float>& new_bias);
    void set_leaky_relu_slope(float slope) { negative_slope = slope; }
    
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
                        int weight_idx, float epsilon = 1e-5f);
    
    // 重置优化器状态
    void reset_optimizer_state();
    
    // 获取缓存的batch size
    int get_cached_batch_size() const { return cached_batch_size; }
};

#endif // CONV_LAYER_H