// cnn_c_interface.cpp
//#include "cnn_i.h"
#include "conv.h"
#include "pooling.h"
#include "ga_pooling.h"
#include "flatten.h"
#include "imgl.h"
#include <cstring>
#include <vector>

// 全局错误信息缓冲区
static thread_local char g_last_error[1024] = {0};

static void set_error(const char* msg) {
    std::strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = '\0';
}

static void clear_error() {
    g_last_error[0] = '\0';
}

// 辅助：分配并复制浮点数组
static float* alloc_and_copy(const std::vector<float>& data, size_t* out_size) {
    if (data.empty()) {
        *out_size = 0;
        return nullptr;
    }
    *out_size = data.size();
    float* buffer = new float[data.size()];
    std::memcpy(buffer, data.data(), data.size() * sizeof(float));
    return buffer;
}

// ============================================================================
// 卷积层实现
// ============================================================================
#ifdef _WIN64
#define CNN_API _declspec(dllexport)
#else
#define CNN_API
#endif

CNN_API void* cnn_create_conv_layer(int in_channels, int out_channels, 
                            int kernel_size, int stride, int padding,
                            float negative_slope) {
    clear_error();
    try {
        auto* layer = new ConvLayer(in_channels, out_channels, 
                                    kernel_size, stride, padding, negative_slope);
        return static_cast<void*>(layer);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

CNN_API void cnn_destroy_conv_layer(void* layer) {
    if (layer) {
        delete static_cast<ConvLayer*>(layer);
    }
}

CNN_API void cnn_conv_set_input_shape(void* layer, int height, int width) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    clear_error();
    try {
        static_cast<ConvLayer*>(layer)->set_input_shape(height, width);
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API float* cnn_conv_forward(void* layer, const float* input, size_t input_size,
                        int batch_size, int training, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* conv = static_cast<ConvLayer*>(layer);
        std::vector<float> input_vec(input, input + input_size);
        auto output = conv->forward(input_vec, batch_size, training != 0);
        return alloc_and_copy(output, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_conv_backward(void* layer, const float* grad_output, size_t grad_size,
                         int batch_size, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* conv = static_cast<ConvLayer*>(layer);
        std::vector<float> grad_vec(grad_output, grad_output + grad_size);
        auto d_input = conv->backward(grad_vec, batch_size);
        return alloc_and_copy(d_input, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API void cnn_conv_update_parameters(void* layer, float learning_rate, float momentum) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    clear_error();
    try {
        static_cast<ConvLayer*>(layer)->update_parameters(learning_rate, momentum);
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API void cnn_conv_update_parameters_adam(void* layer, float learning_rate, 
                                     float beta1, float beta2, 
                                     float epsilon, int t) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    clear_error();
    try {
        static_cast<ConvLayer*>(layer)->update_parameters_adam(learning_rate, beta1, beta2, epsilon, t);
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API size_t cnn_conv_get_weights_size(void* layer) {
    if (!layer) return 0;
    return static_cast<ConvLayer*>(layer)->get_weights().size();
}

CNN_API size_t cnn_conv_get_bias_size(void* layer) {
    if (!layer) return 0;
    return static_cast<ConvLayer*>(layer)->get_bias().size();
}

CNN_API float* cnn_conv_get_weights(void* layer) {
    if (!layer) {
        set_error("Null layer pointer");
        return nullptr;
    }
    auto& weights = static_cast<ConvLayer*>(layer)->get_weights();
    float* buffer = new float[weights.size()];
    std::memcpy(buffer, weights.data(), weights.size() * sizeof(float));
    return buffer;
}

CNN_API float* cnn_conv_get_bias(void* layer) {
    if (!layer) {
        set_error("Null layer pointer");
        return nullptr;
    }
    auto& bias = static_cast<ConvLayer*>(layer)->get_bias();
    float* buffer = new float[bias.size()];
    std::memcpy(buffer, bias.data(), bias.size() * sizeof(float));
    return buffer;
}

CNN_API void cnn_conv_set_weights(void* layer, const float* weights, size_t size) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    clear_error();
    try {
        auto* conv = static_cast<ConvLayer*>(layer);
        if (size != conv->get_weights().size()) {
            set_error("Weight size mismatch");
            return;
        }
        conv->set_weights(std::vector<float>(weights, weights + size));
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API void cnn_conv_set_bias(void* layer, const float* bias, size_t size) { 
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    clear_error();
    try {
        auto* conv = static_cast<ConvLayer*>(layer);
        if (size != conv->get_bias().size()) {
            set_error("Bias size mismatch");
            return;
        }
        conv->set_bias(std::vector<float>(bias, bias + size));
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API void cnn_conv_get_output_shape(void* layer, int* out_channels, 
                                int* out_height, int* out_width) {
    if (!layer) {
        set_error("Null layer pointer");
        *out_channels = 0;
        *out_height = 0;
        *out_width = 0;
        return;
    }
    *out_channels = static_cast<ConvLayer*>(layer)->get_output_channels();
    auto shape = static_cast<ConvLayer*>(layer)->get_output_shape();
    *out_height = std::get<0>(shape);
    *out_width = std::get<1>(shape);
}

CNN_API void cnn_conv_clear_cache(void* layer) {
    if (layer) {
        static_cast<ConvLayer*>(layer)->clear_cache();
    }
}

CNN_API void cnn_conv_reset_optimizer_state(void* layer) {
    if (layer) {
        static_cast<ConvLayer*>(layer)->reset_optimizer_state();
    }
}

// ============================================================================
// 池化层实现
// ============================================================================

CNN_API void* cnn_create_pooling_layer(int pool_height, int pool_width, 
                               int stride, int padding, bool is_max) {
    clear_error();
    try {
        auto* layer = new PoolingLayer(pool_height, pool_width, stride, padding, is_max);
        return static_cast<void*>(layer);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

CNN_API void cnn_destroy_pooling_layer(void* layer) {
    if (layer) {
        delete static_cast<PoolingLayer*>(layer);
    }
}

CNN_API void cnn_pooling_set_input_shape(void* layer, int channels, int height, int width) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    clear_error();
    try {
        static_cast<PoolingLayer*>(layer)->set_input_shape(channels, height, width);
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API float* cnn_pooling_forward(void* layer, const float* input, size_t input_size,
                           int batch_size, int training, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* pool = static_cast<PoolingLayer*>(layer);
        std::vector<float> input_vec(input, input + input_size);
        auto output = pool->forward(input_vec, batch_size, training != 0);
        return alloc_and_copy(output, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_pooling_backward(void* layer, const float* grad_output, size_t grad_size,
                            int batch_size, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* pool = static_cast<PoolingLayer*>(layer);
        std::vector<float> grad_vec(grad_output, grad_output + grad_size);
        auto d_input = pool->backward(grad_vec, batch_size);
        return alloc_and_copy(d_input, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API void cnn_pooling_get_output_shape(void* layer, int* out_channels,
                                   int* out_height, int* out_width) {
    if (!layer) {
        set_error("Null layer pointer");
        *out_channels = 0;
        *out_height = 0;
        *out_width = 0;
        return;
    }
    auto shape = static_cast<PoolingLayer*>(layer)->get_output_shape();
    *out_channels = std::get<0>(shape);
    *out_height = std::get<1>(shape);
    *out_width = std::get<2>(shape);
}

// ============================================================================
// 全局平均池化层实现
// ============================================================================

CNN_API void* cnn_create_global_avg_pool_layer(void) {
    clear_error();
    try {
        auto* layer = new GlobalAveragePooling();
        return static_cast<void*>(layer);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

CNN_API void cnn_destroy_global_avg_pool_layer(void* layer) {
    if (layer) {
        delete static_cast<GlobalAveragePooling*>(layer);
    }
}

CNN_API void cnn_global_avg_pool_set_input_shape(void* layer, int channels, int height, int width) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    static_cast<GlobalAveragePooling*>(layer)->set_input_shape(channels, height, width);
}

CNN_API float* cnn_global_avg_pool_forward(void* layer, const float* input, size_t input_size,
                                   int batch_size, int training, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* pool = static_cast<GlobalAveragePooling*>(layer);
        std::vector<float> input_vec(input, input + input_size);
        auto output = pool->forward(input_vec, batch_size, training != 0);
        return alloc_and_copy(output, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_global_avg_pool_backward(void* layer, const float* grad_output, size_t grad_size,
                                    int batch_size, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* pool = static_cast<GlobalAveragePooling*>(layer);
        std::vector<float> grad_vec(grad_output, grad_output + grad_size);
        auto d_input = pool->backward(grad_vec, batch_size);
        return alloc_and_copy(d_input, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

// ============================================================================
// Flatten 层实现
// ============================================================================

CNN_API void* cnn_create_flatten_layer(void) {
    clear_error();
    try {
        auto* layer = new FlattenLayer();
        return static_cast<void*>(layer);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

CNN_API void cnn_destroy_flatten_layer(void* layer) {
    if (layer) {
        delete static_cast<FlattenLayer*>(layer);
    }
}

CNN_API void cnn_flatten_set_input_shape(void* layer, int channels, int height, int width, int batch) {
    if (!layer) {
        set_error("Null layer pointer");
        return;
    }
    static_cast<FlattenLayer*>(layer)->set_input_shape(channels, height, width, batch);
}

CNN_API float* cnn_flatten_forward(void* layer, const float* input, size_t input_size,
                           int batch_size, int training, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* flatten = static_cast<FlattenLayer*>(layer);
        std::vector<float> input_vec(input, input + input_size);
        auto output = flatten->forward(input_vec, batch_size, training != 0);
        return alloc_and_copy(output, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_flatten_backward(void* layer, const float* grad_output, size_t grad_size,
                            int batch_size, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* flatten = static_cast<FlattenLayer*>(layer);
        std::vector<float> grad_vec(grad_output, grad_output + grad_size);
        auto d_input = flatten->backward(grad_vec, batch_size);
        return alloc_and_copy(d_input, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

// ============================================================================
// Dynamic Flatten 层实现
// ============================================================================

CNN_API void* cnn_create_dynamic_flatten_layer(void) {
    clear_error();
    try {
        auto* layer = new DynamicFlattenLayer();
        return static_cast<void*>(layer);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

CNN_API void cnn_destroy_dynamic_flatten_layer(void* layer) {
    if (layer) {
        delete static_cast<DynamicFlattenLayer*>(layer);
    }
}

CNN_API float* cnn_dynamic_flatten_forward(void* layer, const float* input, size_t input_size,
                                   int channels, int height, int width,
                                   int batch_size, int training, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* flatten = static_cast<DynamicFlattenLayer*>(layer);
        std::vector<float> input_vec(input, input + input_size);
        auto output = flatten->forward(input_vec, std::make_tuple(channels, height, width), 
                                       batch_size, training != 0);
        return alloc_and_copy(output, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_dynamic_flatten_backward(void* layer, const float* grad_output, size_t grad_size,
                                    int batch_size, size_t* output_size) {
    if (!layer) {
        set_error("Null layer pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* flatten = static_cast<DynamicFlattenLayer*>(layer);
        std::vector<float> grad_vec(grad_output, grad_output + grad_size);
        auto d_input = flatten->backward(grad_vec, batch_size);
        return alloc_and_copy(d_input, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

// ============================================================================
// 图像加载器实现
// ============================================================================

CNN_API void* cnn_create_image_loader(int channels, int height, int width,
                              int norm_01, int norm_mean_std,
                              float mean, float std, int auto_compute) {
    clear_error();
    try {
        auto* loader = new ImageLoader(channels, height, width,
                                       norm_01 != 0, norm_mean_std != 0,
                                       mean, std, auto_compute != 0);
        return static_cast<void*>(loader);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

CNN_API void cnn_destroy_image_loader(void* loader) {
    if (loader) {
        delete static_cast<ImageLoader*>(loader);
    }
}

CNN_API float* cnn_load_image(void* loader, const char* filepath, size_t* output_size) {
    if (!loader) {
        set_error("Null loader pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* img_loader = static_cast<ImageLoader*>(loader);
        auto data = img_loader->load_image(filepath);
        return alloc_and_copy(data, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_load_batch(void* loader, const char** filepaths, int count, size_t* output_size) {
    if (!loader) {
        set_error("Null loader pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* img_loader = static_cast<ImageLoader*>(loader);
        std::vector<std::string> paths;
        for (int i = 0; i < count; i++) {
            paths.push_back(filepaths[i]);
        }
        auto data = img_loader->load_batch(paths);
        return alloc_and_copy(data, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API float* cnn_load_from_directory(void* loader, const char* dir_path,  
                               const char** extensions, int ext_count,
                               size_t* output_size) {
    if (!loader) {
        set_error("Null loader pointer");
        *output_size = 0;
        return nullptr;
    }
    clear_error();
    try {
        auto* img_loader = static_cast<ImageLoader*>(loader);
        std::vector<std::string> exts;
        for (int i = 0; i < ext_count; i++) {
            exts.push_back(extensions[i]);
        }
        auto data = img_loader->load_from_directory(dir_path, exts);
        return alloc_and_copy(data, output_size);
    } catch (const std::exception& e) {
        set_error(e.what());
        *output_size = 0;
        return nullptr;
    }
}

CNN_API void cnn_compute_dataset_statistics(void* loader, const char** filepaths,
                                    int count, int max_images) {
    if (!loader) {
        set_error("Null loader pointer");
        return;
    }
    clear_error();
    try {
        auto* img_loader = static_cast<ImageLoader*>(loader);
        std::vector<std::string> paths;
        for (int i = 0; i < count; i++) {
            paths.push_back(filepaths[i]);
        }
        img_loader->compute_dataset_statistics(paths, max_images);
    } catch (const std::exception& e) {
        set_error(e.what());
    }
}

CNN_API void cnn_set_normalization_params(void* loader, const float* means, 
                                  const float* stds, int num_channels) {
    if (!loader) {
        set_error("Null loader pointer");
        return;
    }
    auto* img_loader = static_cast<ImageLoader*>(loader);
    img_loader->set_normalization_params(
        std::vector<float>(means, means + num_channels),
        std::vector<float>(stds, stds + num_channels));
}

CNN_API float* cnn_get_normalization_means(void* loader, size_t* means_size) {
    if (!loader) {
        set_error("Null loader pointer");
        *means_size = 0;
        return nullptr;
    }
    auto [means, stds] = static_cast<ImageLoader*>(loader)->get_normalization_params();
    return alloc_and_copy(means, means_size);
}

CNN_API float* cnn_get_normalization_stds(void* loader, size_t* stds_size) {
    if (!loader) {
        set_error("Null loader pointer");
        *stds_size = 0;
        return nullptr;
    }
    auto [means, stds] = static_cast<ImageLoader*>(loader)->get_normalization_params();
    return alloc_and_copy(stds, stds_size);
}

CNN_API int cnn_image_loader_get_height(void* loader) {
    if (!loader) return 0;
    return static_cast<ImageLoader*>(loader)->get_target_height();
}

CNN_API int cnn_image_loader_get_width(void* loader) {
    if (!loader) return 0;
    return static_cast<ImageLoader*>(loader)->get_target_width();
}

CNN_API int cnn_image_loader_get_channels(void* loader) {
    if (!loader) return 0;
    return static_cast<ImageLoader*>(loader)->get_target_channels();
}

// ============================================================================
// 通用内存管理
// ============================================================================

CNN_API void cnn_free_buffer(float* buffer) {
    delete[] buffer;
}

// ============================================================================
// 辅助函数
// ============================================================================

CNN_API const char* cnn_get_last_error(void) {
    return g_last_error;
}