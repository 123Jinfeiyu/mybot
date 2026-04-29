#ifndef IMAGE_LOADER_H
#define IMAGE_LOADER_H

#include <vector>
#include <string>
#include <utility>
#include <sys/stat.h>

// Windows 兼容性处理
#ifdef _WIN32

    // 防止 Windows.h 定义 min/max 宏
    #define NOMINMAX

    // Windows 没有 dirent.h，需要手动定义或使用替代方案
    #include <windows.h>

    // 链接必要的 Windows 库
    #pragma comment(lib, "shell32.lib")

#else
    #include <dirent.h>
#endif

#define cimg_display 0
#include "CImg.h"

class ImageLoader {
private:
    // 图像参数
    int target_channels;     // 目标通道数（如RGB=3, 灰度=1）
    int target_height;       // 目标高度
    int target_width;        // 目标宽度
    bool normalize_to_01;    // 是否归一化到[0,1]
    bool normalize_mean_std; // 是否使用均值标准差归一化
    float mean_val;          // 均值（归一化用）
    float std_val;           // 标准差（归一化用）
    bool auto_mean_std;      // 是否自动计算均值标准差
    
    // 预计算的均值和标准差（针对每个通道）
    std::vector<float> channel_means;
    std::vector<float> channel_stds;
    
    // 用于调试的verbose标志
    bool verbose;
    
    // 辅助函数：检查文件是否存在
    bool file_exists(const std::string& filename);
    
    // 辅助函数：检查是否为普通文件
    bool is_regular_file(const std::string& path);
    
    // 辅助函数：获取文件扩展名
    std::string get_file_extension(const std::string& filename);
    
    // 辅助函数：转换为小写
    std::string to_lower(const std::string& str);
    
    // Windows 平台目录遍历辅助函数
#ifdef _WIN32
    std::vector<std::string> list_directory(const std::string& dir_path, 
                                            const std::vector<std::string>& extensions);
#else
    std::vector<std::string> list_directory_posix(const std::string& dir_path, 
                                                   const std::vector<std::string>& extensions);
#endif
    
    // 图像预处理主函数
    std::vector<float> preprocess_image(cimg_library::CImg<unsigned char>& img, bool verbose = false);
    
    // 调整通道数
    void adjust_channels(cimg_library::CImg<unsigned char>& img);
    
    // 调整图像尺寸
    void resize_image(cimg_library::CImg<unsigned char>& img);
    
    // 转换为浮点数
    std::vector<float> convert_to_float(const cimg_library::CImg<unsigned char>& img);
    
    // 重新排列维度为 [channels, height, width]
    std::vector<float> reorder_dimensions(const std::vector<float>& data, 
                                          const cimg_library::CImg<unsigned char>& img);
    
    // 归一化到[0,1]
    void normalize_0_1(std::vector<float>& data);
    
    // 使用均值标准差归一化（逐通道）
    void normalize_mean_std_channels(std::vector<float>& data);
    
public:
    // 构造函数
    ImageLoader(int channels = 3, int height = 224, int width = 224, 
                bool norm_01 = true, bool norm_mean_std = false,
                float mean = 0.0f, float std = 1.0f, bool auto_compute = false);
    
    // 从文件加载单张图片
    std::vector<float> load_image(const std::string& filepath, bool verbose = false);
    
    // 从CImg对象加载图片
    std::vector<float> load_from_cimg(const cimg_library::CImg<unsigned char>& input_img, bool verbose = false);
    
    // 从CImg对象加载图片（移动语义版本）
    std::vector<float> load_from_cimg(cimg_library::CImg<unsigned char>&& input_img, bool verbose = false);
    
    // 批量加载图片
    std::vector<float> load_batch(const std::vector<std::string>& filepaths, bool verbose = false);
    
    // 从文件夹加载所有图片
    std::vector<float> load_from_directory(const std::string& dir_path, 
                                           const std::vector<std::string>& extensions = {".jpg", ".jpeg", ".png", ".bmp"},
                                           bool verbose = false);
    
    // 计算数据集统计量（用于归一化）
    void compute_dataset_statistics(const std::vector<std::string>& filepaths, 
                                    int max_images = 1000);
    
    // 获取批次数（根据总像素数计算）
    int get_batch_size(const std::vector<float>& data) const;
    
    // 设置归一化参数
    void set_normalization_params(const std::vector<float>& means, 
                                  const std::vector<float>& stds);
    
    // 获取归一化参数
    std::pair<std::vector<float>, std::vector<float>> get_normalization_params() const;
    
    // 显示图像信息（用于调试）
    void display_image_info(const std::vector<float>& image_data, int channels_to_show = 3) const;
    
    // 获取目标尺寸（公开访问器）
    int get_target_height() const { return target_height; }
    int get_target_width() const { return target_width; }
    int get_target_channels() const { return target_channels; }
    
    // 设置verbose模式
    void set_verbose(bool v) { verbose = v; }
};

#endif // IMAGE_LOADER_H