
CREATE TABLE Alias (
	name  String,
	alias String
);
INSERT INTO Alias (name, alias) VALUES (A, angle);
INSERT INTO Alias (name, alias) VALUES (F, frequency);


CREATE TABLE I2C (
	name String,
	path String
);
INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');

CREATE TABLE PCA9685 (
	name    String,
	I2C     String,
	address String
);
INSERT INTO PCA9685 (name, I2C, address) VALUES ('pca9685-0', 'i2c-1', '0x40');

CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t1');

CREATE TABLE Servo (
	name 		String,
	max  		String,
	min  		String,
	offset		String,
	controller	String,
	channel		String
);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s1,    90,   -, -5,  'pca9685-0', 6);		# rb0
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s2,  -180,   -,  0,  'pca9685-0', 5);		# rb1 
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s3,   -90,   -,  0,  'pca9685-0', 14);	# lb0
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s4,   180,   -,  25, 'pca9685-0', 15);	# lb1
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s5,    90,   -,  0,  'pca9685-0', 8);		# lf0
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s6,  -180,   -,  0,  'pca9685-0', 9);		# lf1
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s7,   -90,   -,  0,  'pca9685-0', 11);	# rf0
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s8,   180,   -,  0,  'pca9685-0', 12);	# rf1
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s9,   180,   -,  0,  'pca9685-0', 0);		# cam0
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s10,  180,   -,  0,  'pca9685-0', 1);		# cam1
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s11,   90,   -,  0,  'pca9685-0', 4);		# rb2
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s12,   90,   -,  0,  'pca9685-0', 7);		# lb2
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s13, -180,   -,  55, 'pca9685-0', 13);	# lf2
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s14,  180,   -,  0,  'pca9685-0', 10);	# rf2
