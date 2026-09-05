/*
 * MPU6050 六轴守护采样测试：sensor_daemon 万能采样，
 * interval=10ms，fields 六轴，sign=16（原始值为有符号 16 位）
 * 配合 test_mpu_daemon.mt
 */
CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (
	name     String,
	chunk    String,
	driver   String,
	proto    String,
	path     String,
	addr     String,
	interval String,
	fields   String,
	sign     String
);
INSERT INTO Daemon (name, chunk, driver, proto, path, addr, interval, fields, sign) VALUES ('imu0', 'modules/lib/sensor_daemon.lua', 'MPU6050', 'i2c', '/dev/i2c-1', '0x68', '10', 'ACCEL_X|ACCEL_Y|ACCEL_Z|GYRO_X|GYRO_Y|GYRO_Z', '16');
