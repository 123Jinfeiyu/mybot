# 更新日志

> 从 [README](../README.md) 迁出，记录版本演进要点。

## v1.x（C++ 移植版）

- 词法器引号感知：引号内 `--` / `/* */` / `;` / `,` / 括号 / 空白一律按字面数据解析（`sqlquote` 正负回归）
- `$` 引用指向标量（Alias）改为清晰报错而非 Json 类型崩溃（`ref_alias_scalar` 负值回归）
- E2E 守门落地：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化
- `deactivate` 同因告警去重；`scripts/run.sh` 启动器与文档一致性清理
