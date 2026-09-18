# HAL：基层硬件抽象

> 本文介绍 bot4cj 硬件抽象层 `modules/lib/HAL.lua` 的实现原理、接口契约与驱动沙箱。[README](../README.md) 只留入口链接，实现细节以本文为准。

## 概述

总线怎么"开口说话"封装在 `modules/lib/HAL.lua`，所有驱动共用，**仅支持 LuaJIT**：

- ffi 直调 glibc，原生性能零 fork；fd 为真实内核句柄，跨 Lua 状态机有效
- 系统调用统一走 `HAL.sys.*` 间接层，离线单测（`test_hal_ffi.lua`/`test_uart.lua`/`test_pwm.lua`/`test_gpio.lua`）注入 mock 即可验证调用序列与缓冲区内容，不触硬件
- 参数先归一化（字节 0~255、非负整数）再进 ioctl/缓冲区，设备路径走白名单正则；非法参数打印诊断并显式失败，不静默钳制
- 结构体尺寸与 ioctl 常量经 `test_ioctl_nums.c` / `test_uart_consts.c` 与系统头文件实证；尺寸断言在加载期执行（spi_ioc_transfer=32、i2c_msg=16、gpiohandle_request=364、termios=60），ABI 漂移立即失败

## 各总线要点

| 总线 | 关键实现 |
| ---- | -------- |
| I2C | `I2C_RDWR` ioctl **原子事务**：读的「写指针 + 读」合并为单次内核传输，消除并发访问同一器件时的寄存器指针竞态；地址由每条 i2c_msg 携带，不再需要 `I2C_SLAVE`（没有 EBUSY 问题）。16 位指针器件（VL53Lx ToF 族）同样合并为单事务 |
| SPI | spidev ioctl；mode/bits/speed 经 ioctl 持久化在 fd，`delay_us` 无 per-fd 持久化途径、由驱动在每次传输显式传入（`spi_write*`/`spi_read*`/`spi_xfer_full` 尾参） |
| GPIO | 内核 gpiod uAPI v1（`/dev/gpiochipN` ioctl，deprecated 但全内核支持）；offset 必须为非负整数（小数会被 uint32 静默截断成另一个线号，显式拒绝） |
| UART | termios raw 8N1 + 非阻塞（VMIN=0/VTIME=1）；open 快照原始 termios，close 先恢复再关闭（板载 UART 的配置跨 close 保留在 tty 上）；波特率必须传 glibc `B*` 常量（`UART_B_CONST` 映射，缺失/非法即失败，数值 Hz 会被 EINVAL 静默忽略）；读取按 1ms 步进轮询，`timeout_ms` 超 `UART_READ_MAX_MS`（60000）显式失败 |
| PWM | sysfs（`/sys/class/pwm/pwmchipN/pwmM`，ffi 直写、不经 io 库）；写序固定 duty=0 → period → duty_cycle（否则降频被 EINVAL 拒绝）；export 前置校验 npwm 范围，errno 给人话提示 |

**合并读长度上限**：`i2c_read` / `i2c_read_multi` / `i2c_read_reg16` / `spi_read_multi` 的大端合并值受 2^53 精度限制，n 有效范围 **1~6**，越界显式失败（静默钳制会被调用方的拆字节数学放大成错误数据）；更长块读取用字节表族 `i2c_read_bytes` / `i2c_read_multi_bytes`（n 钳 1~16，表长对调用方可见）或 `spi_xfer_full`（全双工帧，返回等长字节表）。

## 返回契约（驱动按此判定成败）

| 约定 | 函数 |
| ---- | ---- |
| `true` / `false` | `i2c_set_addr`、`i2c_write_reg`、`gpio_set_value`、`pwm_export`/`pwm_enable`/`pwm_set_polarity`/`pwm_unexport`（`pwm_config` 成功返回 `true, period_ns, duty_ns`） |
| `0` / `-1` | `i2c_write_multi`、`i2c_write_reg16`、`spi_setup`、`spi_write`、`spi_write3`、`spi_write_multi`、`uart_close`、`uart_flush` |
| 值 / `-1` | `i2c_read_reg`、`i2c_read_multi`、`i2c_read_reg16`、`i2c_read`、`spi_read`、`spi_read_multi`、`gpio_get_value`、`uart_open`（fd）、`gpio_request_line`（fd） |
| 表 / `nil` | `i2c_read_multi_bytes`、`i2c_read_bytes`、`spi_xfer_full`、`uart_read`（空表 = 超时无数据；`nil` = 读错误或参数非法） |
| 无返回值 | `i2c_close`、`spi_close`、`gpio_free_line`、`milli_sleep`；`uart_write` 返回写入字节数或 `-1` |

完整语义以 `modules/lib/HAL.lua` 各函数注释为准。`virtbus.lua` 逐函数复刻上述契约（含失败返回值），供无硬件 E2E 使用。

## 驱动沙箱

加载驱动**前**先中和 `os.execute`/`io.popen`（shell）、`os.exit`、`io.open`/`io.lines`/`io.input`/`io.output`/`os.remove`/`os.rename`（任意文件读写）——`luavar.cpp` 先调 `sandbox_lua` 再 `luaL_loadfile`。这是**防误用护栏，不是安全边界**——驱动可经 `require("ffi")` 直调任意 libc，脚本均为可信输入；接入 `--tcp` 外部数据源时需另行建立信任边界。

**信任边界（S1）**：`ffi.C` 即整个 libc——`ffi.C.system("...")`、`ffi.C.open(path, 0)` 一行即可绕过上述全部中和，`ffi.cast` 更可任意地址读写。因此**驱动脚本必须视为完全信任的代码**，沙箱仅防误写。当前威胁模型下成立：驱动/chunk 文件名只取自本地 `.sql`/`.mt` 表名，远程 TCP 下发的动作数据无法注入 Lua 加载路径；但若 `BOT4CJ_MODULE_PATH` 指向不可信目录、或未来新增远程选择驱动文件的入口，本护栏即失效（引入时须先补回归测试钉死）。另注：白名单前缀匹配不解析 symlink。

**dofile/loadfile 白名单（M3）**：`dofile`/`loadfile` 未被禁用而是**白名单化**——仅允许加载 `BOT4CJ_MODULE_PATH`（缺省 `./modules`，`:` 分段）内的路径，且路径不得含 `..`（目录穿越拒绝），越界加载明确报错（`sandbox: dofile/loadfile only allowed inside the module path ...`）。驱动/`@Daemon` chunk 按模块路径经 `mpath.resolve` 解析，正常使用不受影响；实现见 `src/luaend.cpp`。
