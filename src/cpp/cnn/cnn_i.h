// cnn_c_interface.h
#pragma once

#include <cstddef>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 神经网络层接口
// ============================================================================

// --- 卷积层 ---

// 创建卷积层
// in_channels: 输入通道数
// out_channels: 输出通道数
// kernel_size: 卷积核大小
// stride: 步长
// padding: 填充
// negative_slope: LeakyReLU 负斜率
// 返回: 不透明指针指向卷积层对象
void* cnn_create_conv_layer(int in_channels, int out_channels, 
                            int kernel_size, int stride, int padding,
                            float negative_slope);

// 销毁卷积层
void cnn_destroy_conv_layer(void* layer);

// 设置卷积层输入形状
void cnn_conv_set_input_shape(void* layer, int height, int width);

// 卷积层前向传播
// input: 输入数据数组
// input_size: 输入数据长度
// batch_size: 批次大小
// training: 是否训练模式
// output_size: 输出参数，返回输出数据长度
// 返回: 输出数据数组（调用者需要调用 cnn_free_buffer 释放）
float* cnn_conv_forward(void* layer, const float* input, size_t input_size,
                        int batch_size, int training, size_t* output_size);

// 卷积层反向传播
// grad_output: 梯度输入数组
// grad_size: 梯度输入长度
// batch_size: 批次大小
// output_size: 输出参数，返回梯度输出长度
// 返回: 输入梯度数组（调用者需要调用 cnn_free_buffer 释放）
float* cnn_conv_backward(void* layer, const float* grad_output, size_t grad_size,
                         int batch_size, size_t* output_size);

// 卷积层参数更新 (SGD with momentum)
void cnn_conv_update_parameters(void* layer, float learning_rate, float momentum);

// 卷积层参数更新 (Adam)
void cnn_conv_update_parameters_adam(void* layer, float learning_rate, 
                                     float beta1, float beta2, 
                                     float epsilon, int t);

// 获取卷积层权重数量
size_t cnn_conv_get_weights_size(void* layer);

// 获取卷积层偏置数量
size_t cnn_conv_get_bias_size(void* layer);

// 获取卷积层权重
// 返回: 权重数组（调用者需要调用 cnn_free_buffer 释放）
float* cnn_conv_get_weights(void* layer);

// 获取卷积层偏置
// 返回: 偏置数组（调用者需要调用 cnn_free_buffer 释放）
float* cnn_conv_get_bias(void* layer);

// 设置卷积层权重
void cnn_conv_set_weights(void* layer, const float* weights, size_t size);

// 设置卷积层偏置
void cnn_conv_set_bias(void* layer, const float* bias, size_t size);

// 获取卷积层输出形状
// out_channels: 输出参数，输出通道数
// out_height: 输出参数，输出高度
// out_width: 输出参数，输出宽度
void cnn_conv_get_output_shape(void* layer, int* out_channels, 
                                int* out_height, int* out_width);

// 清空卷积层缓存
void cnn_conv_clear_cache(void* layer);

// 重置卷积层优化器状态
void cnn_conv_reset_optimizer_state(void* layer);

// --- 池化层 ---

// 创建池化层
// pool_height: 池化窗口高度
// pool_width: 池化窗口宽度
// stride: 步长
// padding: 填充
// pool_type: 池化类型 (CNN_POOL_MAX 或 CNN_POOL_AVG)
// 返回: 不透明指针指向池化层对象
void* cnn_create_pooling_layer(int pool_height, int pool_width, 
                               int stride, int padding, int pool_type);

// 销毁池化层
void cnn_destroy_pooling_layer(void* layer);

// 设置池化层输入形状
void cnn_pooling_set_input_shape(void* layer, int channels, int height, int width);

// 池化层前向传播
float* cnn_pooling_forward(void* layer, const float* input, size_t input_size,
                           int batch_size, int training, size_t* output_size);

// 池化层反向传播
float* cnn_pooling_backward(void* layer, const float* grad_output, size_t grad_size,
                            int batch_size, size_t* output_size);

// 获取池化层输出形状
void cnn_pooling_get_output_shape(void* layer, int* out_channels,
                                   int* out_height, int* out_width);

// --- 全局平均池化层 ---

// 创建全局平均池化层
void* cnn_create_global_avg_pool_layer(void);

// 销毁全局平均池化层
void cnn_destroy_global_avg_pool_layer(void* layer);

// 设置全局平均池化层输入形状
void cnn_global_avg_pool_set_input_shape(void* layer, int channels, int height, int width);

// 全局平均池化层前向传播
float* cnn_global_avg_pool_forward(void* layer, const float* input, size_t input_size,
                                   int batch_size, int training, size_t* output_size);

// 全局平均池化层反向传播
float* cnn_global_avg_pool_backward(void* layer, const float* grad_output, size_t grad_size,
                                    int batch_size, size_t* output_size);

// --- Flatten 层 ---

// 创建 Flatten 层
void* cnn_create_flatten_layer(void);

// 销毁 Flatten 层
void cnn_destroy_flatten_layer(void* layer);

// 设置 Flatten 层输入形状
void cnn_flatten_set_input_shape(void* layer, int channels, int height, int width, int batch);

// Flatten 层前向传播
float* cnn_flatten_forward(void* layer, const float* input, size_t input_size,
                           int batch_size, int training, size_t* output_size);

// Flatten 层反向传播
float* cnn_flatten_backward(void* layer, const float* grad_output, size_t grad_size,
                            int batch_size, size_t* output_size);

// --- Dynamic Flatten 层 ---

// 创建 Dynamic Flatten 层
void* cnn_create_dynamic_flatten_layer(void);

// 销毁 Dynamic Flatten 层
void cnn_destroy_dynamic_flatten_layer(void* layer);

// Dynamic Flatten 层前向传播
float* cnn_dynamic_flatten_forward(void* layer, const float* input, size_t input_size,
                                   int channels, int height, int width,
                                   int batch_size, int training, size_t* output_size);

// Dynamic Flatten 层反向传播
float* cnn_dynamic_flatten_backward(void* layer, const float* grad_output, size_t grad_size,
                                    int batch_size, size_t* output_size);

// ============================================================================
// 图像加载器接口
// ============================================================================

// 创建图像加载器
// channels: 目标通道数
// height: 目标高度
// width: 目标宽度
// norm_01: 是否归一化到 [0,1]
// norm_mean_std: 是否使用均值标准差归一化
// mean: 均值
// std: 标准差
// auto_compute: 是否自动计算均值标准差
// 返回: 不透明指针指向图像加载器对象
void* cnn_create_image_loader(int channels, int height, int width,
                              int norm_01, int norm_mean_std,
                              float mean, float std, int auto_compute);

// 销毁图像加载器
void cnn_destroy_image_loader(void* loader);

// 加载单张图像
// filepath: 图像文件路径
// output_size: 输出参数，返回图像数据长度
// 返回: 图像数据数组（调用者需要调用 cnn_free_buffer 释放）
float* cnn_load_image(void* loader, const char* filepath, size_t* output_size);

// 批量加载图像
// filepaths: 图像文件路径数组
// count: 图像数量
// output_size: 输出参数，返回批量数据长度
// 返回: 批量图像数据数组
float* cnn_load_batch(void* loader, const char** filepaths, int count, size_t* output_size);

// 从目录加载所有图像
// dir_path: 目录路径
// extensions: 允许的文件扩展名数组
// ext_count: 扩展名数量
// output_size: 输出参数，返回数据长度
// 返回: 图像数据数组
float* cnn_load_from_directory(void* loader, const char* dir_path,
                               const char** extensions, int ext_count,
                               size_t* output_size);

// 计算数据集统计量
void cnn_compute_dataset_statistics(void* loader, const char** filepaths, 
                                    int count, int max_images);

// 设置归一化参数
void cnn_set_normalization_params(void* loader, const float* means, 
                                  const float* stds, int num_channels);

// 获取归一化参数
// means_size: 输出参数，均值数组长度
// stds_size: 输出参数，标准差数组长度
float* cnn_get_normalization_means(void* loader, size_t* means_size);
float* cnn_get_normalization_stds(void* loader, size_t* stds_size);

// 获取目标尺寸
int cnn_image_loader_get_height(void* loader);
int cnn_image_loader_get_width(void* loader);
int cnn_image_loader_get_channels(void* loader);

// ============================================================================
// 通用内存管理
// ============================================================================

// 释放由 C 接口返回的缓冲区
void cnn_free_buffer(float* buffer);

// ============================================================================
// 辅助函数
// ============================================================================

// 获取错误信息
const char* cnn_get_last_error(void);

// 获取版本信息
const char* cnn_get_version(void);

#ifdef __cplusplus
}
#endif