
/*
 * FIXME: 1. 传感器数据校准
        2. 可采用卡尔曼滤波

   FIXME: 采用类进行封装，因为可能需要一对
 */
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

/* 初始化 MPU6050 */
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

/* FIXME: 当设备剧烈运动时（如加速度远大于重力），加速度计数据会包含运动加速度，此时角度计算可能不准确。实际应用中需结合陀螺仪数据（如互补滤波或卡尔曼滤波）来提高稳定性。 */
/* FIXME: 适合静态测量或学习用途。如果需要实际应用，建议添加传感器融合算法 */
void MPU6050_calAngle(struct MPU6050_sensorData *data, float *pitch, float *roll) {
    /* - ±2g → 16384 LSB/g
       - ±4g → 8192 LSB/g
       - ±8g → 4096 LSB/g
       - ±16g → 2048 LSB/g */
    const float scale = 4096.0;    /* 加速度数据转重力单位（±8g 量程 4096 LSB/g） */
    float ax = data->accel_x / scale;
    float ay = data->accel_y / scale;
    float az = data->accel_z / scale;

    /* 计算俯仰角(Pitch)和翻滚角(Roll) */
    *pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / M_PI;
    *roll = atan2(ay, az) * 180.0 / M_PI;
}
