# SCSCL 总线舵机无硬件端到端配套动作：单舵机位置控制四帧
# 运行：cat scripts/test/scscl_virt.mt | ./build/mybot --robot=robots/test/scscl_virt.sql
# position 经 offset/限幅管线后按大端写入 0x2A；speed 步/s（0 且 time 非 0
# 时按 time 走）；末行 speed/time 用 "-" 继承上一行（矩阵占位符语义）
@Action
s1.position  s1.speed  s1.time  t0.delay;
        512       300         0       50;
        256         0       400       50;
       1023      1000         0       50;
          0         -         -       50;
@
