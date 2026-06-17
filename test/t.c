#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <linux/spi/spidev.h>
int main () {
	printf("int: %lu\n", sizeof(int));

	printf("O_RDWR: %i\n", O_RDWR);

	printf("pthread_t: %lu\n", sizeof(pthread_t));
	printf("pthread_mutex_t: %lu\n", sizeof(pthread_mutex_t));
	printf("pthread_mutexattr_t: %lu\n", sizeof(pthread_mutexattr_t));

    printf("SPI_IOC_MAGIC: 0x%02X ('%c')\n", SPI_IOC_MAGIC, SPI_IOC_MAGIC);
    printf("SPI_IOC_WR_MODE: 0x%08X\n", SPI_IOC_WR_MODE);
    printf("SPI_IOC_RD_MODE: 0x%08X\n", SPI_IOC_RD_MODE);
    printf("SPI_IOC_WR_BITS_PER_WORD = 0x%08X\n", SPI_IOC_WR_BITS_PER_WORD);
    printf("SPI_IOC_RD_BITS_PER_WORD = 0x%08X\n", SPI_IOC_RD_BITS_PER_WORD);
    printf("SPI_IOC_WR_MAX_SPEED_HZ: 0x%08X\n", SPI_IOC_WR_MAX_SPEED_HZ);
    printf("SPI_IOC_RD_MAX_SPEED_HZ: 0x%08X\n", SPI_IOC_RD_MAX_SPEED_HZ);
}
