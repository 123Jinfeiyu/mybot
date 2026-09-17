/*
 * @Vars 命名文本变量回归（正值）：
 *   1) 结构脚本 @Vars 表（name/value）作为 $ 文本常量，填充 Relay.note；
 *   2) .mt 内联 @Vars 在 @Action 参数位做名字替换（timer-0.A 的参数位 -> delay）。
 * 全程离线：Relay 把 test 转发给 Timer，Timer 为纯软件延时。
 * 用法：cat scripts/test/vars_text.mt | ./build/mybot --robot=robots/test/vars_text.sql
 */
CREATE TABLE Vars (
	name  String,
	value String
);
INSERT INTO Vars (name, value) VALUES ('greeting', 'hello-vars');

CREATE TABLE Timer (name String);
INSERT INTO Timer (name) VALUES ('timer-0');

CREATE TABLE Relay (
	name       String,
	controller String,
	note       String
);
INSERT INTO Relay (name, controller, note) VALUES ('relay0', 'timer-0', '$greeting');
