/* 负向：表头有 max 列但 INSERT 未给值（缺列会使矩阵单元静默错位）*/
CREATE TABLE Servo (name String, max String);
INSERT INTO Servo (name) VALUES ('s1');
