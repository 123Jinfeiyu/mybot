# HAL：基层硬件抽象

> 本文介绍 bot4cj 硬件抽象层 `modules/lib/HAL.lua` 的实现原理与驱动沙箱。[README](../README.md) 只留入口链接，实现细节以本文为准。

## 概述

总线怎么"开口说话"封装在 `modules/lib/HAL.lua`，所有驱动共用，**仅支持 LuaJIT**：

- ffi 直调 glibc，原生性能零 fork；fd 为真实内核句柄，跨 Lua 状态机有效
- I2C 经 `I2C_RDWR` ioctl **原子事务**：读的「写指针 + 读」合并为单次内核传输，消除并发访问同一器件时的寄存器指针竞态；地址由每条 i2c_msg 携带，不再需要 `I2C_SLAVE`（没有 EBUSY 问题）
- `modules/lib/` 还收纳 `kalman.lua` 等公共库；不依赖 HAL 的模块可用 `luajit`/`lua5.4` 独立调试

## 驱动沙箱

加载驱动后中和 `os.execute`/`io.popen`（shell）、`os.exit`、`io.open`/`io.lines`/`io.input`/`io.output`/`os.remove`/`os.rename`（任意文件读写）。这是**防误用护栏，不是安全边界**——驱动可经 `require("ffi")` 直调任意 libc，脚本均为可信输入；接入 `--tcp` 外部数据源时需另行建立信任边界。
