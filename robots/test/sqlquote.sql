/*
 * SQL 词法器引号内注释回归（正值契约）：
 * 引号值内的 "--" 与 "/*" 是字面数据而非注释——修复前词法器把它们当注释
 * 起始符，吞掉引号内后续内容导致解析失败（Token <unexpected end of script>）。
 * 修复后本结构脚本应完整解析、正常建守护、退出 0。
 *
 * 用法：cat scripts/test/sqlquote.mt | bash scripts/run.sh --robot=robots/test/sqlquote.sql
 * 期望退出码 0，输出含 "joined, total publishes="
 */
CREATE TABLE Daemon (
	name   String,
	chunk  String,
	extra  String,
	tag    String
);
INSERT INTO Daemon (name, chunk, extra, tag)
  VALUES ('d0', 'lib/daemon_probe.lua', 'tag--with--dashes', 'a/*b*/c');