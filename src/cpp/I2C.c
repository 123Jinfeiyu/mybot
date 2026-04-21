/*
 *  i2c - 用户界面
 *
 *        这里已经提供了I2C通用接口，可以将上层代码移动到仓颉下面
 */

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h> /* 其中宏的取值可能具有平台特异性 */
#include "bot4cj.h"        /* 有没有可能在仓颉代码中解析C头文件取得宏 */


/*
 * 初始化i2c设备
 *
 *   动态指定设备文件
 *
 *   返回打开的文件句柄或者错误码
 */
int16_t I2C_init(const char* filepath) {  
   int16_t fd;
    if ((fd = open(filepath, O_RDWR)) < 0) {
	perror("Failed to open I2C bus");
	return BOT4CJ_I2C_OPENED_FAILED;
    }
    else {
	return fd;
    }
}

/* 释放I2C设备文件 */
void I2C_free(int16_t fd) {
    close(fd);
}

/* 设置I2C地址 */
int I2C_setAddr(int16_t fd, uint8_t addr) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
	perror("Failed to set I2C address");
	return BOT4CJ_I2C_ERR;
    }
    else {
	return BOT4CJ_I2C_OK;
    }
}

/* 写寄存器 */
int I2C_writeReg(int16_t fd, uint8_t reg, uint8_t val) {  
    uint8_t buf[] = {reg, val};
    if (write(fd, buf, 2) != 2) {
	perror("I2C write error");
	return BOT4CJ_I2C_ERR;
    }
    else {
	return BOT4CJ_I2C_OK;
    }
}

/* 读寄存器 失败返回0 */
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

/* 读寄存器 - 有符号
 *   失败返回0 */
int8_t I2C_readRegFlag(int16_t fd, uint8_t reg) {

    int8_t value;

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
