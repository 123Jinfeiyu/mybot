#ifndef QING_I2C
#define QING_I2C
/* I2C */
const char* I2C_DEV = "/dev/i2c-1";
int16_t I2C_init(const char* filepath);
void I2C_free(int16_t fd);
int I2C_setAddr(int16_t fd, const uint8_t addr);
int I2C_writeReg(int16_t fd, uint8_t reg, uint8_t val);
uint8_t I2C_readReg(int16_t fd, uint8_t reg);
/* PCA9685 */
const uint8_t PCA9685_ADDR = 0x40;  /* 7位地址 */
void PCA9685_init(int16_t fd, uint8_t addr);
void PCA9685_setPWM(int16_t fd, uint8_t channel, uint16_t on, uint16_t off);
void PCA9685_init(int16_t fd, uint8_t addr);
#endif
