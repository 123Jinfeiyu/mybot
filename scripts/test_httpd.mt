# NOTE(20260527-qing):
# - HTTPD硬件变量 - 测试脚本
# - 预计在version_0.0.4beta中可用
# - 
# - 该对象的特点是在接收空参数的play()方法中执行监听操作，所以@Action行为到达Httpd对象时会陷入永恒循环
# -
# - 另外一个方案是为httpd服务创建线程






# 在 @Action 矩阵中，硬件变量的参数可以使用别名
@Alias
name      alias;
   A      angle;
   F  frequency;
@

@I2C
 name        path;
i2c-0  /dev/i2c-0;
i2c-1  /dev/i2c-1;
@

@PCA9685
     name    I2C   addr;
pca9685-0  i2c-1   0x40;
@

@MPU6050_BIND
     name    I2C   addr;
mpu6050-0  i2c-1   0x69;
mpu6050-1  i2c-1   0x68;
#mpu6050-2  i2c-0   0x68;
#mpu6050-3  i2c-0   0x69;
@

@Speaker
name  channels  sample_rate  volumn  max_f  min_f;
 spk         2        48000     0.6   5000      -;
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


@Server
  name    proxy   addr     port
http_0     http   0.0.0.0  6600
@

@Httpd_Config
method  path        action        controller
post    /api/fs     FileSystem        http_0
   -    /api/proxy  HttpProxy              -
   -    /api/bot    MatrixScript           -
@

@Action
  name
http_0
@