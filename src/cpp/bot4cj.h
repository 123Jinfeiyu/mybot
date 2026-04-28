/* I2C操作库 - PCA9685 - MPU6050 */
#ifndef QING_I2C
#define QING_I2C
/* I2C */
#define BOT4CJ_I2C_OK 0
#define BOT4CJ_I2C_ERR 1
#define BOT4CJ_I2C_OPENED_FAILED -1

#ifdef _WIN64
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

DLLEXPORT int16_t I2C_init(const char* filepath);
DLLEXPORT void I2C_free(int16_t fd);
DLLEXPORT int I2C_setAddr(int16_t fd, uint8_t addr);
DLLEXPORT int I2C_writeReg(int16_t fd, uint8_t reg, uint8_t val);
DLLEXPORT uint8_t I2C_readReg(int16_t fd, uint8_t reg);
DLLEXPORT int8_t I2C_readRegFlag(int16_t fd, uint8_t reg);
#endif
