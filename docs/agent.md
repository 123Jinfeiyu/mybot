# agent 模式（--agent）

> **一句话**：管道模式是「给它一份剧本，它照着演一遍就下台」；agent 模式是
> 「**让它一直站在台上**，由大模型看着可用剧本，一步一步决定下一段演什么」。

## 与管道模式的差别

| | 管道模式（默认） | agent 模式（`--agent`） |
| --- | --- | --- |
| 主流程 | 读一段 `.mt` → 逐行重放 → 退出 | 大模型工具循环，多轮决策后才退出 |
| 硬件生命周期 | `active()` → `run()` → `deactivate()`，一次性 | `active()` → **常驻** → 会话结束才 `deactivate()` |
| 动作脚本数量 | 一次一段（可用 mtmerge 合成） | 任意多段，逐个由智能体挑选执行 |
| 输入源 | `--tcp` / `--input` / stdin | `--task` 一次性，或 stdin 交互 REPL |
| 决策者 | 写脚本的人 | 大模型（受提示词与工具边界约束） |

两种模式**互斥**：`--agent` 与 `--tcp`/`--input` 同时给会被显式拒绝（退出码 2）。

## 快速开始

无硬件自检链路（推荐第一条命令）：

```bash
export BOT4CJ_LLM_KEY=sk-xxx
./build/mybot --robot=robots/relay-test.sql --agent --task "让继电器动作一次"
```

蜘蛛机器人交互式控制：

```bash
./build/mybot --robot=robots/spider1.sql --agent
mybot> 站起来
mybot> 先前走两步，然后向左转
mybot> /exit
```

REPL 内置命令：`/hardware` 打印硬件清单，`/scripts` 打印动作脚本清单，`/exit`（或 `/quit`、Ctrl-D）退出。
Ctrl-C 走正常清理链：当前轮中断、机器人 `deactivate()` 去初始化后进程才退出。

## 命令行参数

| 参数 | 说明 | 缺省 |
| --- | --- | --- |
| `--agent` | 开启 agent 模式 | 关 |
| `--task <文本>` | 一次性任务，跑完即退出；不给则进交互 REPL | — |
| `--scripts-dir <目录>` | 动作脚本根目录，也是智能体可执行路径的**沙箱边界** | `scripts` |
| `--llm-endpoint <URL>` | OpenAI 兼容接口地址 | `https://moma.cmecloud.cn/v1/chat/completions` |
| `--llm-model <名称>` | 模型名 | `zhipu/glm-5.3-flash` |
| `--llm-key <KEY>` | API key（不推荐，见安全说明） | 环境变量，见下 |
| `--llm-max-turns <N>` | 单次任务的最大模型轮数（1-100） | 8 |
| `--llm-max-tokens <N>` | 单次生成最大 token 数 | 4096 |
| `--llm-timeout <秒>` | 单次模型调用超时 | 180 |
| `--agent-no-shell` | **不**注册 `run_command`（去掉任意 shell 执行能力） | 关（即默认提供） |

采样参数固定为 `temperature=0.7`、`top_p=0.9`（非流式，`stream=false`）。

`--task` / `--llm-*` / `--scripts-dir` / `--agent-no-shell` 出现在**非** agent 模式下会被拒绝（退出码 2），不会静默忽略。

API key 取值顺序：`--llm-key` → 环境变量 `BOT4CJ_LLM_KEY` → `ROBOT_AGENT_API_KEY` → `DEEPSEEK_API_KEY`。
推荐用环境变量或 `_run.sh` 之类的启动脚本注入：`--llm-key` 会出现在 `/proc/<pid>/cmdline`，本机所有用户可读。

## 工作流程

```text
mybot --robot=X.sql --agent [--task "..."]
  ├─ 解析结构脚本（失败即退出，不上电）
  ├─ 组装 agent 配置 + 建 HTTP 传输（缺 key / 缺库在此报错，仍不上电）
  ├─ robot.active()                     ← 硬件上电并【常驻】
  └─ agent 工具循环
       ├─ 系统提示词 = 角色 + 结构脚本 + 硬件清单 + 动作脚本清单 + 行为约束
       ├─ user 消息 = 任务
       ├─ 模型 → tool_calls → 派发工具 → 结果作为 tool 消息回传 → 再问模型
       │    （循环，最多 --llm-max-turns 轮）
       └─ 模型给出纯文本 → stdout 输出最终回答
  └─ robot.deactivate()                 ← 会话结束才去初始化
```

- **stdout 只输出最终回答**（末轮文本）；模型过渡语、工具调用与工具结果全在 stderr，便于脚本管道处理。
- **轮次上限/传输失败会回滚本轮消息**：半截的 `tool_calls` 序列会让下一轮请求被服务端 400 拒绝，REPL 因此可以继续下一条任务。
- REPL 的会话历史跨轮保留（机器人「记得」之前做过什么），因此上下文会随任务数增长；长会话建议适时重启。

## 给智能体的工具

| 工具 | 作用 | 关键约束 |
| --- | --- | --- |
| `list_action_scripts` | 列出 `--scripts-dir` 下全部 `.mt`（相对路径 + 首行说明） | 只读 |
| `run_action_script` | 在**常驻**机器人上执行指定 `.mt` | 只允许 `--scripts-dir` 下的相对路径；拒绝绝对路径、`..`、非 `.mt`；单文件 8 MiB 上限 |
| `list_hardware` | 列出结构脚本注册的硬件变量（名字 + 种类） | 只读清单；**不**调用 `read()`/`play()`，绝无副作用 |
| `run_command` | 在主机上执行 shell 命令（退出码 + stdout + stderr） | 超时（缺省 10s，上限 60s，超时整进程组 SIGKILL）、单流 64KB 截断、环境变量清洗；`--agent-no-shell` 可关闭 |

`run_action_script` 执行失败（脚本名错、硬件未接、解析错误）不会中断会话：报错文本作为工具结果回传模型，由模型决定换个做法还是如实汇报。

## 安全说明（请务必读）

1. **`run_command` 是无白名单、无审批的任意 shell 执行**。模型可能被外部输入（文件内容、设备读数、网络数据）诱导执行任意命令。
   仅在可信环境（你自己的机器人主机）使用；需要对外暴露，请先加命令审批或在进程外隔离（专用用户 / 容器 / 网络命名空间）。
   不需要 shell 能力时加 `--agent-no-shell`。
2. **子进程环境已清洗**：名称含 `key` / `token` / `secret` / `password` / `passwd` / `credential` 的变量不会传给子进程——
   这天然覆盖 `BOT4CJ_LLM_KEY`，模型无法用 `run_command` 读走自己的 API key。
3. **动作脚本路径是沙箱边界**：`run_action_script` 只接受 `--scripts-dir` 下的 `.mt` 相对路径。这是防误用护栏，
   不是对抗恶意模型的边界——真正的边界仍然是「模型输出即不可信输入」这条工程常识。
4. **仪表器件不会被工具误触发**：`list_hardware` 不读值（对舵机等执行器，`play({})` 会真的下发指令）。
5. **上电时机**：配置错误、缺 key、缺 libus_http 都在 `active()` 之前报错退出，不会出现「带电空等」。

## 构建（可选依赖）

agent 模式的唯一外部依赖是 **libus_http**（OpenAI 兼容 HTTP 客户端：libcurl + OpenSSL + header-only Boost.Beast）。
CMake 自动探测：

- **找到** `us/HttpClient.hpp` + `libus_httpclient.so` → 编译真实传输，`--agent` 可用；
- **找不到** → agent 源码照编，但 `--agent` 会明确报「本构建未启用 agent 模式」并退出 2。
  CI 基线 `debian:bookworm` 没有该库，这条降级路径让「零第三方依赖」承诺与 CI 全绿同时成立。

```bash
# 默认：自动探测
cmake -S . -B build && cmake --build build

# 显式关闭（连探测都不做）
cmake -S . -B build -DMYBOT_AGENT=OFF

# 非系统安装位置：指向源码树即可（先在该仓库 make）
# 探测提示路径：../libus_http/inc 与 ../libus_http/build
```

配置结果会打印在 cmake 输出里：`agent mode: ON (libus_http: ...)` / `agent mode: OFF (...)`。

## 源码分层

| 文件 | 职责 | 是否依赖 libus_http |
| --- | --- | --- |
| `src/jsonparse.{hpp,cpp}` | 严格 JSON 文本解析（外部 JSON 首次入口） | 否 |
| `src/agentproto.{hpp,cpp}` | OpenAI 兼容消息/工具编解码、`ChatTransport` 抽象 | 否 |
| `src/agenttools.{hpp,cpp}` | 工具注册表与四个工具、脚本路径沙箱、硬件描述 | 否 |
| `src/agentloop.{hpp,cpp}` | 工具循环（消息历史、轮次上限、回滚） | 否 |
| `src/agentmode.{hpp,cpp}` | CLI 配置组装、系统提示词、一次性/REPL 外壳 | 否 |
| `src/agenthttp.{hpp,cpp}` | `ChatTransport` 的 libus_http 实现（含退避重试） | **是**（`MYBOT_HAVE_AGENT`） |

除 `agenthttp.cpp` 外全部与传输无关，因此工具循环可以完全离线单测（注入脚本化传输）。

## 测试

```bash
bash scripts/agent_e2e.sh        # 离线端到端：假 OpenAI 服务 + 真实工具循环
./build/mybot_tests agent        # C++ 单测：proto / tools / loop
```

- `test/cpp/test_agentproto.cpp`、`test_agenttools.cpp`、`test_agentloop.cpp`、`test_jsonparse.cpp`
  覆盖请求组包、响应解析、工具派发、轮次上限回滚、中断、路径沙箱、环境清洗、超时截断。
- `scripts/agent_e2e.sh` 用一个**本地假模型服务**（`scripts/fake_llm_server.py`）驱动
  `list_hardware → run_action_script → run_command → 最终回答` 的完整循环，断言动作脚本真的被执行。
- 全量守门 `bash test/run_all.sh` 已包含该段；本构建未启用 agent 模式时按 **SKIP** 计入
  「未执行清单」（可见，不假绿）。

## 已知限制

- **非流式**：`stream=false`，长回答要等整轮生成完；接口超时用 `--llm-timeout` 调整。
- REPL 历史无自动裁剪，长会话上下文持续增长。
- 无记忆持久化（重启即忘）。
- 只支持对话式工具调用协议（OpenAI 兼容 `tools` / `tool_calls`），未适配其它厂商私有协议。
