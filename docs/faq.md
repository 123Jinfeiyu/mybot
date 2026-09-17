# 已知限制与常见问题

> [README](../README.md) 中只保留一句话提示，排障细节都在这里。

## 已知限制

- **仅支持 Linux**：HAL 经 ffi 直调 glibc，Windows/macOS 未支持
- **仅支持 LuaJIT**：总线驱动依赖 ffi（不依赖 HAL 的模块可用 lua5.4 独立调试）
- **守护线程开销**：每个守护一个独立线程 + 独立 Lua 状态，拍间必须让出式 sleep（见 [daemon.md](daemon.md)）
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
