@Timer
name max min ;
delay 1000 - ;
@

@I2C
name path ;
i2c-1 /dev/i2c-1 ;
@

@PCA9685
name I2C path ;
pca9685-0 i2c-1 0x40 ;
@

@MPU6050
name I2C path ;
mpu6050-0 i2c-1 0x69 ;
mpu6050-1 i2c-1 0x68 ;
@

@Speaker
name channels sample_rate volumn max_f min_f ;
spk 2 48000 0.6 5000 - ;
@

@Servo
name max min offset controller channel ;
s1 90 - - pca9685-0 0 ;
s2 180 - - - 1 ;
s3 -90 - - - 2 ;
s4 -180 - - - 3 ;
s5 90 - - - 4 ;
s6 180 - - - 5 ;
s7 -90 - - - 6 ;
s8 -180 - - - 7 ;
s9 180 - - - 8 ;
s10 180 - - - 9 ;
@

@Action
s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 spk delay ;
45 - 45 - 45 - 45 - 90 90 10 200 ;
90 - - - - - - - - - - 20 ;
45 - - - - - - - - - 0 500 ;
- - 90 - - - 45 - - - - 20 ;
- - 45 - - - - - - - 200 500 ;
- - - - 90 - - - - - 0 20 ;
- - - - 45 - - - - - - 500 ;
- - - - - - 90 - - - - 20 ;
- - - - - - 45 - - - - 500 ;
- - - - - - - - - - - 20 ;
@

