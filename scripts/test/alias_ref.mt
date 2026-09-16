# 用例4：结构脚本的 $ 引用列指向 @Alias 变量 A（配套 alias_ref.sql，预期非 0 退出）
# 运行：cat scripts/test/alias_ref.mt | ./target/release/bin/main --robot=robots/test/alias_ref.sql
# 别名只用于展开 @Action 列头的参数名，不是元数据值；引用它应在 build 期显式报错
@Action
dummy.echo;
1;
@
