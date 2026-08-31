
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
  s1    90     -      -5   pca9685-0        6;	# rb0
  s2  -180     -       0           -        5;	# rb1 
  s3   -90     -       -           -       14;	# lb0
  s4   180     -      25           -       15;	# lb1
  s5   -90     -       0           -       11;	# lf0
  s6   180     -       -           -       12;	# lf1
  s7    90     -       -           -        8;	# rf0
  s8  -180     -       -           -        9;	# rf1
  s9   180     -       -           -        0;	# cam0
 s10   180     -       -           -        1;	# cam1
 s11    90     -       -   pca9685-0        4;  # rb2
 s12    90     -       -           -        7;  # lb2
 s13  -180     0      55           -       13;  # lf2
 s14   180     -       -           -       10;  # rf2
@
@
