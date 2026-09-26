# ICA 原型接入 Rust Agent

本分支以最新两进程架构为基础：`mybot --mcp-backend` 仍是 C++ 控制器，
`mybot-agent` 是 Rust 模型循环。ICA 证据账本运行在 Rust Agent 中；控制器的硬件
调用仍经原有 AF_UNIX socket 和动态工具表。远程 `dev_agent` 仓库未修改。

## 启动

构建两端后，启动 C++ 控制器，再用 `BOT4CJ_ICA=1` 启动 Rust Agent。
启用时，Rust Agent 在控制器工具之外注册七个本地 ICA 工具：
`ica_propose_claim`、`ica_propose_future`、`ica_compare_future`、
`ica_cite_observation`、`ica_depend`、`ica_retract_observation`、`ica_status`。
不设该环境变量时，控制器和 Rust Agent 的原有工具流程不变。

Windows 本机的双进程脚本为 `D:\ICA-Agent\launch-rust-agent.cmd`。脚本在
当前窗口提示输入 DeepSeek API Key，C++ 控制器只收到硬件配置，不收到模型密钥。

## 原型的事实边界

- 用户任务登记为 `user` 观察；“用户说了 X”只能证明收到了这段输入。
- 控制器工具回执登记为观察。成功回执可被引用；错误或未执行的回执标为
  `tool_error`，不能充当事实支持来源。
- 同名工具的回执保守地算一个来源。复制同一来源不能凑足独立支持数。
- 主张先为 `locked`；一份独立工具来源为 `partial`；两份来源为
  `needs_review`；反向证据为 `contested`。没有把来源数量当成事实验证：模型
  无权将状态改为 `supported_in_scope`。
- 假设、计划、未知缺口和行动建议只作为未来候选记录。登记和暂定比较都不会
  执行硬件动作，也不等同于未来已获证实。
- 撤回观察会令对应引用和未来比较失效，并把依赖相关主张的下游主张标为
  `stale`。历史记录保留。
- 模型每轮完成后的 ICA 注记显示当前主张核验状态；它不是逐句自动事实判定。

开启 `--session-file` 时，ICA 账本另存为 `<会话文件>.ica.json`，权限 0600。
恢复会话要求该文件存在且其记录的会话文件长度与当前文件一致；工具表和系统
提示词指纹仍由原有会话恢复逻辑检查。`/clear` 同时清空 ICA 账本。

## 限制

当前通用工具只有工具名，没有网页或传感器的稳定来源身份；按工具名归并可能
把不同来源合在一起，也不能确认工具文本内容的真实性。后续需要在来源适配器
中提供稳定 `origin_id`，再由可信宿主检查证据内容、范围与相关性。ICA 目前是
可检查的比赛原型，不是 100M 模型、自主科学发现或完整的 Self 神经场。

Rust 单元测试覆盖复制来源、用户输入不能验证主张、观察撤回与依赖失效、会话
账本长度校验。双进程连接及模型工具循环由项目现有端到端脚本检查。
