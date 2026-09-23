# agent 部分开发计划（第三轮：定点收口）

> **落地状态（2026-09-23）：G1 / G2 / G3 全部完成**（G3 实测撤销修复后新用例转红）。
> C++ 单测 **659 / 0**、`-Werror` 零告警构建通过；`bash test/run_all.sh agent-all` 因本机未装
> luajit 无法运行（环境限制）。
>
> 日期：2026-09-23 · 基线：`dev_agent` @ `0104a1a`
> 范围：`src/agent_subprocess.*`、`src/agenttools.*`、`src/agentaudit.*`、`src/agentmode.*`、`docs/agent.md`、`docs/reference.md`
> 前置：`docs/proposals/agent-refactor-plan-2.md`（第二轮，A1/A2/B4/C5/C6/C7 已全部落地）。
> 本计划为**新一轮**定点收口：按 HEAD 逐行核对，不做对旧审计的转述。

## 一、现状判断

第二轮收口后，agent 部分的骨架已经稳定，`runChild` 统一子进程骨架、运行期数值全部进
配置文件、日志注入、脚本清单单次读取均已落地；后续又叠加了新能力（`query_sql` /
`apply_sql` 热插表、REPL `/status`、审计的 session/model 记录、`/clear`、PL-2 动作脚本
执行预算）。分层与单点仍然成立，**不需要推倒重来**。

本轮聚焦两处真实缺口（G1 是安全面，G2/G3 是审计面），一处复核（G3）。

## 二、待收口问题（核实于 HEAD `0104a1a`）

| 编号 | 问题 | 位置 | 级别 |
| --- | --- | --- | --- |
| G1 | `run_command` 回传给模型的 stdout/stderr **未遮蔽敏感值**。`formatRunResult` 原样拼接命令输出（含 `KEY=val` 形态），`agenttools.cpp` 直接把该文本回传模型上下文。`maskSecretsInText` 只用于**审计前的命令原文**（agentpolicy.cpp:98）与策略确认入参，工具结果这条通道漏了。allowShell 开启（本就在关与开之间，模型可驱动）且模型执行如 `env` / `cat mybot.llm.json` 时，密钥可再生进上下文并经同一配置 endpoint 外发。 | [agent_subprocess.cpp:615-631](../../src/agent_subprocess.cpp#L615-L631)、[agenttools.cpp:830](../../src/agenttools.cpp#L830) | **高** |
| G2 | `apply_sql`（热插表，与 `run_command` 同级的高危工具，能新增硬件并当场派发 Action）的审计**看不出生效了什么**：`AuditEntry` 只记 `tool` 名与 `args`，而 `args` 是经过 `maskToolArgs` 的（`apply_sql` 不在遮蔽名单，原样是 SQL 文本）——回执摘要（新增几个变量/表/行、瞬态动作几行）没有进审计，复盘时无法对账「这次热插到底改了什么」。 | [agentaudit.hpp:30-52](../../src/agentaudit.hpp#L30-L52)、[agenttools.cpp:666-673](../../src/agenttools.cpp#L666-L673) | 中 |
| G3 | 会话结束记录覆盖全面性复核：`runAgentMode` 的一次性任务（:645/:651）与 REPL（:665）路径都写了 `writeSessionEnd`，需确认 `--check-config` 提前返回、上电失败等**没进会话**的路径不写（它们本就不该有 session），以及 REPL 内 `stopRequested` 中断后是否仍落 `end`。属复核+补用例，不预期改逻辑。 | [agentmode.cpp:645-666](../../src/agentmode.cpp#L645-L666) | 低 |

## 三、开发方案

### 阶段 1 —— G1：run_command 回执密钥遮蔽（唯一「真缺陷」级，单独一批）

- 位置：`AgentToolResult` 回传前对 `formatRunResult` 的结果过一次 `maskSecretsInText`，
  在 [agenttools.cpp:830](../../src/agenttools.cpp#L830) 收敛。
- 语义与既有遮蔽单点同源：`maskSecretsInText`（[agentaudit.cpp:207](../../src/agentaudit.cpp#L207)）
  判据（敏感名+`=`+值）与子进程环境清洗同源。**不改 `formatRunResult` 本身**——它仍然产出原始
  回执，遮蔽是「回传模型前」这一步，避免把「命令到底输出了什么」的信息在审计侧也抹掉。
- 需显式决策的一条：**遮蔽后模型可能不知道命令真实输出了什么**。对策：遮蔽是**有损提示**
  ——在 `maskSecretsInText` 检测到有替换时，回执末尾追加一句 `（检测到疑似敏感内容，已替换为 ***）`，
  让模型意识到结果被净化过、不要据此假定原值。不改审计 schema。
- 验收：
  - 新增钉子用例：`run_command` 输出含 `KEY=secret` 时回传文本中 `secret` 不可见，且出现净化提示；
  - 无敏感值时输出逐字节不变（不产生噪音）；
  - 既有 `agent:` 用例（`test_agenttools.cpp` 的 run_command 截断/超时/回执）零修改全绿。
  - **反向验证**：临时去掉遮蔽应使新用例转红。

### 阶段 2 —— G2：apply_sql 审计区分

- `AuditEntry` 对 `apply_sql` 追加一个只读字段 `summary`（生效摘要，来自回执的
  `robot.applySql` 返回值；放 `args` 之外，避免污染参数回显）。**加法演进**，老字段不动。
- `DispatchGate`/AuditLog 写路径：`AgentToolResult` 增加可选 `summary` 字段透传到审计。
- 收益：热插「新增 N 变量 / M 表 / 瞬态动作 K 行」落进审计，复盘可对账。
- 验收：`test_agentaudit.cpp` / `test_agentloop.cpp` 新增用例断言 `apply_sql` 记录带 `summary`
  且 `tool=="apply_sql"`；既有审计 schema 用例零修改。

### 阶段 3 —— G3：会话结束记录覆盖复核

- 复核 `runAgentMode` / `runAgentRepl` 各退出路径的 `writeSessionEnd` 覆盖：
  正常结束、模型失败、`/exit`、EOF、`stopRequested` 中断、`--check-config` 提前返回、
  上电失败（应无 session）。
- 若发现某条真实会话路径漏写 `end`（预期是有的，但需用用例钉住），补一行；否则仅补用例。
- 验收：新增用例钉住「一次性任务与 REPL 正常/异常退出都写 end；`--check-config` 与上电失败
  不写 start/end」。

## 四、实施纪律（沿用既有闭环）

1. 每步一个 commit；行为保持型以「存量用例零修改」为验收线。
2. 行为变更型（G1、G2）先补钉住新语义的用例，再做反向验证（注入破坏应转红）。
3. 每阶段收口后跑：`bash test/run_all.sh agent-all` + `./build/mybot_tests`（全量）。
4. 文档（`docs/agent.md` 安全边界节、`docs/reference.md`）与实现同 commit。

## 五、顺序与依赖

```
G1（单独一批）──► G2 ──► G3（复核+用例）
```

- G1 是唯一安全缺陷级且独立；G2 依赖 `AgentToolResult` 结构改动，独立一批；
  G3 只用例、可最后。

## 附：审计方法与盲区

- 结论来自 HEAD 逐行核对（给出 `文件:行号`）。
- 盲区如实列出：① G1 的遮蔽是文本级防线，不是「密钥永不进模型上下文」的结构保证
  （模型仍可让命令把值编码如 base64 绕过文本判据）——本计划只收「顺手一读」这类普通形态，
  与 `maskSecretsInText` 既有口径一致；② `apply_sql` 的审计只记摘要与参数，不记被删/被改的
  表逐行（要记需带矩阵 diff，收益不抵复杂度，本轮不做）。
