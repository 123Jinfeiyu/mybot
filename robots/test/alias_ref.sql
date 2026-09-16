/*
 * 用例4配套结构：$ 引用列指向 @Alias 变量——别名不可被引用
 * 配合 alias_ref.mt 运行，预期非 0 退出并给出带表/列/行上下文的文案
 * （回归防线：0.3.3 前此处抛裸 JsonException 'Fail to convert to JsonObject'）
 */
CREATE TABLE Alias (
	name  String,
	alias String
);
INSERT INTO Alias (name, alias) VALUES ('A', 'angle');

CREATE TABLE dummy (
	name String,
	tag  String
);
INSERT INTO dummy (name, tag) VALUES ('dummy-0', '$A');
