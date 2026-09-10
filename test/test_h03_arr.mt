# H-03 修复验证：h03 驱动 call 一次，打印应含 values_n=4 / values_sum / list=[10,20,30]
# 第一拍 t0.delay=200ms 暖机：等 arr_src 守护完成 setup 首发（规避首发竞态），再读快照
@Action
t0.delay  h03.test;
   200       -     ;
    -        1     ;
@