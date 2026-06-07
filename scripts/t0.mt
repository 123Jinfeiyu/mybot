
# 在 @Action 矩阵中，硬件变量的参数可以使用别名
#@Alias
#name      alias;
#   A      angle;
#   F  frequency;
#@

@I2C
 name        path;
#i2c-0  /dev/i2c-0;
i2c-1  /dev/i2c-1;
@

@PCA9685
     name    I2C   address;
pca9685-0  i2c-1   0x40;
@

@Servo
 name   max  min  offset  controller  channel;
  s1    90     -       -   pca9685-0        0;	# rb0
  s2   180     -       -           -        1;	# rb1 
  s3   -90     -       -           -        2;	# lb0
  s4  -180     -       -           -        3;	# lb1
  s5    90     -       -           -        4;	# lf0
  s6   180     -       -           -        5;	# lf1
  s7   -90     -       -           -        6;	# rf0
  s8  -180     -       -           -        7;	# rf1
  s9   180     -       -           -        8;	# cam0
 s10   180     -       -           -        9;	# cam1
@

# 行动矩阵
@Action
s1.A  s2.A  s3.A  s4.A  s5.A  s6.A  s7.A  s8.A  s9.A  s10.A  spk.F  spk.delay;	# column name
  45     -    45     -    45     -    45     -    90     90     10        200;	# stand
@
