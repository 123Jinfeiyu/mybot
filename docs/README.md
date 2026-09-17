# bot4cj 文档索引

> 这里收录 README 之外的全部深入文档。初次接触请先看 [README](../README.md) 跑通快速开始，再按需深入。

## 使用与参考

- [命令行与环境变量参考](reference.md)——动作输入源（TCP/文件/stdin）、CLI 参数、安全约束、`BOT4CJ_*` 环境变量
- [DSL 参考：结构脚本与动作脚本](dsl.md)——占位符、引号字面量、`@Alias`/`@Vars`、`$(...)` 表达式、controller 与 `$` 引用完整语义
- [硬件变量（驱动）清单](drivers.md)——`modules/` 下 50+ 驱动按类别速查
- [已知限制与常见问题](faq.md)——平台/运行时边界与排障问答

## 机制深入

- [@Daemon 守护线程](daemon.md)——并发模型、万能采样 chunk、滤波器链路、采样→滤波→消费闭环
- [HAL 硬件抽象层](hal.md)——ffi 直调 glibc、I2C_RDWR 原子事务、驱动沙箱
- [虚拟总线 virtbus](virtbus.md)——无真机验证驱动链路的完整参考手册（含器件模型库与回归清单）
- [架构分析](architecture_analysis.html)——分层图与数据流图（**历史文档：仓颉版**，C++ 版架构见 [重构计划](refactoring-plan.md)）

## 测试与贡献

- [回归测试手册](testing.md)——守门分段明细、守门完整性规则、sanitizer 跑法
- [开发与贡献](development.md)——pre-push 钩子、新增驱动/回归/单测流程、代码规范
- [重构计划](refactoring-plan.md)——dev_cxx 结构重构分阶段方案(含分支归宿决策)
- [更新日志](changelog.md)
