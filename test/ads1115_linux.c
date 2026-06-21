/**
 * ads1115_linux.c
 *
 * Linux C 通过 I2C 采集 ADS1115 示例
 * ADS1115: TI 16位 4通道 ΔΣ ADC, 最高 860SPS, I2C 接口
 *
 * 编译: gcc -o ads1115 ads1115_linux.c
 * 运行: ./ads1115
 *       ./ads1115 0x48          # 指定 I2C 地址
 *       ./ads1115 0x48 1 2      # 指定地址、PGA、数据速率
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <time.h>
#include <math.h>

// ============================================================
// I2C 配置
// ============================================================
#define I2C_DEVICE      "/dev/i2c-1"    // I2C 总线 (树莓派是 i2c-1)
#define ADS1115_ADDR    0x48             // ADDR 接 GND 时默认地址

// ============================================================
// ADS1115 寄存器
// ============================================================
#define ADS1115_REG_CONVERSION    0x00   // 转换结果 (只读)
#define ADS1115_REG_CONFIG        0x01   // 配置寄存器 (读/写)
#define ADS1115_REG_LO_THRESH     0x02   // 低阈值比较
#define ADS1115_REG_HI_THRESH     0x03   // 高阈值比较

// ============================================================
// ADS1115 配置寄存器 bit 定义 (16 位)
// ============================================================
// BIT15:     OS[0]   — 单次转换启动位 (1=开始)
// BIT14-12:  MUX[2:0] — 输入多路选择
// BIT11-9:   PGA[2:0] — 可编程增益放大器
// BIT8:      MODE     — 0=连续转换, 1=单次(省电)
// BIT7-5:    DR[2:0]  — 数据速率
// BIT4:      COMP_MODE — 比较模式 (0=传统)
// BIT3:      COMP_POL  — 比较极性 (0=低有效)
// BIT2:      COMP_LAT  — 比较锁存 (0=不锁存)
// BIT1-0:    COMP_QUE  — 比较器队列 (11=禁用)

// --- MUX 输入通道 ---
#define ADS1115_MUX_AIN0_AIN1     0x0000  // 差分: P=AIN0, N=AIN1 (默认)
#define ADS1115_MUX_AIN0_AIN3     0x1000  // 差分: P=AIN0, N=AIN3
#define ADS1115_MUX_AIN1_AIN3     0x2000  // 差分: P=AIN1, N=AIN3
#define ADS1115_MUX_AIN2_AIN3     0x3000  // 差分: P=AIN2, N=AIN3
#define ADS1115_MUX_AIN0_GND      0x4000  // 单端: AIN0
#define ADS1115_MUX_AIN1_GND      0x5000  // 单端: AIN1
#define ADS1115_MUX_AIN2_GND      0x6000  // 单端: AIN2
#define ADS1115_MUX_AIN3_GND      0x7000  // 单端: AIN3

// --- PGA 增益 (满量程范围) ---
#define ADS1115_PGA_6_144V        0x0000  // ±6.144V  (1LSB=0.1875mV)
#define ADS1115_PGA_4_096V        0x0200  // ±4.096V  (1LSB=0.125mV)
#define ADS1115_PGA_2_048V        0x0400  // ±2.048V  (1LSB=0.0625mV) (默认)
#define ADS1115_PGA_1_024V        0x0600  // ±1.024V  (1LSB=0.03125mV)
#define ADS1115_PGA_0_512V        0x0800  // ±0.512V  (1LSB=0.015625mV)
#define ADS1115_PGA_0_256V        0x0A00  // ±0.256V  (1LSB=0.0078125mV)

// --- 数据速率 ---
#define ADS1115_DR_8SPS           0x0000  // 8 SPS
#define ADS1115_DR_16SPS          0x0020  // 16 SPS
#define ADS1115_DR_32SPS          0x0040  // 32 SPS
#define ADS1115_DR_64SPS          0x0060  // 64 SPS
#define ADS1115_DR_128SPS         0x0080  // 128 SPS (默认)
#define ADS1115_DR_250SPS         0x00A0  // 250 SPS
#define ADS1115_DR_475SPS         0x00C0  // 475 SPS
#define ADS1115_DR_860SPS         0x00E0  // 860 SPS

// --- 比较器队列 (COMP_QUE) ---
#define ADS1115_COMP_QUE_1        0x0000  // 1 次后触发
#define ADS1115_COMP_QUE_2        0x0001  // 2 次后触发
#define ADS1115_COMP_QUE_4        0x0002  // 4 次后触发
#define ADS1115_COMP_QUE_DISABLE  0x0003  // 禁用比较器 (默认)

// --- 操作模式 ---
#define ADS1115_MODE_CONTINUOUS   0x0000  // 连续转换
#define ADS1115_MODE_SINGLE       0x0100  // 单次转换 (默认, 省电)

// --- 启动转换 ---
#define ADS1115_OS_START          0x8000  // 启动单次转换

// ============================================================
// 结构体: ADS1115 配置上下文
// ============================================================
typedef struct {
    int      fd;              // I2C 文件描述符
    uint8_t  addr;            // I2C 设备地址
    uint16_t config;          // 当前配置值
    double   fs_voltage;      // 满量程电压 (取决于 PGA)
} ads1115_t;

// ============================================================
// PGA 对应的满量程电压 (单位: V)
// ============================================================
static const double pga_fs[] = {
    [0] = 6.144,   // ADS1115_PGA_6_144V
    [1] = 4.096,   // ADS1115_PGA_4_096V
    [2] = 2.048,   // ADS1115_PGA_2_048V
    [3] = 1.024,   // ADS1115_PGA_1_024V
    [4] = 0.512,   // ADS1115_PGA_0_512V
    [5] = 0.256,   // ADS1115_PGA_0_256V
};

// ============================================================
// PGA 增益对应的满量程电压名
// ============================================================
static const char *pga_names[] = {
    [0] = "±6.144V",
    [1] = "±4.096V",
    [2] = "±2.048V",
    [3] = "±1.024V",
    [4] = "±0.512V",
    [5] = "±0.256V",
};

// ============================================================
// 数据速率字符串
// ============================================================
static const char *dr_names[] = {
    [0] = "8 SPS",
    [1] = "16 SPS",
    [2] = "32 SPS",
    [3] = "64 SPS",
    [4] = "128 SPS",
    [5] = "250 SPS",
    [6] = "475 SPS",
    [7] = "860 SPS",
};

// ============================================================
// 毫秒延时
// ============================================================
static void delay_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ============================================================
// 微秒延时
// ============================================================
static void delay_us(int us)
{
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
}

// ============================================================
// I2C 初始化
// ============================================================
static int i2c_init(ads1115_t *dev, const char *i2c_dev, uint8_t addr)
{
    dev->fd = open(i2c_dev, O_RDWR);
    if (dev->fd < 0) {
        perror("打开 I2C 设备失败");
        return -1;
    }

    if (ioctl(dev->fd, I2C_SLAVE, addr) < 0) {
        perror("设置 I2C 从机地址失败");
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }

    dev->addr  = addr;
    dev->config = 0;
    printf("I2C 初始化完成: 设备=%s, 地址=0x%02X\n", i2c_dev, addr);
    return 0;
}

// ============================================================
// I2C 写 2 字节 (大端)
// ============================================================
static int i2c_write_word(ads1115_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;   // 高字节
    buf[2] = value & 0xFF;          // 低字节

    if (write(dev->fd, buf, 3) != 3) {
        perror("I2C 写失败");
        return -1;
    }
    return 0;
}

// ============================================================
// I2C 读 2 字节 (大端)
// ============================================================
static int i2c_read_word(ads1115_t *dev, uint8_t reg, uint16_t *value)
{
    // 先写入寄存器地址
    if (write(dev->fd, &reg, 1) != 1) {
        perror("I2C 设置寄存器地址失败");
        return -1;
    }

    // 读取 2 字节
    uint8_t buf[2];
    if (read(dev->fd, buf, 2) != 2) {
        perror("I2C 读失败");
        return -1;
    }

    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return 0;
}

// ============================================================
// 写配置寄存器
// ============================================================
static int ads1115_write_config(ads1115_t *dev, uint16_t config)
{
    dev->config = config;
    return i2c_write_word(dev, ADS1115_REG_CONFIG, config);
}

// ============================================================
// 读配置寄存器
// ============================================================
static int ads1115_read_config(ads1115_t *dev, uint16_t *config)
{
    return i2c_read_word(dev, ADS1115_REG_CONFIG, config);
}

// ============================================================
// 读转换结果寄存器
// ============================================================
static int ads1115_read_conversion(ads1115_t *dev, int16_t *value)
{
    uint16_t raw;
    int ret = i2c_read_word(dev, ADS1115_REG_CONVERSION, &raw);
    if (ret == 0) {
        *value = (int16_t)raw;
    }
    return ret;
}

// ============================================================
// 读阈值寄存器
// ============================================================
static int ads1115_read_thresh(ads1115_t *dev, int16_t *lo, int16_t *hi)
{
    uint16_t raw_lo, raw_hi;
    if (i2c_read_word(dev, ADS1115_REG_LO_THRESH, &raw_lo) < 0) return -1;
    if (i2c_read_word(dev, ADS1115_REG_HI_THRESH, &raw_hi) < 0) return -1;
    *lo = (int16_t)raw_lo;
    *hi = (int16_t)raw_hi;
    return 0;
}

// ============================================================
// 写阈值寄存器
// ============================================================
static int ads1115_write_thresh(ads1115_t *dev, int16_t lo, int16_t hi)
{
    if (i2c_write_word(dev, ADS1115_REG_LO_THRESH, (uint16_t)lo) < 0) return -1;
    if (i2c_write_word(dev, ADS1115_REG_HI_THRESH, (uint16_t)hi) < 0) return -1;
    return 0;
}

// ============================================================
// 初始化 ADS1115
//
// pga_idx: 0~5 对应 ADS1115_PGA_xxx
// dr_idx:  0~7 对应 ADS1115_DR_xxx
// ============================================================
static int ads1115_init(ads1115_t *dev, int pga_idx, int dr_idx)
{
    if (pga_idx < 0 || pga_idx > 5) pga_idx = 2;  // 默认 ±2.048V
    if (dr_idx  < 0 || dr_idx  > 7) dr_idx  = 4;  // 默认 128SPS

    dev->fs_voltage = pga_fs[pga_idx];

    // 构建配置:
    //   - 单次转换模式
    //   - 禁用比较器
    //   - 默认通道 AIN0-GND
    uint16_t config = ADS1115_MUX_AIN0_GND      // 单端 AIN0
                    | (pga_idx << 9)            // PGA
                    | ADS1115_MODE_SINGLE        // 单次模式
                    | (dr_idx << 5)             // 数据速率
                    | ADS1115_COMP_QUE_DISABLE; // 禁用比较器

    int ret = ads1115_write_config(dev, config);
    if (ret < 0) return ret;

    printf("ADS1115 初始化完成: ");
    printf("PGA=%s, 速率=%s, 满量程=%.3fV\n",
           pga_names[pga_idx], dr_names[dr_idx], dev->fs_voltage);
    return 0;
}

// ============================================================
// 读取单端通道 (单次转换)
// ch: 0~3 对应 AIN0~AIN3
// ============================================================
static double ads1115_read_single(ads1115_t *dev, int ch)
{
    if (ch < 0 || ch > 3) {
        fprintf(stderr, "通道号无效: %d (有效: 0~3)\n", ch);
        return 0.0;
    }

    // 1. 重写配置: 设置通道 + 启动转换
    uint16_t mux = ADS1115_MUX_AIN0_GND | (ch << 12);  // AINch-GND
    uint16_t config = (dev->config & ~0x7000)  // 清除 MUX 位
                    | mux
                    | ADS1115_OS_START;        // 启动单次转换

    ads1115_write_config(dev, config);

    // 2. 等待转换完成 (OS 位被硬件清 0)
    //    根据数据速率等待足够时间
    uint16_t dr_idx = (config >> 5) & 0x07;
    int wait_ms = 1000 / (8 << dr_idx) + 1;  // 保守估计
    delay_ms(wait_ms + 2);

    // 3. 读转换值
    int16_t raw = 0;
    if (ads1115_read_conversion(dev, &raw) < 0) {
        return 0.0;
    }

    // 4. 转换为电压
    //    ADS1115 16位有符号, 满量程 = FS_VOLTAGE
    //    V = raw * FS_VOLTAGE / 32768
    double voltage = (double)raw * dev->fs_voltage / 32768.0;
    return voltage;
}

// ============================================================
// 读取差分通道 (单次转换)
// pos: 正极通道 (0~3)
// neg: 负极通道 (0~3)
// ============================================================
static double ads1115_read_diff(ads1115_t *dev, int pos, int neg)
{
    if (pos < 0 || pos > 3 || neg < 0 || neg > 3) {
        fprintf(stderr, "通道号无效\n");
        return 0.0;
    }

    // 构建 MUX 值 (差分)
    // AIN0-AIN1: 0, AIN0-AIN3: 1, AIN1-AIN3: 2, AIN2-AIN3: 3
    uint16_t mux_val;
    if      (pos == 0 && neg == 1) mux_val = 0;
    else if (pos == 0 && neg == 3) mux_val = 1;
    else if (pos == 1 && neg == 3) mux_val = 2;
    else if (pos == 2 && neg == 3) mux_val = 3;
    else {
        fprintf(stderr, "不支持的差分组合: AIN%d - AIN%d\n", pos, neg);
        fprintf(stderr, "支持: 0-1, 0-3, 1-3, 2-3\n");
        return 0.0;
    }

    uint16_t mux = mux_val << 12;
    uint16_t config = (dev->config & ~0x7000)
                    | mux
                    | ADS1115_OS_START;

    ads1115_write_config(dev, config);

    uint16_t dr_idx = (config >> 5) & 0x07;
    int wait_ms = 1000 / (8 << dr_idx) + 1;
    delay_ms(wait_ms + 2);

    int16_t raw = 0;
    if (ads1115_read_conversion(dev, &raw) < 0) {
        return 0.0;
    }

    double voltage = (double)raw * dev->fs_voltage / 32768.0;
    return voltage;
}

// ============================================================
// 连续模式读数 (需先配置为连续模式)
// ============================================================
static int ads1115_start_continuous(ads1115_t *dev, int ch)
{
    if (ch < 0 || ch > 3) return -1;

    uint16_t config = (dev->config & ~0x7100)    // 清除 MUX+MODE 位
                    | (ADS1115_MUX_AIN0_GND | (ch << 12))  // 通道
                    | ADS1115_MODE_CONTINUOUS    // 连续模式
                    | ADS1115_COMP_QUE_DISABLE;

    return ads1115_write_config(dev, config);
}

// ============================================================
// 连续模式读数
// ============================================================
static double ads1115_read_continuous(ads1115_t *dev)
{
    int16_t raw = 0;
    if (ads1115_read_conversion(dev, &raw) < 0) {
        return 0.0;
    }
    return (double)raw * dev->fs_voltage / 32768.0;
}

// ============================================================
// 扫描所有单端通道
// ============================================================
static void ads1115_scan_all(ads1115_t *dev)
{
    printf("\n===== 单端通道扫描 =====\n");
    const char *ch_names[] = {"AIN0", "AIN1", "AIN2", "AIN3"};
    for (int ch = 0; ch < 4; ch++) {
        double v = ads1115_read_single(dev, ch);
        printf("  %s: %+.6f V\n", ch_names[ch], v);
    }
}

// ============================================================
// 打印当前配置
// ============================================================
static void ads1115_print_config(ads1115_t *dev)
{
    uint16_t config;
    if (ads1115_read_config(dev, &config) < 0) return;

    int mux   = (config >> 12) & 0x07;
    int pga   = (config >> 9)  & 0x07;
    int mode  = (config >> 8)  & 0x01;
    int dr    = (config >> 5)  & 0x07;
    int comp  = config & 0x03;

    printf("\n===== ADS1115 寄存器 =====\n");
    printf("  配置寄存器: 0x%04X\n", config);
    printf("  MUX: %d (", mux);
    switch (mux) {
        case 0: printf("AIN0-AIN1"); break;
        case 1: printf("AIN0-AIN3"); break;
        case 2: printf("AIN1-AIN3"); break;
        case 3: printf("AIN2-AIN3"); break;
        case 4: printf("AIN0-GND"); break;
        case 5: printf("AIN1-GND"); break;
        case 6: printf("AIN2-GND"); break;
        case 7: printf("AIN3-GND"); break;
    }
    printf(")\n");
    printf("  PGA: %d -> %s (满量程 %.3fV)\n", pga,
           (pga <= 5) ? pga_names[pga] : "未知",
           (pga <= 5) ? pga_fs[pga] : 0);
    printf("  模式: %s\n", mode ? "单次" : "连续");
    printf("  速率: %d -> %s\n", dr, dr_names[dr & 0x07]);
    printf("  比较器: %s\n", comp == 3 ? "禁用" : "启用");

    // 读阈值
    int16_t lo, hi;
    if (ads1115_read_thresh(dev, &lo, &hi) == 0) {
        printf("  阈值: LO=%d (%.6fV), HI=%d (%.6fV)\n",
               lo, (double)lo * dev->fs_voltage / 32768.0,
               hi, (double)hi * dev->fs_voltage / 32768.0);
    }
}

// ============================================================
// 打印转换结果
// ============================================================
static void print_reading(const char *label, double voltage)
{
    printf("  %s: %+.6f V  (%.3f mV)\n", label, voltage, voltage * 1000.0);
}

// ============================================================
// 打印帮助
// ============================================================
static void print_help(const char *prog)
{
    printf("\n用法: %s [I2C地址] [PGA] [数据速率]\n", prog);
    printf("\n参数 (可选):\n");
    printf("  I2C地址    0x48~0x4B (默认 0x48)\n");
    printf("  PGA        0~5 (默认 2 = ±2.048V)\n");
    printf("              0=±6.144V, 1=±4.096V, 2=±2.048V\n");
    printf("              3=±1.024V, 4=±0.512V, 5=±0.256V\n");
    printf("  数据速率   0~7 (默认 4 = 128SPS)\n");
    printf("              0=8, 1=16, 2=32, 3=64\n");
    printf("              4=128, 5=250, 6=475, 7=860 (SPS)\n");
    printf("\n示例:\n");
    printf("  %s\n", prog);
    printf("  %s 0x49\n", prog);
    printf("  %s 0x48 2 6\n", prog);
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char *argv[])
{
    ads1115_t dev;
    uint8_t   addr  = ADS1115_ADDR;
    int       pga   = 2;   // ±2.048V
    int       dr    = 4;   // 128 SPS

    // 解析命令行参数
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
        addr = (uint8_t)strtol(argv[1], NULL, 0);
    }
    if (argc >= 3) pga  = atoi(argv[2]);
    if (argc >= 4) dr   = atoi(argv[3]);

    printf("ADS1115 Linux I2C 采集示例\n");
    printf("============================\n\n");

    // 1. 初始化 I2C
    if (i2c_init(&dev, I2C_DEVICE, addr) < 0) {
        return 1;
    }

    // 2. 初始化 ADS1115
    if (ads1115_init(&dev, pga, dr) < 0) {
        close(dev.fd);
        return 1;
    }

    // 3. 单通道测试 (AIN0)
    printf("\n===== 单通道测试 (AIN0, 连续读 10 次) =====\n");
    for (int i = 0; i < 10; i++) {
        double v = ads1115_read_single(&dev, 0);
        char label[32];
        snprintf(label, sizeof(label), "[%d]", i);
        print_reading(label, v);
        delay_ms(10);
    }

    // 4. 扫描所有单端通道
    ads1115_scan_all(&dev);

    // 5. 差分模式示例
    printf("\n===== 差分测试 (AIN0 - AIN1) =====\n");
    for (int i = 0; i < 5; i++) {
        double v = ads1115_read_diff(&dev, 0, 1);
        char label[32];
        snprintf(label, sizeof(label), "[%d]", i);
        print_reading(label, v);
        delay_ms(10);
    }

    // 6. 连续模式测试
    printf("\n===== 连续模式测试 (AIN0, 5 次) =====\n");
    if (ads1115_start_continuous(&dev, 0) == 0) {
        delay_ms(50);  // 等第一次转换完成
        for (int i = 0; i < 5; i++) {
            double v = ads1115_read_continuous(&dev);
            char label[32];
            snprintf(label, sizeof(label), "[%d] 连续", i);
            print_reading(label, v);
            delay_ms(10);
        }
    } else {
        printf("  启动连续模式失败\n");
    }

    // 7. 打印配置信息
    ads1115_print_config(&dev);

    // 8. 清理
    close(dev.fd);
    printf("\n程序结束.\n");

    return 0;
}
