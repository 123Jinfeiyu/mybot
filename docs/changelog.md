# 更新日志

> 从 [README](../README.md) 迁出，记录版本演进要点。

## v1.x（C++ 移植版）

- **审计廉价批**：未知/拼错的 `--key` 由「打印帮助 + exit 0」改为报错并以退出码 2 结束（`main.cpp` `isKnownOption`，守门补 `unknown_arg` 负值用例）——拼错 `--robot` 曾是 CI 假成功出口；MS5611 非法 `osr`（非数字 / <256 无档可降）由静默回落最慢档 4096 改为 build 期显式 ERR、且校验先于一切总线动作（test_ms5611.lua 补三条负值断言与零总线事务检查）
- **结构脚本限流 + 词法器线性化**：`--robot` 结构脚本与动作脚本统一 8 MiB 上限（共用 `kMaxScriptBytes` + `LimitedReader`，报错首词区分 `structure script` / `action script`；`ActionScriptTooLarge` 更名 `ScriptTooLarge` 并携带输入类别）——旧实现只有 `.mt` 有限流，超大 `.sql` 会把全部 token 驻留内存；词法器引号态改增量维护（`sqlparse.cpp`），消除逐字符重扫 token 的 O(n²)：100 KB 引号值 4.84 s → 毫秒级；新增 1 MB 引号值单测（旧实现按平方标度会撞 60 s 单测超时）与引号态等价性回归，守门补 `sql_toolarge` 负值用例
- UART 销毁恢复接口配置：HAL 在 open 时快照原始 termios，uart_close 先 tcsetattr 恢复再关闭（raw 模式/波特率不残留给后续使用者；恢复失败告警不阻断关闭），test_uart.lua 补恢复/失败分支回归
- **UART 波特率修复（真机实证）**：cfsetispeed/cfsetospeed 必须传 glibc 常量——旧实现传数值 Hz 被 EINVAL 拒绝且返回码未查，波特率实际从未生效（/dev/serial0 真机复验：请求 115200 后 B 字段仍为原值）；白名单扩 500000/1000000（SCSCL 出厂默认档），常量映射与白名单同步维护、缺失即明确失败
- **新增 `SCSCL` 总线舵机驱动**（飞特 FT-SCS 协议，UART）：build PING 探测与站号校验、call 位置/时间/速度/扭矩（0x2A 起六字节大端，运行时间=speed 0 时生效的到位耗时）、read 无副作用反馈块（0x38 起 11 字节）、destroy 扭矩关闭；call 另支持 `set_id` **站号烧写**（独占该次调用：解锁 EPROM 0x30=0 → 广播 0xFE 写 0x05，掉电重启生效；`set_id<=0`（首行 "-" 解析为 0）即不烧号，烧号行独立成行、真机须只接目标舵机）；尾部内嵌 `SCSCL_VIRT` 协议模型（帧/校验和/广播与站号过滤/寄存器地图/站号切换与 persisted 掉电语义/advance 运动学），test_scscl.lua 145 断言 + `scscl_virt`/`scscl_badid` 两条 @Action E2E 接入守门
- **非守护 virt 前置修复**：`drv_common` 原把 `*_VIRT` 工厂表直接 attach（工厂无 write/on_write → 写静默成功、读永远无应答，PCA9685 直写 E2E 的旧缺口），改为与 `sensor_daemon` 同约定先 `new()` 实例化；新增 `BUS="uart"` 模型按 tty 路径 `attach_uart` 与跨状态机 uart fd 采纳（UART 引用额外下发 `path`）
- 词法器引号感知：引号内 `--` / `/* */` / `;` / `,` / 括号 / 空白一律按字面数据解析（`sqlquote` 正负回归）
- `$` 引用指向标量（Alias）改为清晰报错而非 Json 类型崩溃（`ref_alias_scalar` 负值回归）
- E2E 守门落地：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化
- `deactivate` 同因告警去重；`scripts/run.sh` 启动器与文档一致性清理
