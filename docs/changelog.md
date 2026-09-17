# 更新日志

> 从 [README](../README.md) 迁出，记录版本演进要点。

## v1.x（C++ 移植版）

- UART 销毁恢复接口配置：HAL 在 open 时快照原始 termios，uart_close 先 tcsetattr 恢复再关闭（raw 模式/波特率不残留给后续使用者；恢复失败告警不阻断关闭），test_uart.lua 补恢复/失败分支回归
- 词法器引号感知：引号内 `--` / `/* */` / `;` / `,` / 括号 / 空白一律按字面数据解析（`sqlquote` 正负回归）
- `$` 引用指向标量（Alias）改为清晰报错而非 Json 类型崩溃（`ref_alias_scalar` 负值回归）
- E2E 守门落地：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化
- `deactivate` 同因告警去重；`scripts/run.sh` 启动器与文档一致性清理
