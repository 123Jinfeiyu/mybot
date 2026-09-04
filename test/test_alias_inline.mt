# 用例1：动作脚本内联 @Alias（结构脚本 test_alias_inline.sql 没有 Alias 表）
# 运行：cat test/test_alias_inline.mt | ./target/release/bin/main --robot=test/test_alias_inline.sql
@Alias
name  alias;
   A  angle;
@

@Action
s1.A;
   45;
@
