/*
 * SQL 词法器引号内特殊字符回归（正值契约）：
 * 引号值内的 "--"、块注释起始符、";"、","、"("、")" 与空白都是字面数据
 * ——修复前词法器会把它们当注释/分隔符拆分 token，导致解析失败或数据被切碎。
 * 修复后本结构脚本应完整解析、正常建守护、退出 0。
 *
 * 用法：cat scripts/test/sqlquote.mt | ./build/mybot --robot=robots/test/sqlquote.sql
 * 期望退出码 0，输出含 "joined, total publishes="
 */
CREATE TABLE Daemon (
	name   String,
	chunk  String,
	extra  String,
	tag    String
);
INSERT INTO Daemon (name, chunk, extra, tag)
  VALUES ('d0', 'lib/daemon_probe.lua', 'tag,with (parens) and ; semi and space', 'a/*b*/c d--e');