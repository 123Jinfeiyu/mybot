#include "flatten.h"
#include <algorithm>

// ==================== FlattenLayer ====================

FlattenLayer::FlattenLayer() 
    : batch_size(0), 
      input_channels(0), 
      input_height(0), 
      input_width(0), 
      output_size(0) {
}

void FlattenLayer::set_input_shape(int channels, int height, int width, int batch) {
    input_channels = channels;
    input_height = height;
    input_width = width;
    batch_size = batch;
    output_size = channels * height * width;
    
    input_shape_cache.clear();
    input_shape_cache.push_back(channels);
    input_shape_cache.push_back(height);
    input_shape_cache.push_back(width);
    input_shape_cache.push_back(batch);
}

void FlattenLayer::set_input_shape(const std::tuple<int, int, int>& shape_tuple) {
    set_input_shape(std::get<0>(shape_tuple), 
                    std::get<1>(shape_tuple), 
                    std::get<2>(shape_tuple));
}

std::vector<float> FlattenLayer::forward(const std::vector<float>& input, int batch_size, bool training) {
    if (input_channels == 0 && input_height == 0 && input_width == 0) {
        return input;
    }
    
    int expected_input_size = batch_size * input_channels * input_height * input_width;
    if (static_cast<int>(input.size()) != expected_input_size) {
        return std::vector<float>();
    }
    
    if (training) {
        this->batch_size = batch_size;
        input_shape_cache.clear();
        input_shape_cache.push_back(input_channels);
        input_shape_cache.push_back(input_height);
        input_shape_cache.push_back(input_width);
        input_shape_cache.push_back(batch_size);
        input_cache = input;
    }
    
    int output_total_size = batch_size * output_size;
    std::vector<float> output(output_total_size, 0.0f);
    flatten_forward(input.data(), output.data());
    
    return output;
}

std::vector<float> FlattenLayer::backward(const std::vector<float>& grad_output, int batch_size) {
    int expected_grad_size = batch_size * output_size;
    if (static_cast<int>(grad_output.size()) != expected_grad_size) {
        return std::vector<float>();
    }
    
    if (input_shape_cache.size() >= 4) {
        input_channels = input_shape_cache[0];
        input_height = input_shape_cache[1];
        input_width = input_shape_cache[2];
        this->batch_size = input_shape_cache[3];
    }
    
    int input_total_size = this->batch_size * input_channels * input_height * input_width;
    std::vector<float> grad_input(input_total_size, 0.0f);
    flatten_backward(grad_output.data(), grad_input.data());
    
    return grad_input;
}

int FlattenLayer::get_output_size() const {
    return output_size;
}

std::tuple<int> FlattenLayer::get_output_shape() const {
    return std::make_tuple(output_size);
}

std::tuple<int, int, int> FlattenLayer::get_input_shape() const {
    return std::make_tuple(input_channels, input_height, input_width);
}

std::tuple<int, int, int, int> FlattenLayer::get_full_input_shape() const {
    return std::make_tuple(input_channels, input_height, input_width, batch_size);
}

void FlattenLayer::print_info() const {
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

void FlattenLayer::reset() {
    input_shape_cache.clear();
    input_cache.clear();
    batch_size = 0;
    input_channels = 0;
    input_height = 0;
    input_width = 0;
    output_size = 0;
}

bool FlattenLayer::is_initialized() const {
    return input_channels > 0 && input_height > 0 && input_width > 0;
}

void FlattenLayer::flatten_forward(const float* input, float* output) {
    int elements_per_sample = input_channels * input_height * input_width;
    
    for (int b = 0; b < batch_size; b++) {
        const float* input_ptr = input + b * elements_per_sample;
        float* output_ptr = output + b * output_size;
        std::copy(input_ptr, input_ptr + elements_per_sample, output_ptr);
    }
}

void FlattenLayer::flatten_backward(const float* grad_output, float* grad_input) {
    int elements_per_sample = input_channels * input_height * input_width;
    
    for (int b = 0; b < batch_size; b++) {
        const float* grad_output_ptr = grad_output + b * output_size;
        float* grad_input_ptr = grad_input + b * elements_per_sample;
        std::copy(grad_output_ptr, grad_output_ptr + elements_per_sample, grad_input_ptr);
    }
}

// ==================== DynamicFlattenLayer ====================

DynamicFlattenLayer::DynamicFlattenLayer() 
    : batch_size(0), 
      input_channels(0), 
      input_height(0), 
      input_width(0), 
      output_size(0) {
}

std::vector<float> DynamicFlattenLayer::forward(const std::vector<float>& input, 
                                                 const std::tuple<int, int, int>& input_shape,
                                                 int batch_size, 
                                                 bool training) {
    input_channels = std::get<0>(input_shape);
    input_height = std::get<1>(input_shape);
    input_width = std::get<2>(input_shape);
    this->batch_size = batch_size;
    
    int expected_size = batch_size * input_channels * input_height * input_width;
    if (static_cast<int>(input.size()) != expected_size) {
        return std::vector<float>();
    }
    
    output_size = input_channels * input_height * input_width;
    int total_output_size = batch_size * output_size;
    std::vector<float> output(total_output_size);
    
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

std::vector<float> DynamicFlattenLayer::backward(const std::vector<float>& grad_output, int batch_size) {
    if (output_size == 0) {
        return std::vector<float>();
    }
    
    int expected_grad_size = batch_size * output_size;
    if (static_cast<int>(grad_output.size()) != expected_grad_size) {
        return std::vector<float>();
    }
    
    int total_input_size = batch_size * input_channels * input_height * input_width;
    std::vector<float> grad_input(total_input_size);
    
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

int DynamicFlattenLayer::get_output_size() const {
    return output_size;
}

std::tuple<int, int, int> DynamicFlattenLayer::get_input_shape() const {
    return std::make_tuple(input_channels, input_height, input_width);
}