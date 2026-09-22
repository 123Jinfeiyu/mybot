# agent 部分重构方案

> 日期：2026-09-22 · 基线：`dev_agent` @ `6bffb3e`（P1 审计日志 / P2a 可取消 / P3 策略钩子已落地）
> 范围：`src/agent*.{hpp,cpp}`、`src/promptpack*`、`src/llmconfig*`、`src/agent_subprocess.*`、
> `modules/Llm*`、`modules/lib/llm_*`、`modules/Camera.lua`
> 前置依据：AG-1~26 各条修复的落地记录见 [docs/changelog.md](../changelog.md) 与 git 历史
> （AG-19/20/21/4′/22/23/24/25 均已落地），本方案按 HEAD 现状重新核定。
> 注：原引用的两份 `.tmp/` 审计报告已于 2026-09-22 核实清理时删除（问题均已闭环；
> 残留未修项见 `.tmp/NEXT_STEPS_20260922.md`）。

## 一、现状判断（不需要推倒重来）

agent 部分经 AG-1~26 多轮审计已打磨到位：分层干净
（`agentloop → agentproto → agentdriver → Lua 驱动 → llm_wire → llm_http`，宿主零 HTTP 依赖）、
`ChatTransport`/`AgentToolRegistry` 双抽象支持完全离线单测、安全防线系统化、守门 446/0
（`agent:` 130）。**本方案是定向收口，不是重构性重写。**

HEAD 相比旧审计基线新增了 `dispatchOneCall` 内的策略确认与审计记账——这正是下述 A1
拆分比旧计划更紧迫的原因。

## 二、待收口问题（按 HEAD 核实）

| 编号 | 问题 | 位置 | 级别 |
| --- | --- | --- | --- |
| R1 | `AgentSession` 职责叠加至四套：历史裁剪 / 任务内预算 / 策略确认+审计 / 轮次编排与回滚，392 行且是全仓注释最密、不变量最交织的文件 | [agentloop.cpp](../../src/agentloop.cpp) | 中 |
| R2 | 三个上下文字节预算（`maxHistoryBytes` / `maxTaskToolResultBytes` / `maxTaskImageBytes`）无 CLI / 配置入口，只能改源码缺省（docs/agent.md 已知限制自认） | [agentloop.hpp](../../src/agentloop.hpp#L33-L57) | 中 |
| R3 | `LlmOpenAI` 缺省 endpoint 指向特定第三方网关（moma.cmecloud.cn），"什么都没配"即外发上下文，仅 stderr 注记缓解 | [llm_wire_openai.lua](../../modules/lib/llm_wire_openai.lua) `DEFAULTS` | 中 |
| R4 | 单图峰值内存仍是原图数十倍（AG-19 实测 1.5MB→55MiB），payload 在「驱动缓冲→Lua 串→base64→Json→data URL→历史→请求体」各有一份副本；已收口拷贝点，剩传输层架构问题 | `agenttools.cpp` / `agentproto.cpp` / `llm_http.lua` | 中（大工程） |
| R5 | `agentmode.cpp`（578 行）混合配置组装、系统提示词构建、REPL 外壳三件事 | [agentmode.cpp](../../src/agentmode.cpp) | 低 |
| R6 | REPL 无 `/clear`：上下文被污染后唯一出路是重启，重启丢硬件常驻状态 | [agentmode.cpp](../../src/agentmode.cpp) | 低 |
| R7 | `runAgentMode` 本体零单测（REPL 循环、逐轮错误恢复、内置命令仅 E2E 间接覆盖）；`list_action_scripts` 结果里的磁盘首行注释未做「数据非指令」包裹（与系统提示词通道待遇不一致） | `agentmode.cpp` / `agenttools.cpp` | 低 |
| R8 | `trimHistory` 的 `messageWeight` 不计 system 提示词与工具 schema，注释声称的「压住请求体」严格说只对历史段成立 | [agentloop.cpp](../../src/agentloop.cpp#L67-L122) | 低 |

## 三、重构方案

### 阶段 A —— 结构收口（行为逐字不变，低风险）

**A1. 拆分 AgentSession（对应 R1）** —— ✅ 已落地（2026-09-22，存量用例零修改全绿 461/0、agent-all 7 段 0 失败）

- `HistoryManager`：`messageWeight` / `trimHistory` / 历史字节预算；独立可单测。
- `TaskBudget`：任务内文本/图片额度、单向累加、降级文案（含 AG-21/4′ 的三条边界语义），
  对外一个 `admit(result) -> Decision`。
- `dispatchOneCall` 的策略确认 + 审计记账抽为 `DispatchGate`（policy → execute → audit
  的固定顺序单点化），`AgentSession` 只留 `runTurn` 编排、`complete` 回滚与 `executeToolRound`。
- 验收：现有 `test_agentloop.cpp` 全部用例不改一个字仍绿；`bash test/run_all.sh agent-all` 全绿。

**A2. 拆分 agentmode.cpp（对应 R5）**

- 系统提示词构建（`quoteUntrusted` / `readHead` / 清单拼接 / `kMaxSystemPromptBytes` 上限）
  独立为 `agentprompt.{hpp,cpp}`；配置组装与 REPL 外壳留在 `agentmode`。
- 验收：`test_promptpack` / `agentmode_` / `agentconfig_` 作用域用例不动仍绿。

### 阶段 B —— 配置与性能

**B1. 上下文预算 CLI / 配置化（对应 R2）** —— ✅ 已落地（2026-09-22，配置文件化，464/0）

- 配置文件新增 `max_history_bytes` / `max_task_tool_result_bytes` / `max_task_image_bytes`
  （严格 JSON、未知字段拒绝，走现成 `llmconfig` 通道；`0` = 不限，与现语义一致）。
- CLI 可不加选项（选项面已 20+），只走配置文件；docs/agent.md「已知限制」相应段落移除。

**B2. 缺省 endpoint 收口（对应 R3）** —— ✅ 已落地（2026-09-22，破坏性变更，Lua 层 call 期 ERR）

- 目标：`LlmOpenAI` 无 endpoint 时不再静默回退第三方网关，改为 fail-closed 拒绝并提示
  配置方法；真要用该网关的人显式配置即可。DeepSeek 缺省（api.deepseek.com）是厂商官方
  域，可保留但同样打注记。
- 落地勘误：endpoint 只随 `call(data)` 到达驱动（`build(data)` 只收结构脚本参数），
  故 ERR 落在**首次模型调用的组包期**（驱动 pcall 折成 ERR 契约），而非 build 期——
  不为提前一轮失败把 endpoint 知识搬进宿主或参数表。
- 这是行为变更（破坏「零配置直跑」），需在 changelog 与 docs/agent.md 显著标注。

**B3. 图片传输层降副本（对应 R4，可选、大工程）** —— ✅ 已落地（2026-09-22，
`VmHWM` 实测 1.2MB 图 49MB→28.5MB、786KB 图 40MB→24MB，小图底噪 14.5MB，474/0）

- 前置：A1 完成后 `TaskBudget`/`HistoryManager` 已独立，才有安全改动面。
- 方向：图片以「游标/引用 + 请求体组装期落值」形态穿过 Json 与 Lua 边界，目标把
  同一 payload 的副本从 7 份降到 2~3 份；验收用 AG-19 的 `VmHWM` 实测法（786KB 图
  峰值 40MiB 应降到 ~15MiB 量级）。
- 若现场无小内存压力，可推迟。
- 落地：`buildDriverRequest`/`messageToJson` 增 `imageRefs` 出参，图片 url 换成
  `{"__bot_img": N}` 占位（字节不进请求 Json）；宿主经 `luavar.cpp` 的 `__bot_img(N)`
  闭包在**请求体组装窗口**内落值一次，`agentdriver` 用 `CursorScope` RAII 挂/摘
  （窗口外/越界/非数字一律 fail-closed，报错折成 ERR 契约）；驱动侧
  `llm_wire_openai.lua` 的 `resolve_image_url` 负责落值。
- 实测副产物（方案未预见）：`Camera.lua` base64 的 Lua 表项瞬态（1.2MB 图 ~160 万项
  ≈25MB，是差值主力）改用 `ffi.new("unsigned char[?]")` 预分配；`json.lua`
  `encode_string` 对无需转义的 base64 加 find 快路径（原 `gsub` 整串拷贝）。两处都
  是量级放大点，仅靠游标改造不足以达标。
- 未达初版目标口径（~15MiB）的原因：全程仍剩 3 份（存储落值 + 宿主→Lua 落值 +
  请求体组装），再压需动 libcurl 写回调；当前收益已足够，不再下探。

### 阶段 C —— 能力补齐（小而确定，按需）

- **C1. REPL `/clear`（R6）** —— ✅ 已落地（2026-09-22，保留 system，465/0）：裁掉 system 之后的全部历史（复用 `HistoryManager` 的
  任务块语义），硬件不释放；与「无记忆持久化」的现有边界一致。
- **C2. `list_action_scripts` 结果包 `quoteUntrusted`（R7）** —— ✅ 已落地（2026-09-22，含钉子用例）：一行级改动，对齐两条
  磁盘内容通道的定界待遇。
- **C3. `runAgentMode` 单测（R7）** —— ✅ 已落地（2026-09-22，`runAgentRepl` 拆出
  + 六用例，472/0）：把 REPL 循环依赖的 I/O 收成接口后补循环/命令/
  错误恢复用例（现由 agent_e2e.sh 六场景兜底，非阻塞）。
- **C4. `messageWeight` 计入 system 与 schema（R8）** —— ✅ 已落地（2026-09-22，
  按方案倾向改注释口径诚实 + 口径钉子用例，466/0）：或把注释口径改诚实（二选一，
  倾向后者——system 本就有 64KiB 上限独立约束）。

### 不做（显式声明）

- 记忆持久化、流式响应、新协议驱动：agent 能力面已覆盖「大模型控制常驻机器人」核心
  场景，扩展即增攻击面，违背本项目「不过度设计」纪律。
- `agentmode` 配置组装再拆：字段虽多但同源（CLI/配置/环境三通道合一），拆开反而增加
  搬运点。

## 四、实施纪律（沿用项目既有闭环）

1. 每步一个 commit，行为保持型改动（A1/A2）以「存量用例零修改」为验收线。
2. 行为变更型改动（B1/B2/C1/C2）先补钉住新语义的用例，再做反向验证（注入破坏转红）。
3. 每阶段收口后跑：`bash test/run_all.sh agent-all` + `./build/mybot_tests`（全量）
   + ASan 产物下 `bash scripts/agent_e2e.sh`。
4. 文档（docs/agent.md、changelog.md）与实现同 commit 更新，保持「文档同步」基线。

## 五、顺序与依赖

```
A1 ──► A2 ──► B1 ──► B2 ──► (B3 可选)
 │
 └────► C2（随时可做）──► C1 ──► C3 / C4
```

建议单批提交粒度：A1、A2、B1、B2、C2+C1、C3+C4 各一批；B3 单独立项评估。
