#ifndef FLATTEN_LAYER_H
#define FLATTEN_LAYER_H

#include <vector>
#include <tuple>
#include <iostream>

class FlattenLayer {
private:
    int batch_size;
    int input_channels;
    int input_height;
    int input_width;
    int output_size;
    
    std::vector<int> input_shape_cache;
    std::vector<float> input_cache;
    
    void flatten_forward(const float* input, float* output);
    void flatten_backward(const float* grad_output, float* grad_input);
    
public:
    FlattenLayer();
    void set_input_shape(int channels, int height, int width, int batch = 1);
    void set_input_shape(const std::tuple<int, int, int>& shape_tuple);
    std::vector<float> forward(const std::vector<float>& input, int batch_size = 1, bool training = true);
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1);
    int get_output_size() const;
    std::tuple<int> get_output_shape() const;
    std::tuple<int, int, int> get_input_shape() const;
    std::tuple<int, int, int, int> get_full_input_shape() const;
    void print_info() const;
    void reset();
    bool is_initialized() const;
    int get_input_channels() const { return input_channels; }
    int get_output_channels() const { return output_size; }
};

class DynamicFlattenLayer {
private:
    int batch_size;
    int input_channels;
    int input_height;
    int input_width;
    int output_size;
    
public:
    DynamicFlattenLayer();
    std::vector<float> forward(const std::vector<float>& input, 
                               const std::tuple<int, int, int>& input_shape,
                               int batch_size = 1, 
                               bool training = true);
    std::vector<float> backward(const std::vector<float>& grad_output, int batch_size = 1);
    int get_output_size() const;
    std::tuple<int, int, int> get_input_shape() const;
    int get_input_channels() const { return input_channels; }
    int get_output_channels() const { return output_size; }
};

#endif // FLATTEN_LAYER_H