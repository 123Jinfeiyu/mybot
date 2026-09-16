/* 负向：INSERT 写了表头不存在的列 */
CREATE TABLE Timer (name String);
INSERT INTO Timer (name, zz) VALUES ('a', '1');
