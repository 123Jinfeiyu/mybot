

@Timer
 name   max  min;
delay  1000    -;
@

@I2C
 name        path;
i2c-0  /dev/i2c-0;
i2c-1  /dev/i2c-1;
@

@PCA9685
     name    I2C   path;
pca9685-0  i2c-1   0x40;
@

@MPU6050
     name    I2C   path;
mpu6050-0  i2c-1   0x69;
mpu6050-1  i2c-1   0x68;
mpu6050-2  i2c-0   0x68;
mpu6050-3  i2c-0   0x69;
@

@Speaker
name  channels  volumn  max_f  min_f;
 spk         2     0.6   5000      -;
@

@Servo
 name   max  min  offset  controller  id;
  s1    90     -       -   pca9685-0   0;	# rb0
  s2   180     -       -           -   1;	# rb1 
  s3   -90     -       -           -   2;	# lb0
  s4  -180     -       -           -   3;	# lb1
  s5    90     -       -           -   4;	# lf0
  s6   180     -       -           -   5;	# lf1
  s7   -90     -       -           -   6;	# rf0
  s8  -180     -       -           -   7;	# rf1
  s9   180     -       -           -   8;	# cam0
 s10   180     -       -           -   9;	# cam1
@

@Action
s1    s2  s3   s4  s5   s6  s7   s8   s9 s10   spk  delay;	# column name
45     -  45    -  45    -  45    -   90  90   10     200;	# stand
90     -   -    -   -    -   -    -    -   -    -      20;	# left-behind

45     -   -    -   -    -   -    -    -   -    0     500;	# stand
 -     -  90    -   -    -  45    -    -   -    -      20;	# right-behind

 -     -  45    -   -    -   -    -    -   -  200     500;	# stand
 -     -   -    -  90    -   -    -    -   -    0      20;	# right-front

 -     -   -    -  45    -   -    -    -   -    -     500;	# stand
 -     -   -    -   -    -  90    -    -   -    -      20;	# left-front

 -     -   -    -   -    -  45    -    -   -    -     500;	# stand
 -     -   -    -   -    -   -    -    -   -    -      20;	# stand
@
