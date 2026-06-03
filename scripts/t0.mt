
# 在 @Action 矩阵中，硬件变量的参数可以使用别名
#@Alias
#name      alias;
#   A      angle;
#   F  frequency;
#@

@I2C
 name        path;
i2c-0  /dev/i2c-0;
i2c-1  /dev/i2c-1;
@

@PCA9685
     name    I2C   addr;
pca9685-0  i2c-1   0x40;
@

