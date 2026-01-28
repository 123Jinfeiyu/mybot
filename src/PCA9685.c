/* PCA9685 舵机驱动板 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "bot4cj.h"

void PCA9685_init(int16_t fd, uint8_t addr) {
    I2C_setAddr(fd, addr);
    I2C_writeReg(fd, 0x00, 0x20);   /* 开启AI（地址自增）和关闭SLEEP */
    usleep(5000);               /* 等待振荡器启动 */

    /* 设置PWM频率为50Hz（舵机标准周期20ms） */
    const float freq = 50.0;
    float prescale_val = 25000000.0 / (4096 * freq) - 1;    /* 25MHz主频 */
    uint8_t prescale = (uint8_t)(prescale_val + 0.5);

    I2C_writeReg(fd, 0x00, 0x10);       /* 进入SLEEP模式（允许改频率） */
    I2C_writeReg(fd, 0xFE, prescale);   /* 写入预分频值 */
    I2C_writeReg(fd, 0x00, 0x20);       /* 退出SLEEP */
    usleep(5000);
}

/* 设置指定通道的PWM信号（0-4095） */
void PCA9685_setPWM(int16_t fd, uint8_t channel, uint16_t on, uint16_t off) {
    uint8_t reg = 0x06 + 4 * channel;   /* LED0 寄存器起始地址 */
    I2C_writeReg(fd, reg, on & 0xFF);
    I2C_writeReg(fd, reg + 1, on >> 8);
    I2C_writeReg(fd, reg + 2, off & 0xFF);
    I2C_writeReg(fd, reg + 3, off >> 8);
}

/*
 * 角度转PWM值（0°~180° → 0.5ms~2.5ms脉宽）
 */
void PCA9685_setAngle(int16_t fd, uint8_t channel, float angle) {
    const float min = 0.5;    /* 0°对应0.5ms */
    const float max = 2.5;    /* 180°对应2.5ms */
    float width = min + (max - min) * (angle / 180.0);
    uint16_t off_val = (uint16_t)(width * 4096 / 20);        /* 20ms 周期 */
    PCA9685_setPWM(fd, channel, 0, off_val); /* ON=0, OFF=计算值 */
}
