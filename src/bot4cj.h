/* I2C操作库 - PCA9685 - MPU6050 */
#ifndef QING_I2C
#define QING_I2C
/* I2C */
#define BOT4CJ_I2C_OK 0
#define BOT4CJ_I2C_ERR 1
#define BOT4CJ_I2C_OPENED_FAILED -1
int16_t I2C_init(const char* filepath);
void I2C_free(int16_t fd);
int I2C_setAddr(int16_t fd, uint8_t addr);
int I2C_writeReg(int16_t fd, uint8_t reg, uint8_t val);
uint8_t I2C_readReg(int16_t fd, uint8_t reg);
int8_t I2C_readRegFplag(int16_t fd, uint8_t reg);
#endif
