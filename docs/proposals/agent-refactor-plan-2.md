# agent 部分重构方案（第二轮）

> 日期：2026-09-22 · 基线：`dev_agent` @ `e075139`（上一轮 A1/A2/B1/B2/B3/C1–C4 全部已落地）
> 范围：`src/agent*.{hpp,cpp}`、`src/agent_subprocess.*`、`src/llmconfig.*`、`docs/agent.md`、`docs/reference.md`
> 前置：`docs/proposals/agent-refactor-plan.md`（第一轮，已全部收口）。本文档为**新一轮**审计，
> 所有结论按 HEAD 逐行核对（引用格式 `文件:行号`），不是对旧审计的转述。
> 上一轮已显式声明的不做项（记忆持久化 / 流式 / 新协议驱动 / 配置组装再拆）在本文档继续有效。

## 一、现状判断

第一轮收口后，agent 部分的结构已经稳定，**不需要推倒重来**：

- 分层：`AgentSession`（编排）→ `ChatTransport`（`agentdriver` 适配 Lua 驱动）→ `modules/Llm*.lua`
  → `modules/lib/llm_*.lua`（协议组包）→ `llm_http.lua`（libcurl FFI）。宿主零 HTTP 依赖。
- 四个单点：`HistoryManager`（历史裁剪）/ `TaskBudget`（任务内预算）/ `DispatchGate`
  （策略 → 执行 → 审计）/ `agentprompt`（系统提示词构建）。
- 测试面：`mybot_tests` 的 `agent:` 作用域（8 个测试文件）+ `test_agent*.lua` + `agent_e2e.sh`
  六场景 + 加固段 `agent_`，入口 `bash test/run_all.sh agent-all`（`test/run_all.sh:68-73`）。
  基线实测（2026-09-22，本方案撰写时现跑）：C++ 全量 **474 通过 / 0 失败**，
  `agent:` 作用域 **157 通过 / 0 失败**。

本轮是**定点收口**：重点是两处真实的健壮性/一致性缺口（F1/F2），其次是配置面残留（F3）
与若干低风险小项（F4–F6）。

## 二、待收口问题（核实于 HEAD `e075139`）

| 编号 | 问题 | 位置 | 级别 |
| --- | --- | --- | --- |
| F1 | 策略确认程序的回收**无界**：超时后 `waitpid(pid,&status,0)` 无超时阻塞（`retryEintr` 包着，SIGINT 只会被重试），kill 未生效时 agent 会话永久挂死 | [agentpolicy.cpp:116-125](../../src/agentpolicy.cpp#L116-L125) | **高** |
| F2 | 两份「fork + exec + 超时 + 进程组终止」实现重复且已漂移，防线性差异只在 `runShell` 一侧 | `agent_subprocess.cpp` / `agentpolicy.cpp` | 中 |
| F3 | 四个运行期数值无配置入口：`maxCallsPerRound` / `maxRetries` / `retryBackoffMs` / `run_command` 超时 | [agentloop.hpp:54](../../src/agentloop.hpp#L54)、[agentdriver.hpp:29-31](../../src/agentdriver.hpp#L29-L31)、[agenttools.hpp:78-79](../../src/agenttools.hpp#L78-L79) | 中 |
| F4 | 传输层重试日志硬编码 `std::cerr`，不经注入的日志流，审计轨迹也看不到「重试了几次」 | [agentdriver.cpp:88](../../src/agentdriver.cpp#L88) | 低 |
| F5 | 动作脚本清单重复扫盘：配置预检读一次即丢弃，提示词构建再读一次；REPL `/scripts` 与工具各再遍历一次 | [agentmode.cpp:307-319](../../src/agentmode.cpp#L307-L319)、[agentprompt.cpp:72](../../src/agentprompt.cpp#L72)、[agenttools.cpp:399-400](../../src/agenttools.cpp#L399-L400) | 低 |
| F6 | 三条未被覆盖/可疑的小路径（见下） | 各处 | 低 |

### F1 详述（本轮唯一「真缺陷」级）

`runConfirmProgram` 的超时分支先整组 `SIGKILL` 再回收（[agentpolicy.cpp:119-122](../../src/agentpolicy.cpp#L119-L122)），
但回收用的是 `waitpid(pid, &status, 0)` —— **无超时**。若 kill 未生效（子进程处于 D 态、
受限容器里的权限异常），这一行会永久阻塞；且它被 `retryEintr` 包着，Ctrl-C 只会触发 EINTR
重试，**连信号都打断不了**。唯一出路是二次 Ctrl-C 强杀，而强杀会跳过 `deactivate()`，
把执行器留在最后指令位置——这正是 `runShell` 注释里明确要避免的失效形态。

对照实现：`runShell` 的同类路径用 `reapWithGrace`（WNOHANG 轮询到 2s 宽限，放弃回收时
按 `reapGiveUpWarning` 响亮告警），[agent_subprocess.cpp:163-176](../../src/agent_subprocess.cpp#L163-L176)
的注释写着「绝不无超时阻塞——……会把 agent 会话永久挂死」。两条路径的判据来自同一次审计，
但只有一个落到了代码里。

同一函数还缺 `runShell` 侧已有的两道加固：`markAllFdsCloexec` + 子侧 `revokeInheritedFds`
（[agent_subprocess.cpp:385](../../src/agent_subprocess.cpp#L385)、[:297](../../src/agent_subprocess.cpp#L297)），
即确认程序仍会继承宿主全部 ≥3 的 fd（HAL 句柄已是 `O_CLOEXEC`，风险有限，但「碰巧安全」
不该是本项目的口径）。

### F2 详述（重复与漂移）

两份实现共同骨架约 100 行：`pipe2` → `fork` → `setpgid` → 写 stdin（非阻塞 + poll +
局部屏蔽 SIGPIPE）→ `WNOHANG` 轮询等退出 → 超时整组 `SIGKILL`。差异（**只有 runShell 有**）：

| 防线 | runShell | runConfirmProgram |
| --- | --- | --- |
| 有界回收（防永久挂死） | ✅ `reapWithGrace` 2s + 告警 | ❌ 无超时 `waitpid` |
| 子进程 fd 吊销 | ✅ 父侧全量 + 子侧 `close_range`/退化循环 | ❌ 无 |
| 环境清洗 | ✅ `buildCleanEnvp` | ❌ `execl` 继承全量 `environ`（含宿主注入的 `BOT4CJ_LLM_KEY`） |
| 输出截断 + UTF-8 消毒 | ✅ 64KB/流 | ❌ 不捕获输出（继承宿主终端） |
| 硬上限（kill 不生效时不卡死） | ✅ `hardDeadline` + `hardStopped` 如实报告 | ❌ 无 |
| 审计可见的失败形态 | ✅ `RunResult` 三态 | 部分（timedOut/失败原因） |

**环境一项需要显式决策而不是顺手统一**：确认程序是操作者自己写的可信程序（审批方案已声明
「不是安全边界」），且合法的确认程序完全可能依赖自己的 `…_TOKEN` 类环境变量——默认套用
`buildCleanEnvp` 会**静默砸掉**这类脚本。故本方案主张：保持 inherit 缺省，把差异写成
**显式选项 + 文档声明 + 钉子用例**，而不是继续「碰巧不同」。

### F6 详述（三条小路径）

1. `escapeLogMarkers`（[agentloop.cpp:34](../../src/agentloop.cpp#L34)）在匿名命名空间，仅经
   verbose 日志间接覆盖，无直接用例。
2. `call_capped` 是**死分支**（[agentloop.cpp:214](../../src/agentloop.cpp#L214)）：
   `maxCallsPerRound > 0` 时 `calls` 已被 `resize` 到非空，`calls.empty()` 不可能成立。
   「注释/代码声称的路径从未执行」在本仓已被记为最难发现的一类缺陷（AG-3 前科）。
3. 确认程序「不读 stdin」的写路径（EPIPE / 写超时，[agentpolicy.cpp:100-102](../../src/agentpolicy.cpp#L100-L102)）
   无用例；现有 7 例覆盖 allow/deny/signal/timeout/遮蔽，未覆盖这条。

## 三、重构方案

### 阶段 A —— 健壮性收口

**A1. 子进程运行器单点（对应 F1/F2）** —— 建议**单独一个 commit 先行**

- 在 `agent_subprocess` 抽出统一骨架（形如 `runChild(spec)`），两个调用点共用：
  `pipe2`/`fork`/`setpgid`/写 stdin/轮询/整组 `SIGKILL`/`reapWithGrace`。
- 差异用**显式选项**表达，不靠调用方各自实现：
  `envp`（clean | inherit）、`stdin`（payload | devnull）、`stdout/stderr`（capture | inherit）、
  `timeoutSec` / `graceMs`、`fdRevoke`（on | off）、`captureOutput`（是否截断 + 消毒）。
- `runShell` 侧**行为逐字不变**（选项取现状值）；确认路径获得有界回收与 fd 吊销（F1 修复）。
  这是**行为变更**：从「可能永久挂死」变为「2s 宽限后放弃回收 + 响亮告警」。
- 验收：
  - `runShell` 侧存量用例（`test_agenttools.cpp` 的 AG-11 探针、截断、超时、中断）**零修改全绿**；
  - 新增有界回收用例：宽限内正常回收、`waitpid` 返回 ECHILD 时立即返回不阻塞；
    D 态不可构造（沿用 `reapGiveUpWarning` 的既有处置）→ 以「宽限参数 + 告警文案纯函数」钉住；
  - 新增确认路径用例：`reap` 走通 + 告警文案与 `reapGiveUpWarning` 同源。

**A2. 确认程序一致性补齐（对应 F2 的环境/fd 决策）**

- fd：复用父侧 `markAllFdsCloexec` + 子侧 `revokeInheritedFds`。
- 环境：**保持 inherit 缺省**，理由写进代码注释与 `docs/agent.md`（防将来被「顺手统一」
  改成清洗，砸掉合法的通知脚本）；新增钉子用例断言「确认程序环境里能看到
  `BOT4CJ_LLM_KEY`」——把这条**决策**钉住，而不是把现状钉死。
- 验收：`test_agentpolicy.cpp` 现有 7 例不动仍绿 + 新用例。

### 阶段 B —— 配置面补齐（对应 F3）

**B4. 四个数值进配置文件通道**（沿用 B1 的「配置文件 > 编译期缺省、严格 JSON、未知字段拒绝」）

| 字段 | 区间 | 说明 |
| --- | --- | --- |
| `max_calls_per_round` | 1..1024，**拒绝 0** | 单轮向物理机器人下发动作数的上限。`0`（不限）不开放：与 `allow_shell` 同源的「能力只能命令行开、配置文件只能收紧」口径——放宽到 1024 已覆盖任何现实场景，而「无界」没有正当用法 |
| `max_retries` | 0..6 | 0 = 不重试（离线/内网直连场景想立刻失败时用） |
| `retry_backoff_ms` | 0..10000 | 同时给退避加**单次上限**（如 60s）：`backoff << attempt` 在放宽的区间下会算出小时级等待 |
| `run_command_timeout_sec` | 1..600 | 现硬编码 10（[agenttools.hpp:78](../../src/agenttools.hpp#L78)） |
| `run_command_max_timeout_sec` | 1..3600，且 ≥ 前者 | 现硬编码 60，是模型可请求的上限 |

- 链路：`llmconfig`（严格解析 + 区间单点进 `llm_limits`）→ `AgentModeConfig` 透传 →
  `AgentOptions` / `AgentDriverConfig` / `AgentToolContext`。
- 仅走配置文件，不加 CLI 选项（选项面已 20+，与 B1 同款理由）。
- 验收：`test_llmconfig.cpp` 新增区间/类型/未知字段用例；`agent-all` 全绿；
  `docs/agent.md` 字段表 + `docs/reference.md` 同步。

### 阶段 C —— 小项（低风险，可合批）

- **C5. 传输层日志注入（F4）**：`makeDriverTransport`/`AgentDriverConfig` 增加日志 sink
  （缺省 stderr = 现状，行为保持）；重试信息进 `--verbose` 日志流与 REPL 日志。
  **不改审计 JSONL schema**（见「不做」）。
- **C6. 脚本清单单次读取（F5）**：`buildAgentConfig` 预检读到的结构脚本头与脚本清单存进
  `AgentModeConfig`，`buildAgentSystemPrompt` 优先复用（空则回退实时读取——离线单测手搓
  配置仍可用）。收益：每会话少一次全树遍历 + N 次 4KB 读，且「预检口径」与「提示词口径」
  按构造一致（现在两次读之间文件若变动会分叉）。
- **C7. F6 三条小路径**：`escapeLogMarkers` 挪到可测位置（或补间接用例）；`call_capped`
  死分支删除；确认程序 EPIPE 路径补用例。

### 不做（显式声明）

- **不改审计 JSONL 的 schema**（不加 `retries` / `confirm_ms` 字段）：审计是 append-only 且
  有外部消费方，字段演进走新提案；C5 只让重试可见于日志流。
- **不把确认程序的环境默认改为清洗**：理由见 A2（确认程序是操作者可信程序，清洗会静默
  砸掉依赖自有 TOKEN 的合法脚本）。
- 不拆 `agentmode` 的配置组装（第一轮已声明，本轮无新证据）。
- 不拆 `agenttools.cpp` 的图片段（体积判据 / 魔数 / MIME 约 150 行同源逻辑，拆开只增搬运点）。
- 不做流式响应、记忆持久化、P2b 取消通道（沿用第一轮口径：扩展即增攻击面）。

## 四、实施纪律（沿用项目既有闭环）

1. 每步一个 commit；行为保持型（A1 的 runShell 侧、C6）以「存量用例零修改」为验收线。
2. 行为变更型（A1 的确认路径、A2、B4、C5、C7）先补钉住新语义的用例，再做反向验证
   （注入破坏应转红）。
3. 每阶段收口后跑：`bash test/run_all.sh agent-all` + `./build/mybot_tests`（全量）
   + ASan 产物下 `bash scripts/agent_e2e.sh`。
4. 文档（`docs/agent.md`、`docs/reference.md`、`docs/changelog.md`）与实现同 commit。

## 五、顺序与依赖

```
A1（F1 修复，单独一批）──► A2 ──► B4 ──► C5 / C6 / C7（可合批）
```

- A1 是唯一「真缺陷」级项，且是 A2 的前置（骨架收敛后一致性补齐才有落点）。
- C6 与 A 阶段无耦合，可与 B4 并行合批。

## 附：本轮审计方法与盲区

- 全部结论来自 HEAD 逐行核对（文中均给出 `文件:行号`）；基线数字为撰写时实测
  （C++ 全量 474/0、`agent:` 157/0，Release 构建）。
- 未覆盖的盲区如实列出：① D 态子进程无法在离线环境构造（沿用既有处置，只钉宽限与文案）；
  ② 图片传输的剩余副本（B3 已收口，再压需动 libcurl 写回调，收益不抵复杂度）；
  ③ 策略确认程序与宿主同权限，本身**不是**安全边界（`docs/agent.md` 既有口径不变）。