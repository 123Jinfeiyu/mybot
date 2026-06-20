/**
 * ads1256_linux.c
 * 
 * Linux C 通过 SPI 采集 ADS1256 示例
 * 适用平台: Raspberry Pi / 任何 Linux + SPI 设备
 * 编译: gcc -o ads1256 ads1256_linux.c -lwiringPi  (树莓派用 wiringPi)
 *      gcc -o ads1256 ads1256_linux.c               (通用 /dev/spidev)
 * 
 * ADS1256: TI 24位 8通道 ΔΣ ADC, 最高 30kSPS
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <time.h>

// ============================================================
// 配置参数 - 按需修改
// ============================================================
#define SPI_DEVICE      "/dev/spidev0.0"   // SPI 设备路径
#define SPI_SPEED       1920000            // SPI 时钟 (1.92MHz, ADS1256 最大支持 2MHz)
#define SPI_MODE        1                  // SPI 模式: CPOL=0, CPHA=1 (模式1)
#define SPI_BITS        8                  // 8 位传输

// ADS1256 片选用 GPIO (如果使用硬件 CS 则不需要)
// 若用 /dev/spidev 的硬件 CS, 这部分不需要手动控制
// #define CS_PIN          8               // 例如 wiringPi 的 GPIO 8

// ============================================================
// ADS1256 寄存器地址
// ============================================================
#define ADS1256_REG_STATUS     0x00
#define ADS1256_REG_MUX        0x01
#define ADS1256_REG_ADCON      0x02
#define ADS1256_REG_DRATE      0x03
#define ADS1256_REG_IO         0x04
#define ADS1256_REG_OFC0       0x05
#define ADS1256_REG_OFC1       0x06
#define ADS1256_REG_OFC2       0x07
#define ADS1256_REG_FSC0       0x08
#define ADS1256_REG_FSC1       0x09
#define ADS1256_REG_FSC2       0x0A

// ============================================================
// ADS1256 命令定义
// ============================================================
#define ADS1256_CMD_WAKEUP     0x00    // 退出待机
#define ADS1256_CMD_RDATA      0x01    // 读数据
#define ADS1256_CMD_RDATAC     0x03    // 连续读数据
#define ADS1256_CMD_SDATAC     0x0F    // 停止连续读
#define ADS1256_CMD_RREG       0x10    // 读寄存器 (| 寄存器地址)
#define ADS1256_CMD_WREG       0x50    // 写寄存器 (| 寄存器地址)
#define ADS1256_CMD_SELFCAL    0xF0    // 自校准
#define ADS1256_CMD_SELFOCAL   0xF1    // 自失调校准
#define ADS1256_CMD_SELFGCAL   0xF2    // 自增益校准
#define ADS1256_CMD_SYSOCAL    0xF3    // 系统失调校准
#define ADS1256_CMD_SYSGCAL    0xF4    // 系统增益校准
#define ADS1256_CMD_SYNC       0xFC    // 同步
#define ADS1256_CMD_STANDBY    0xFD    // 待机
#define ADS1256_CMD_RESET      0xFE    // 复位

// ============================================================
// ADS1256 数据速率 (DRATE 寄存器值)
// ============================================================
#define ADS1256_DRATE_30000    0xF0    // 30,000 SPS
#define ADS1256_DRATE_15000    0xE0    // 15,000 SPS
#define ADS1256_DRATE_7500     0xD0    // 7,500 SPS
#define ADS1256_DRATE_3750     0xC0    // 3,750 SPS
#define ADS1256_DRATE_2000     0xB0    // 2,000 SPS
#define ADS1256_DRATE_1000     0xA0    // 1,000 SPS
#define ADS1256_DRATE_500      0x90    // 500 SPS
#define ADS1256_DRATE_100      0x80    // 100 SPS
#define ADS1256_DRATE_60       0x70    // 60 SPS
#define ADS1256_DRATE_50       0x60    // 50 SPS
#define ADS1256_DRATE_30       0x50    // 30 SPS
#define ADS1256_DRATE_25       0x40    // 25 SPS
#define ADS1256_DRATE_15       0x30    // 15 SPS
#define ADS1256_DRATE_10       0x20    // 10 SPS
#define ADS1256_DRATE_5        0x10    // 5 SPS

// ============================================================
// PGA 增益 (ADCON 寄存器的低 3 位)
// ============================================================
#define ADS1256_PGA_1          0x00    // ±5V
#define ADS1256_PGA_2          0x01    // ±2.5V
#define ADS1256_PGA_4          0x02    // ±1.25V
#define ADS1256_PGA_8          0x03    // ±0.625V
#define ADS1256_PGA_16         0x04    // ±0.3125V
#define ADS1256_PGA_32         0x05    // ±0.15625V
#define ADS1256_PGA_64         0x06    // ±0.078125V

// 参考电压 (V)
#define ADS1256_VREF           2.5

// ============================================================
// 全局变量
// ============================================================
static int      spi_fd = -1;            // SPI 文件描述符
static uint8_t  spi_txbuf[32];
static uint8_t  spi_rxbuf[32];

// ============================================================
// SPI 初始化
// ============================================================
int spi_init(const char *device, uint32_t speed, uint8_t mode, uint8_t bits)
{
    int ret;
    
    spi_fd = open(device, O_RDWR);
    if (spi_fd < 0) {
        perror("打开 SPI 设备失败");
        return -1;
    }

    // 设置 SPI 模式
    ret = ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    if (ret < 0) {
        perror("设置 SPI 模式失败");
        close(spi_fd);
        return -1;
    }

    // 设置位数
    ret = ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    if (ret < 0) {
        perror("设置 SPI 位数失败");
        close(spi_fd);
        return -1;
    }

    // 设置速度
    ret = ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    if (ret < 0) {
        perror("设置 SPI 速度失败");
        close(spi_fd);
        return -1;
    }

    // 读取并验证设置
    uint8_t  rb_mode, rb_bits;
    uint32_t rb_speed;
    
    ioctl(spi_fd, SPI_IOC_RD_MODE, &rb_mode);
    ioctl(spi_fd, SPI_IOC_RD_BITS_PER_WORD, &rb_bits);
    ioctl(spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &rb_speed);

    printf("SPI 初始化完成: 模式=%d, 位数=%d, 速度=%d Hz\n", 
           rb_mode, rb_bits, rb_speed);

    return 0;
}

// ============================================================
// SPI 收发 (全双工)
// ============================================================
int spi_transfer(uint8_t *tx, uint8_t *rx, int len)
{
    struct spi_ioc_transfer tr = {
        .tx_buf        = (unsigned long)tx,
        .rx_buf        = (unsigned long)rx,
        .len           = (uint32_t)len,
        .delay_usecs   = 0,
        .speed_hz      = SPI_SPEED,
        .bits_per_word = SPI_BITS,
        .cs_change     = 0,
    };

    int ret = ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 0) {
        perror("SPI 传输失败");
    }
    return ret;
}

// ============================================================
// SPI 发送单字节
// ============================================================
static inline void spi_write_byte(uint8_t byte)
{
    spi_txbuf[0] = byte;
    spi_transfer(spi_txbuf, spi_rxbuf, 1);
}

// ============================================================
// SPI 读单字节
// ============================================================
static inline uint8_t spi_read_byte(void)
{
    spi_txbuf[0] = 0xFF;  // MOSI 发 0xFF 以产生时钟
    spi_transfer(spi_txbuf, spi_rxbuf, 1);
    return spi_rxbuf[0];
}

// ============================================================
// 等待 DRDY 引脚变低 (数据就绪)
// 
// 注意: 这里用延时模拟等待，实际应接 GPIO 中断或轮询
// 若 DRDY 接了 GPIO (如 wiringPi 的 pin 25)，可改为:
//   while(digitalRead(DRDY_PIN));
// ============================================================
void wait_drdy(void)
{
    // 实际项目中用 GPIO 轮询更准确
    // 这里用延时估算
    usleep(100);  // 100us，根据采样率调整
}

// ============================================================
// ADS1256 复位
// ============================================================
void ads1256_reset(void)
{
    spi_write_byte(ADS1256_CMD_RESET);
    usleep(1000);          // 等待复位完成
    spi_write_byte(ADS1256_CMD_WAKEUP);  // 退出待机
    usleep(1000);
}

// ============================================================
// 写 ADS1256 寄存器
// ============================================================
void ads1256_write_reg(uint8_t reg, uint8_t value)
{
    spi_write_byte(ADS1256_CMD_WREG | reg);
    spi_write_byte(0x00);          // 后续字节数 - 1 (0 = 1 byte)
    spi_write_byte(value);
}

// ============================================================
// 读 ADS1256 寄存器
// ============================================================
uint8_t ads1256_read_reg(uint8_t reg)
{
    spi_write_byte(ADS1256_CMD_RREG | reg);
    spi_write_byte(0x00);          // 后续字节数 - 1
    return spi_read_byte();
}

// ============================================================
// 设置输入通道 (MUX 寄存器)
//
// mux: 
//   单端模式: POS = channel, NEG = 0x08 (AINCOM)
//   差分模式: POS = channel_p, NEG = channel_n
// ============================================================
void ads1256_set_channel(uint8_t pos, uint8_t neg)
{
    uint8_t mux = ((pos & 0x07) << 4) | (neg & 0x07);
    ads1256_write_reg(ADS1256_REG_MUX, mux);
    usleep(1);
}

// ============================================================
// ADS1256 初始化
// ============================================================
int ads1256_init(void)
{
    printf("正在初始化 ADS1256...\n");

    // 1. 复位 ADS1256
    ads1256_reset();
    usleep(10000);

    // 2. 配置状态寄存器
    //    BIT7: ID (只读)
    //    BIT6: ID (只读)
    //    BIT5: ORDER = 0 (MSB 优先)
    //    BIT4: ACAL  = 1 (自动校准)
    //    BIT3: BUFEN = 0 (关闭缓冲)
    //    BIT2: DRB   = 0 (DRDY 低有效)
    //    BIT1-0: 保留
    ads1256_write_reg(ADS1256_REG_STATUS, 0x14);  // 0b00010100

    // 3. 配置 MUX (默认: AIN0 + AINCOM)
    ads1256_set_channel(0, 0x08);

    // 4. 配置 ADCON (A/D 控制寄存器)
    //    BIT7-5: CLK1-0 = 00 (时钟输出关), SDCS = 0
    //    BIT4-3: 保留
    //    BIT2-0: PGA = 001 (增益 ×2)
    ads1256_write_reg(ADS1256_REG_ADCON, ADS1256_PGA_2);

    // 5. 设置数据速率 (1000 SPS)
    ads1256_write_reg(ADS1256_REG_DRATE, ADS1256_DRATE_1000);

    // 6. 自校准
    printf("正在进行自校准...\n");
    spi_write_byte(ADS1256_CMD_SELFCAL);
    
    // 等待校准完成 (约 400ms @ 1000SPS)
    usleep(500000);
    
    printf("ADS1256 初始化完成!\n");

    // 读寄存器验证
    uint8_t status = ads1256_read_reg(ADS1256_REG_STATUS);
    uint8_t mux    = ads1256_read_reg(ADS1256_REG_MUX);
    uint8_t adcon  = ads1256_read_reg(ADS1256_REG_ADCON);
    uint8_t drate  = ads1256_read_reg(ADS1256_REG_DRATE);
    
    printf("寄存器状态: STATUS=0x%02X, MUX=0x%02X, ADCON=0x%02X, DRATE=0x%02X\n",
           status, mux, adcon, drate);

    return 0;
}

// ============================================================
// 单次读取 24 位数据
//
// 返回: 24 位有符号整数 (二进制补码)
// ============================================================
int32_t ads1256_read_data(void)
{
    uint8_t buf[3];

    // 发送 RDATA 命令
    wait_drdy();
    spi_write_byte(ADS1256_CMD_RDATA);

    // 读 3 字节 (24 位)
    buf[0] = spi_read_byte();
    buf[1] = spi_read_byte();
    buf[2] = spi_read_byte();

    // 组合成 32 位有符号数 (符号扩展)
    int32_t value;
    value = ((int32_t)buf[0] << 16) | 
            ((int32_t)buf[1] << 8)  | 
            ((int32_t)buf[2]);

    // 24 位有符号数符号扩展
    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    return value;
}

// ============================================================
// 将原始值转换为电压
// ============================================================
double ads1256_raw_to_voltage(int32_t raw, uint8_t pga)
{
    // ADS1256 满量程 = ±VREF / PGA
    // 24 位: -2^23 ~ 2^23-1
    double voltage = (double)raw * ADS1256_VREF / (double)(1 << 23) / (double)pga;
    return voltage;
}

// ============================================================
// 读取指定单端通道
// ============================================================
int32_t ads1256_read_channel(uint8_t ch)
{
    ads1256_set_channel(ch, 0x08);  // AINch vs AINCOM
    usleep(1);

    // 发送 SYNC + WAKEUP 以启动转换
    spi_write_byte(ADS1256_CMD_SYNC);
    usleep(1);
    spi_write_byte(ADS1256_CMD_WAKEUP);

    // 等待转换完成 (根据采样率调整)
    // 1000 SPS -> 约 1ms
    usleep(2000);

    return ads1256_read_data();
}

// ============================================================
// 读取指定差分通道
// ============================================================
int32_t ads1256_read_diff(uint8_t pos, uint8_t neg)
{
    ads1256_set_channel(pos, neg);
    usleep(1);

    spi_write_byte(ADS1256_CMD_SYNC);
    usleep(1);
    spi_write_byte(ADS1256_CMD_WAKEUP);

    usleep(2000);

    return ads1256_read_data();
}

// ============================================================
// 连续扫描所有 8 个单端通道
// ============================================================
void ads1256_scan_all(void)
{
    printf("\n===== 多通道扫描 =====\n");
    for (int ch = 0; ch < 8; ch++) {
        int32_t raw = ads1256_read_channel(ch);
        double volts = ads1256_raw_to_voltage(raw, 2);  // PGA=2
        printf("通道 AIN%d: raw = %7d, 电压 = %+.6f V\n", ch, raw, volts);
    }
}

// ============================================================
// SPI 关闭
// ============================================================
void spi_close(void)
{
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }
}

// ============================================================
// 毫秒延时
// ============================================================
void delay_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char *argv[])
{
    printf("ADS1256 Linux SPI 采集示例\n");
    printf("===========================\n\n");

    // 1. 初始化 SPI
    if (spi_init(SPI_DEVICE, SPI_SPEED, SPI_MODE, SPI_BITS) < 0) {
        return 1;
    }

    // 2. 初始化 ADS1256
    if (ads1256_init() < 0) {
        spi_close();
        return 1;
    }

    printf("\n===== 单通道测试 (AIN0) =====\n");
    for (int i = 0; i < 10; i++) {
        int32_t raw   = ads1256_read_channel(0);
        double  volts = ads1256_raw_to_voltage(raw, 2);
        printf("[%d] raw=%7d, voltage=%+.6f V\n", i, raw, volts);
        delay_ms(10);
    }

    // 3. 扫描所有通道
    ads1256_scan_all();

    // 4. 差分模式示例 (AIN0 - AIN1)
    printf("\n===== 差分测试 (AIN0 - AIN1) =====\n");
    for (int i = 0; i < 5; i++) {
        int32_t raw   = ads1256_read_diff(0, 1);
        double  volts = ads1256_raw_to_voltage(raw, 2);
        printf("[%d] raw=%7d, voltage=%+.6f V\n", i, raw, volts);
        delay_ms(10);
    }

    // 5. 清理
    spi_close();
    printf("\n程序结束.\n");

    return 0;
}
