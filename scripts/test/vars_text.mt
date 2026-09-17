# @Vars 回归：内联 @Vars 把 A 替换成 delay（@Action 参数位），
# 结构脚本的 Vars 则把 $greeting 文本常量填进 Relay.note。
# 用法：cat scripts/test/vars_text.mt | ./build/mybot --robot=robots/test/vars_text.sql
@Vars
name  value;
   A   delay;
@

@Action
timer-0.A  relay0.test;
       0        200;
@
