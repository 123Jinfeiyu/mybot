# 已知限制与常见问题

> [README](../README.md) 中只保留一句话提示，排障细节都在这里。

## 已知限制

- **仅支持 Linux**：HAL 经 ffi 直调 glibc，Windows/macOS 未支持
- **仅支持 LuaJIT**：总线驱动依赖 ffi（不依赖 HAL 的模块可用 lua5.4 独立调试）。
  缓解：新架构（如 riscv64）可关注 LuaJIT 社区分支移植；纯算法类驱动
  （Ema/Kalman/fft 等）已保持不依赖 HAL，可用 lua5.4 跨解释器复用
- **守护线程开销**：每个守护一个独立线程 + 独立 Lua 状态，拍间必须让出式 sleep（见 [daemon.md](daemon.md)）。
  缓解：守护数量随器件线性增长，规模化时优先合并同类采样到单个守护
  （如一个守护轮询多只同型号传感器），并为低频守护放宽 tick 间隔
- **一器件一消费者**：页式寄存器序列器件（如 VL53L0X）与握手式间接访问器件（如 AS7262）只支持单消费者
- 与仓颉原版不同：本版**没有**仓颉运行时并发 GC 缺陷，超长动作脚本不受该问题影响

## 常见问题

### Q: 不接硬件也能开发吗？

A: 可以。`bash test/run_all.sh` 用内存仿真（virtbus）与桩替身跑全部离线回归；`relay-test` 链路不需要任何硬件，先跑通它即可验证环境。详见 [virtbus.md](virtbus.md) 与 [testing.md](testing.md)。

### Q: 守护线程 publishes=0？

A: 动作序列极短（几行瞬时 play）时，守护可能在首次采样前就被停止。真实动作序列为秒级没问题；测试时用 Timer 延时列拉长主线程时间窗。

### Q: I2C 设备报 EBUSY？

A: HAL 已用 `I2C_RDWR` 原子事务，地址由每条 i2c_msg 携带，不需要 `I2C_SLAVE` ioctl，一般不会再有内核驱动占用地址导致的 EBUSY（见 [hal.md](hal.md)）。

### Q: 报错提示看不懂怎么办？

A: 结构脚本报错会指明是哪张表哪一行哪一列；动作脚本报错会说明是哪个硬件变量与参数。按提示修正即可。

### Q: 不知道 SCSCL 舵机现在的站号（二手舵机 / 被别人改过号）怎么办？

A: 结构脚本的 `id` 列描述的是**舵机此刻的真实站号**，不是你想设的值——不知道就没法装配（`build()` 会 PING 它，不应答即报 `No PING response`）。`modules/SCSCL.lua` 把 `id` 限死在 1~253，广播地址 `0xFE` 被明确拒绝（驱动不承担扫总线职责，且放行广播会让一个 typo 同时动到总线所有舵机），所以用独立扫描工具：

```bash
luajit scripts/scscl_scan.lua --scan            # 逐号 1~253 定向 PING（多舵机用这个，不会冲突）
luajit scripts/scscl_scan.lua                   # 广播 PING（只挂一块舵机时最快）
luajit scripts/scscl_scan.lua --scan --to 32    # 限定范围，省时间
luajit scripts/scscl_scan.lua --raw             # 附原始字节 dump，用来分辨「线路不通」与「协议对不上」
```

全程只发 PING(0x01)，不写任何寄存器、不碰 EPROM，可反复跑。输出末尾会给出可直接粘贴的 `INSERT INTO SCSCL ...` 片段。注意两点：打开 `/dev/serial0` 需要 root 或 `dialout` 组；广播 PING 会让总线上**每块**舵机同时应答，半双工单总线会帧冲突，所以只适合只挂一块时"捉"它——多舵机请用 `--scan`。

想把舵机换成你指定的号：在 `.sql` 的 SCSCL 表显式声明授权列（`allow_burn String` 且该行值为 `1`——烧号是不可逆广播写，须结构期授权），然后在 `.mt` 里单开一行写 `set_id=N`（解锁 EPROM → 广播写号，断电重启生效；真机烧号只接目标舵机），重启后再把 `.sql` 的 `id` 改成 N。未声明 `allow_burn` 的结构脚本会拒绝 `set_id` 烧号（回归 `scscl_burn_guard`）。完整流程见 `modules/SCSCL.lua` 头部注释。
