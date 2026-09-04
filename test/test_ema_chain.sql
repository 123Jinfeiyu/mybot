CREATE TABLE Timer (name String);
INSERT INTO Timer (name) VALUES ('t0');

CREATE TABLE Daemon (name String, chunk String);
INSERT INTO Daemon (name, chunk) VALUES ('sim0', 'modules/lib/daemon_probe.lua');

CREATE TABLE Ema (name String, alpha String, src String, field String);
INSERT INTO Ema (name, alpha, src, field) VALUES ('ema0', '0.3', 'sim0', 'v0');

CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', 'ema0');
