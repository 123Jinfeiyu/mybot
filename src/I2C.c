/*
 *  i2c - 用户界面
 *
 *        按理说这里已经提供了I2C通用接口，可以将上层代码移动到仓颉下面
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

int16_t I2C_init(const char* filepath) {  /* 动态指定设备文件 */
    int16_t fd;
    if ((fd = open(filepath, O_RDWR)) < 0) {
	perror("Failed to open I2C bus");
	return -1;
    }
    else {
	return fd;
    }
}

void I2C_free(int16_t fd) {
    close(fd);
    fd = -1;
}

int I2C_setAddr(int16_t fd, uint8_t addr) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
	perror("Failed to set I2C address");
	return 1;
    }
    else {
	return 0;
    }
}

int I2C_writeReg(int16_t fd, uint8_t reg, uint8_t val) {  /* 写寄存器 */
    uint8_t buf[] = {reg, val};
    if (write(fd, buf, 2) != 2) {
	perror("I2C write error");
	return 1;
    }
    else {
	return 0;
    }
}

/* 读寄存器 */
uint8_t I2C_readReg(int16_t fd, uint8_t reg) {

    uint8_t value;

    if (write(fd, &reg, 1) != 1) {
	perror("I2C read error");
	return 0;
    }

    if (read(fd, &value, 1)!= 1) {
	perror("I2C read error");
	return 0;
    }

    return value;
}
