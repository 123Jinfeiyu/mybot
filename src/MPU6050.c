/* MPU6050陀螺仪与加速度计 - 使用界面 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "bot4cj.h"

#define MPU6050_ADDR1 0x68           /* 默认地址 */
#define MPU6050_PWR_MGMT_1 0x6B      /* 电源管理寄存器1 */
#define MPU6050_PWR_NOSLEEP 0x00     /* 电源管理 - 非睡眠模式 */
#define MPU6050_GYRO_CONF_REG 0x1B   /* 陀螺仪配置寄存器 */
#define MPU6050_GYRO_FS_500DPS 0x08  /* 陀螺仪量程 - 正负500度/秒 */
#define MPU6050_GYRO_REG_X_HIGH 0x43 /* x - 陀螺仪横轴 高字节 */
#define MPU6050_GYRO_REG_X_LOW 0x44
#define MPU6050_GYRO_REG_Y_HIGH 0x45 /* y */
#define MPU6050_GYRO_REG_Y_LOW 0x46
#define MPU6050_GYRO_REG_Z_HIGH 0x47 /* z */
#define MPU6050_GYRO_REG_Z_LOW 0x48
#define MPU6050_ACCEL_CONF_REG 0x1C /* 加速度计配置寄存器 */
#define MPU6050_ACCEL_FS_8g 0x10 /* 加速度计量程 正负8g */
#define MPU6050_ACCEL_REG_X_HIGH 0x3B
#define MPU6050_ACCEL_REG_X_LOW 0x3C
#define MPU6050_ACCEL_REG_Y_HIGH 0x3D
#define MPU6050_ACCEL_REG_Y_LOW 0x3E
#define MPU6050_ACCEL_REG_Z_HIGH 0x3F
#define MPU6050_ACCEL_REG_Z_LOW 0x40

/* 初始化 MPU6050
 * FIXME: 输入I2C地址*/
void MPU6050_init(int16_t fd) {
    I2C_setAddr(fd, MPU6050_ADDR1);
    I2C_writeReg(fd, MPU6050_PWR_MGMT_1,     MPU6050_PWR_NOSLEEP);     /* 退出睡眠 */
    I2C_writeReg(fd, MPU6050_GYRO_CONF_REG,  MPU6050_GYRO_FS_500DPS);  /* ±500°/s */
    I2C_writeReg(fd, MPU6050_ACCEL_CONF_REG, MPU6050_ACCEL_FS_8g);       /*  ±8g */
    usleep(100000);                 /* 等待稳定 */
}

/* 读取 16 位传感器数据（带符号） */
static int16_t read16BitReg(int16_t fd, uint8_t high, uint8_t low) {
    return (I2C_readReg(fd, high) << 8) | I2C_readReg(fd, low);
}

/* 获取原始MPU6050传感器数据 */
void MPU6050_readSensor(int16_t fd, struct MPU6050_sensorData *data) {
    I2C_setAddr(fd, MPU6050_ADDR1);
    data->accel_x = read16BitReg(fd, MPU6050_ACCEL_REG_X_HIGH, MPU6050_ACCEL_REG_X_LOW);
    data->accel_y = read16BitReg(fd, MPU6050_ACCEL_REG_Y_HIGH, MPU6050_ACCEL_REG_Y_LOW);
    data->accel_z = read16BitReg(fd, MPU6050_ACCEL_REG_Z_HIGH, MPU6050_ACCEL_REG_Z_LOW);
    data->gyro_x  = read16BitReg(fd, MPU6050_GYRO_REG_X_HIGH, MPU6050_GYRO_REG_X_LOW);
    data->gyro_y  = read16BitReg(fd, MPU6050_GYRO_REG_Y_HIGH, MPU6050_GYRO_REG_Y_LOW);
    data->gyro_z  = read16BitReg(fd, MPU6050_GYRO_REG_Z_HIGH, MPU6050_GYRO_REG_Z_LOW);
}