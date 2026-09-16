/* 实证 HAL.lua 硬编码的 ioctl 编号与结构尺寸同内核头文件一致。
 *
 * 本程序只【打印事实】，断言放在 test_abi_consts.lua——那里才读得到 HAL.lua 的
 * 常量。原实现同样只打印、main 恒返回 0，属【假自检】：ioctl 编号漂移时它照样绿灯。
 *
 * 输出约定：每行严格 `key = value`（十进制或 0x 十六进制），供 Lua 侧 gmatch 解析。
 * 编译：cc -Wall -o <out> test/test_ioctl_nums.c
 */
#include <stdio.h>
#include <pthread.h>
#include <linux/spi/spidev.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>

int main(void) {
	/* ---- I2C：HAL 走 I2C_RDWR 原子事务（I2C_SLAVE 已不用，留档备查）---- */
	printf("I2C_RDWR = 0x%04lx\n", (unsigned long)I2C_RDWR);
	printf("I2C_M_RD = 0x%04lx\n", (unsigned long)I2C_M_RD);
	printf("I2C_SLAVE = 0x%04lx\n", (unsigned long)I2C_SLAVE);
	printf("sizeof_i2c_msg = %zu\n", sizeof(struct i2c_msg));
	printf("sizeof_i2c_rdwr_ioctl_data = %zu\n", sizeof(struct i2c_rdwr_ioctl_data));

	/* ---- SPI ---- */
	printf("SPI_IOC_WR_MODE = 0x%08x\n", (unsigned)SPI_IOC_WR_MODE);
	printf("SPI_IOC_WR_BITS_PER_WORD = 0x%08x\n", (unsigned)SPI_IOC_WR_BITS_PER_WORD);
	printf("SPI_IOC_WR_MAX_SPEED_HZ = 0x%08x\n", (unsigned)SPI_IOC_WR_MAX_SPEED_HZ);
	printf("SPI_IOC_MESSAGE_1 = 0x%08x\n", (unsigned)SPI_IOC_MESSAGE(1));
	printf("sizeof_spi_ioc_transfer = %zu\n", sizeof(struct spi_ioc_transfer));

	/* ---- GPIO（gpiod v1 ioctl）---- */
	printf("GPIO_GET_LINEHANDLE_IOCTL = 0x%08x\n", (unsigned)GPIO_GET_LINEHANDLE_IOCTL);
	printf("GPIOHANDLE_GET_LINE_VALUES_IOCTL = 0x%08x\n", (unsigned)GPIOHANDLE_GET_LINE_VALUES_IOCTL);
	printf("GPIOHANDLE_SET_LINE_VALUES_IOCTL = 0x%08x\n", (unsigned)GPIOHANDLE_SET_LINE_VALUES_IOCTL);
	printf("sizeof_gpiohandle_request = %zu\n", sizeof(struct gpiohandle_request));
	printf("sizeof_gpiohandle_data = %zu\n", sizeof(struct gpiohandle_data));
	printf("sizeof_gpiochip_info = %zu\n", sizeof(struct gpiochip_info));

	/* ---- 守护共享槽的 mutex 预留位（pthread.lua daemon_slot_t 预留 64B）---- */
	printf("sizeof_pthread_mutex_t = %zu\n", sizeof(pthread_mutex_t));
	return 0;
}
