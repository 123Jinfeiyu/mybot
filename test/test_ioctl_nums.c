/* 实证 HAL.lua 硬编码的 ioctl 编号与内核头文件一致 */
#include <stdio.h>
#include <linux/spi/spidev.h>
#include <linux/i2c-dev.h>

int main(void) {
	printf("I2C_SLAVE          = 0x%04lx\n", (unsigned long)I2C_SLAVE);
	printf("SPI_IOC_WR_MODE    = 0x%08x\n", SPI_IOC_WR_MODE);
	printf("SPI_IOC_WR_BITS    = 0x%08x\n", SPI_IOC_WR_BITS_PER_WORD);
	printf("SPI_IOC_WR_SPEED   = 0x%08x\n", SPI_IOC_WR_MAX_SPEED_HZ);
	printf("SPI_IOC_MESSAGE(1) = 0x%08x\n", SPI_IOC_MESSAGE(1));
	printf("sizeof(spi_ioc_transfer) = %zu\n", sizeof(struct spi_ioc_transfer));
	return 0;
}
