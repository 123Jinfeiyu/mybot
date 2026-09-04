CREATE TABLE Daemon (
	name  String,
	chunk String
);
INSERT INTO Daemon (name, chunk) VALUES ('d0', 'modules/lib/daemon_probe.lua');

CREATE TABLE dummy (
	name String,
	pp1  String
);
INSERT INTO dummy (name, pp1) VALUES ('dm', '-');
