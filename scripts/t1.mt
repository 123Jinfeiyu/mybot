# 无硬件情况下的测试脚本


# 在 @Action 矩阵中，硬件变量的参数可以使用别名
@Alias
name      alias;
   A      angle;
   F  frequency;
@

@TEST
name  parameter0  patameter1;
  t0          p0          p1;
  t1          p2          p3;
@


@Action
t0.A   t1.F;
  10     20;
@
