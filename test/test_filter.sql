CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (
	name  String,
	chunk String
);
INSERT INTO Daemon (name, chunk) VALUES ('sim0', 'modules/lib/daemon_probe.lua');

CREATE TABLE Kalman (
	name  String,
	Q     String,
	R     String,
	src   String,
	field String
);
INSERT INTO Kalman (name, Q, R, src, field) VALUES ('kf0', '20', '200', 'sim0', 'v0');

CREATE TABLE Monitor (
	name String,
	src  String
);
INSERT INTO Monitor (name, src) VALUES ('mon0', 'kf0');
