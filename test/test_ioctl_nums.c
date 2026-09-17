/* HAL.lua 硬编码 ioctl 编号与内核头文件的一致性自检（F-24）
 * 全部为编译期断言：常量漂移（换内核/换架构）在构建期即失败，
 * 不再依赖人工比对打印输出。期望值出处：modules/lib/HAL.lua ioctl 段注释。
 * 用 _Static_assert 而非 #if：_IOW/_IOWR 宏展开含 sizeof，#if 表达式不支持。 */
#include <stdio.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>

_Static_assert(SPI_IOC_WR_MODE == 0x40016b01,
	"SPI_IOC_WR_MODE != 0x40016b01 (HAL.lua)");
_Static_assert(SPI_IOC_WR_BITS_PER_WORD == 0x40016b03,
	"SPI_IOC_WR_BITS_PER_WORD != 0x40016b03 (HAL.lua)");
_Static_assert(SPI_IOC_WR_MAX_SPEED_HZ == 0x40046b04,
	"SPI_IOC_WR_MAX_SPEED_HZ != 0x40046b04 (HAL.lua)");
_Static_assert(SPI_IOC_MESSAGE(1) == 0x40206b00,
	"SPI_IOC_MESSAGE(1) != 0x40206b00 (HAL.lua)");
_Static_assert(GPIO_GET_LINEHANDLE_IOCTL == 0xC16CB403,
	"GPIO_GET_LINEHANDLE_IOCTL != 0xC16CB403 (HAL.lua)");
_Static_assert(GPIOHANDLE_GET_LINE_VALUES_IOCTL == 0xC040B408,
	"GPIOHANDLE_GET_LINE_VALUES_IOCTL != 0xC040B408 (HAL.lua)");
_Static_assert(GPIOHANDLE_SET_LINE_VALUES_IOCTL == 0xC040B409,
	"GPIOHANDLE_SET_LINE_VALUES_IOCTL != 0xC040B409 (HAL.lua)");

_Static_assert(sizeof(struct spi_ioc_transfer) == 32,
	"sizeof(spi_ioc_transfer) != 32 (HAL.lua ffi 断言)");
_Static_assert(sizeof(struct gpiohandle_request) == 364,
	"sizeof(gpiohandle_request) != 364 (HAL.lua ffi 断言)");
_Static_assert(sizeof(struct gpiohandle_data) == 64,
	"sizeof(gpiohandle_data) != 64 (HAL.lua ffi 断言)");

int main(void) {
	printf("ioctl 常量自检通过：SPI/GPIO ioctl 编号与结构尺寸均与内核头文件一致\n");
	return 0;
}
