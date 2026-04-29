#include "imgl.h"
#include <iostream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <sys/stat.h>

// S_ISREG 兼容性定义
#ifndef S_ISREG
    #define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

// ==================== 辅助函数实现 ====================

bool ImageLoader::file_exists(const std::string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

bool ImageLoader::is_regular_file(const std::string& path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    #ifdef _WIN32
        DWORD attributes = GetFileAttributesA(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        return !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    #else
        return S_ISREG(path_stat.st_mode);
    #endif
}

std::string ImageLoader::get_file_extension(const std::string& filename) {
    size_t dot_pos = filename.find_last_of(".");
    if (dot_pos == std::string::npos) {
        return "";
    }
    return filename.substr(dot_pos);
}

std::string ImageLoader::to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// Windows 平台目录遍历实现
#ifdef _WIN32

std::vector<std::string> ImageLoader::list_directory(const std::string& dir_path, 
                                                      const std::vector<std::string>& extensions) {
    std::vector<std::string> image_files;
    
    std::string search_path = dir_path;
    if (dir_path.back() != '\\' && dir_path.back() != '/') {
        search_path += "\\";
    }
    search_path += "*";
    
    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path.c_str(), &find_data);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        return image_files;
    }
    
    do {
        std::string filename = find_data.cFileName;
        
        if (filename == "." || filename == "..") {
            continue;
        }
        
        std::string full_path = dir_path;
        if (dir_path.back() != '\\' && dir_path.back() != '/') {
            full_path += "\\";
        }
        full_path += filename;
        
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string ext = get_file_extension(filename);
            std::string ext_lower = to_lower(ext);
            
            bool valid_ext = false;
            for (size_t i = 0; i < extensions.size(); i++) {
                if (ext_lower == to_lower(extensions[i])) {
                    valid_ext = true;
                    break;
                }
            }
            
            if (valid_ext) {
                image_files.push_back(full_path);
            }
        }
    } while (FindNextFileA(hFind, &find_data) != 0);
    
    FindClose(hFind);
    
    return image_files;
}

#else

// POSIX 平台目录遍历实现
std::vector<std::string> ImageLoader::list_directory_posix(const std::string& dir_path, 
                                                            const std::vector<std::string>& extensions) {
    std::vector<std::string> image_files;
    
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        return image_files;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        
        if (filename == "." || filename == "..") {
            continue;
        }
        
        std::string full_path = dir_path;
        if (dir_path.back() != '/') {
            full_path += "/";
        }
        full_path += filename;
        
        if (is_regular_file(full_path)) {
            std::string ext = get_file_extension(filename);
            std::string ext_lower = to_lower(ext);
            
            bool valid_ext = false;
            for (size_t i = 0; i < extensions.size(); i++) {
                if (ext_lower == to_lower(extensions[i])) {
                    valid_ext = true;
                    break;
                }
            }
            
            if (valid_ext) {
                image_files.push_back(full_path);
            }
        }
    }
    
    closedir(dir);
    
    return image_files;
}

#endif

// ==================== 构造函数 ====================

ImageLoader::ImageLoader(int channels, int height, int width, 
                         bool norm_01, bool norm_mean_std,
                         float mean, float std, bool auto_compute)
    : target_channels(channels),
      target_height(height),
      target_width(width),
      normalize_to_01(norm_01),
      normalize_mean_std(norm_mean_std),
      mean_val(mean),
      std_val(std),
      auto_mean_std(auto_compute),
      verbose(false) {
    
    channel_means.resize(target_channels, 0.0f);
    channel_stds.resize(target_channels, 1.0f);
}

// ==================== 图像加载函数 ====================

std::vector<float> ImageLoader::load_image(const std::string& filepath, bool verbose) {
    this->verbose = verbose;
    
    try {
        if (!file_exists(filepath)) {
            throw std::runtime_error("File not found: " + filepath);
        }
        
        cimg_library::CImg<unsigned char> img(filepath.c_str());
        
        if (verbose) {
            std::cout << "Loaded image: " << filepath << std::endl;
            std::cout << "Original size: " << img.width() << "x" << img.height() 
                      << "x" << img.spectrum() << std::endl;
        }
        
        return preprocess_image(img, verbose);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading image " << filepath << ": " << e.what() << std::endl;
        return {};
    }
}

std::vector<float> ImageLoader::load_from_cimg(const cimg_library::CImg<unsigned char>& input_img, bool verbose) {
    this->verbose = verbose;
    
    try {
        cimg_library::CImg<unsigned char> img = input_img;
        
        if (verbose) {
            std::cout << "Loading from CImg object" << std::endl;
            std::cout << "Original size: " << img.width() << "x" << img.height() 
                      << "x" << img.spectrum() << std::endl;
        }
        
        if (img.is_empty()) {
            throw std::runtime_error("Input CImg object is empty");
        }
        
        return preprocess_image(img, verbose);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading image from CImg object: " << e.what() << std::endl;
        return {};
    }
}

std::vector<float> ImageLoader::load_from_cimg(cimg_library::CImg<unsigned char>&& input_img, bool verbose) {
    this->verbose = verbose;
    
    try {
        cimg_library::CImg<unsigned char> img = std::move(input_img);
        
        if (verbose) {
            std::cout << "Loading from CImg object (move semantics)" << std::endl;
            std::cout << "Original size: " << img.width() << "x" << img.height() 
                      << "x" << img.spectrum() << std::endl;
        }
        
        if (img.is_empty()) {
            throw std::runtime_error("Input CImg object is empty");
        }
        
        return preprocess_image(img, verbose);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading image from CImg object: " << e.what() << std::endl;
        return {};
    }
}

std::vector<float> ImageLoader::load_batch(const std::vector<std::string>& filepaths, bool verbose) {
    this->verbose = verbose;
    std::vector<float> batch_data;
    int loaded_count = 0;
    
    int single_image_size = target_channels * target_height * target_width;
    batch_data.reserve(filepaths.size() * single_image_size);
    
    for (const auto& filepath : filepaths) {
        auto img_data = load_image(filepath, verbose);
        if (!img_data.empty()) {
            batch_data.insert(batch_data.end(), img_data.begin(), img_data.end());
            loaded_count++;
        }
    }
    
    if (verbose) {
        std::cout << "Successfully loaded " << loaded_count << " images out of " 
                << filepaths.size() << std::endl;
    }
    
    return batch_data;
}

std::vector<float> ImageLoader::load_from_directory(const std::string& dir_path, 
                                                     const std::vector<std::string>& extensions,
                                                     bool verbose) {
    this->verbose = verbose;
    
    std::vector<std::string> image_files;
    
    try {
#ifdef _WIN32
        image_files = list_directory(dir_path, extensions);
#else
        image_files = list_directory_posix(dir_path, extensions);
#endif
        
        if (image_files.empty()) {
            std::cerr << "Warning: No image files found in directory: " << dir_path << std::endl;
            return {};
        }
        
        if (verbose) {
            std::cout << "Found " << image_files.size() << " images in directory: " 
                      << dir_path << std::endl;
        }
        
        return load_batch(image_files, verbose);
        
    } catch (const std::exception& e) {
        std::cerr << "Error reading directory " << dir_path << ": " << e.what() << std::endl;
        return {};
    }
}

// ==================== 统计与工具函数 ====================

void ImageLoader::compute_dataset_statistics(const std::vector<std::string>& filepaths, 
                                              int max_images) {
    std::cout << "Computing dataset statistics..." << std::endl;
    
    int num_images = std::min(static_cast<int>(filepaths.size()), max_images);
    std::vector<std::vector<float>> channel_sums(target_channels, std::vector<float>());
    
    for (int i = 0; i < num_images; i++) {
        auto img_data = load_image(filepaths[i], false);
        if (!img_data.empty()) {
            int pixels_per_channel = target_height * target_width;
            for (int c = 0; c < target_channels; c++) {
                const float* channel_start = img_data.data() + c * pixels_per_channel;
                channel_sums[c].insert(channel_sums[c].end(), 
                                       channel_start, 
                                       channel_start + pixels_per_channel);
            }
        }
        
        if (i % 100 == 0) {
            std::cout << "Processed " << i << " images..." << std::endl;
        }
    }
    
    for (int c = 0; c < target_channels; c++) {
        if (!channel_sums[c].empty()) {
            float sum = std::accumulate(channel_sums[c].begin(), channel_sums[c].end(), 0.0f);
            channel_means[c] = sum / channel_sums[c].size();
            
            float variance = 0.0f;
            for (float val : channel_sums[c]) {
                variance += (val - channel_means[c]) * (val - channel_means[c]);
            }
            variance /= channel_sums[c].size();
            channel_stds[c] = std::sqrt(variance);
            
            std::cout << "Channel " << c << ": mean = " << channel_means[c] 
                      << ", std = " << channel_stds[c] << std::endl;
        }
    }
    
    std::cout << "Statistics computation completed." << std::endl;
}

int ImageLoader::get_batch_size(const std::vector<float>& data) const {
    int single_image_size = target_channels * target_height * target_width;
    if (single_image_size == 0) return 0;
    return data.size() / single_image_size;
}

void ImageLoader::set_normalization_params(const std::vector<float>& means, 
                                            const std::vector<float>& stds) {
    if (means.size() == static_cast<size_t>(target_channels) && 
        stds.size() == static_cast<size_t>(target_channels)) {
        channel_means = means;
        channel_stds = stds;
    } else {
        std::cerr << "Error: Means and stds must have size " << target_channels << std::endl;
    }
}

std::pair<std::vector<float>, std::vector<float>> ImageLoader::get_normalization_params() const {
    return {channel_means, channel_stds};
}

void ImageLoader::display_image_info(const std::vector<float>& image_data, int channels_to_show) const {
    std::cout << "Image data info:" << std::endl;
    std::cout << "  Dimensions: " << target_channels << " x " << target_height 
              << " x " << target_width << std::endl;
    std::cout << "  Total values: " << image_data.size() << std::endl;
    
    if (!image_data.empty()) {
        float min_val = *std::min_element(image_data.begin(), image_data.end());
        float max_val = *std::max_element(image_data.begin(), image_data.end());
        float sum_val = std::accumulate(image_data.begin(), image_data.end(), 0.0f);
        float mean_val = sum_val / image_data.size();
        
        std::cout << "  Min value: " << min_val << std::endl;
        std::cout << "  Max value: " << max_val << std::endl;
        std::cout << "  Mean value: " << mean_val << std::endl;
        
        int pixels_per_channel = target_height * target_width;
        for (int c = 0; c < std::min(target_channels, channels_to_show); c++) {
            std::cout << "  Channel " << c << " (first 5 pixels): ";
            for (int i = 0; i < std::min(5, pixels_per_channel); i++) {
                std::cout << image_data[c * pixels_per_channel + i] << " ";
            }
            std::cout << std::endl;
        }
    }
}

// ==================== 预处理函数 ====================

std::vector<float> ImageLoader::preprocess_image(cimg_library::CImg<unsigned char>& img, bool verbose) {
    adjust_channels(img);
    resize_image(img);
    std::vector<float> float_data = convert_to_float(img);
    std::vector<float> reordered_data = reorder_dimensions(float_data, img);
    
    if (normalize_to_01) {
        normalize_0_1(reordered_data);
    }
    
    if (normalize_mean_std) {
        normalize_mean_std_channels(reordered_data);
    }
    
    if (verbose) {
        std::cout << "Processed image size: " << target_channels << "x" 
                  << target_height << "x" << target_width << std::endl;
    }
    
    return reordered_data;
}

void ImageLoader::adjust_channels(cimg_library::CImg<unsigned char>& img) {
    int current_channels = img.spectrum();
    
    if (current_channels == 1 && target_channels == 3) {
        cimg_library::CImg<unsigned char> rgb_img(img.width(), img.height(), img.depth(), 3);
        cimg_forXYZ(img, x, y, z) {
            rgb_img(x, y, z, 0) = img(x, y, z, 0);  // R
            rgb_img(x, y, z, 1) = img(x, y, z, 0);  // G
            rgb_img(x, y, z, 2) = img(x, y, z, 0);  // B
        }
        img = rgb_img;
        
    } else if (current_channels == 3 && target_channels == 1) {
        img = img.get_channel(0) * 0.299f + 
              img.get_channel(1) * 0.587f + 
              img.get_channel(2) * 0.114f;
              
    } else if (current_channels == 4 && target_channels == 3) {
        img.channels(0, 2);
        
    } else if (current_channels > target_channels) {
        img.channels(0, target_channels - 1);
        
    } else if (current_channels < target_channels) {
        int channels_to_add = target_channels - current_channels;
        for (int i = 0; i < channels_to_add; i++) {
            img.append(img.get_channel(current_channels - 1), 'c');
        }
    }
}

void ImageLoader::resize_image(cimg_library::CImg<unsigned char>& img) {
    if (img.width() != target_width || img.height() != target_height) {
        if (verbose) {
            std::cout << "Resizing from " << img.width() << "x" << img.height() 
                      << " to " << target_width << "x" << target_height << std::endl;
        }
        img.resize(target_width, target_height, -100, -100, 3);
    }
}

std::vector<float> ImageLoader::convert_to_float(const cimg_library::CImg<unsigned char>& img) {
    std::vector<float> float_data;
    float_data.reserve(img.width() * img.height() * img.spectrum());
    
    cimg_forC(img, c) {
        cimg_forXY(img, x, y) {
            float_data.push_back(static_cast<float>(img(x, y, 0, c)));
        }
    }
    
    return float_data;
}

std::vector<float> ImageLoader::reorder_dimensions(const std::vector<float>& data, 
                                                    const cimg_library::CImg<unsigned char>& img) {
    int channels = img.spectrum();
    int height = img.height();
    int width = img.width();
    
    std::vector<float> reordered(channels * height * width);
    
    for (int c = 0; c < channels; c++) {
        for (int h = 0; h < height; h++) {
            for (int w = 0; w < width; w++) {
                int src_idx = c * (height * width) + h * width + w;
                int dst_idx = c * (height * width) + h * width + w;
                reordered[dst_idx] = data[src_idx];
            }
        }
    }
    
    return reordered;
}

void ImageLoader::normalize_0_1(std::vector<float>& data) {
    for (auto& val : data) {
        val /= 255.0f;
    }
}

void ImageLoader::normalize_mean_std_channels(std::vector<float>& data) {
    int pixels_per_channel = target_height * target_width;
    
    for (int c = 0; c < target_channels; c++) {
        float mean = auto_mean_std ? channel_means[c] : mean_val;
        float std = auto_mean_std ? channel_stds[c] : std_val;
        
        for (int i = 0; i < pixels_per_channel; i++) {
            int idx = c * pixels_per_channel + i;
            data[idx] = (data[idx] - mean) / std;
        }
    }
}