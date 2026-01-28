/*
 *  该文件是i2c用户界面
 *
 * NOTE:  以C++的方式处理异常可能会更好
 * FIXME: 包含询问方法以确定i2c设备的状态
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <pthread.h>

static int fd = -1;
static pthread_mutex_t mutex;  /* 这个锁只提供接口给外部使用 */

int I2C_init(const char* filepath) {  /* 动态指定设备文件 */
    if ((fd = open(filepath, O_RDWR)) < 0) {
	perror("Failed to open I2C bus");
	return 1;
    }
    else {
	return 0;
    }
}

void I2C_free() {
    close(fd);
    fd = -1;
}

int I2C_setAddr(const uint8_t addr) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
	perror("Failed to set I2C address");
	return 1;
    }
    else {
	return 0;
    }
}

int I2C_writeReg(uint8_t reg, uint8_t val) {  /* 写寄存器 */
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
uint8_t I2C_readReg(uint8_t reg) {

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


/* 获取锁 */
void I2C_lock() {
    pthread_mutex_lock(&mutex); /* 加锁 */
}

/* 释放锁 */
void I2C_unlock() {
    pthread_mutex_unlock(&mutex);   /* 解锁 */
}
