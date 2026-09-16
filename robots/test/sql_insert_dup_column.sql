/* 负向：同一行重复列 */
CREATE TABLE Timer (name String);
INSERT INTO Timer (name, name) VALUES ('a', 'b');
