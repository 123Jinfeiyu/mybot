
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
INSERT INTO Timer (name) VALUES ('spk');

CREATE TABLE Servo (
	name 		String,
	max  		String,
	min  		String,
	offset		String,
	controller	String,
	channel		String
);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s1,    90,   -, -5,  'pca9685-0', 6);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s2,  -180,   -,  0,  'pca9685-0', 5); 
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s3,   -90,   -,  0,  'pca9685-0', 14);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s4,   180,   -,  25, 'pca9685-0', 15);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s5,    90,   -,  0,  'pca9685-0', 8);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s6,  -180,   -,  0,  'pca9685-0', 9);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s7,   -90,   -,  0,  'pca9685-0', 11);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s8,   180,   -,  0,  'pca9685-0', 12);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s9,   180,   -,  0,  'pca9685-0', 0);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s10,  180,   -,  0,  'pca9685-0', 1);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s11,   90,   -,  0,  'pca9685-0', 4);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s12,   90,   -,  0,  'pca9685-0', 7);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s13, -180,   -,  55, 'pca9685-0', 13);
INSERT INTO Servo (name, max, min, offset, controller, channel)  VALUES  (s14,  180,   -,  0,  'pca9685-0', 10);
