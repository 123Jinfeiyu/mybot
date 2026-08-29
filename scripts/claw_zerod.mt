
# 在 @Action 矩阵中，硬件变量的参数可以使用别名
@Alias
name      alias;
   A      angle;
   F  frequency;
@

@I2C
 name        path;
#i2c-0	/dev/i2c-0;
i2c-1  /dev/i2c-1;
@

@PCA9685
     name    I2C   address;
pca9685-0  i2c-1   0x40;
@

@MPU6050_BIND
     name    I2C   addr;
#mpu6050-0  i2c-1   0x69;
#mpu6050-1  i2c-1   0x68;
#mpu6050-2  i2c-0   0x68;
#mpu6050-3  i2c-0   0x69;
@

@Speaker
name  channels  sample_rate  volumn  max_f  min_f;
 spk         2        48000     0.6   5000      -;
@

@Servo
name   max  min  offset  controller  channel;
s11     90    -       -   pca9685-0        10;  # rb2
s12     90    -       -           -       11;  # lb2
s13   -180    0      55           -       12;  # lf2
s14    180    -       -           -       13;  # rf2
@

# 行动矩阵
@Action
s11.A  s12.A  s13.A  s14.A   spk.F  spk.delay;	# column name
    -      -    90       -      10       1000;	# stand
    -      -    60       -      10       1000;	# stand
    -      -    30       -      10       1000;	# stand
    -      -     0       -      10       1000;	# stand
    -      -    30       -      10       1000;	# stand
    -      -    60       -      10       1000;	# stand
    -      -    90       -      10       1000;	# stand
@
