# 更新日志

> 从 [README](../README.md) 迁出，按主题分组记录版本演进要点，组内大致按时间倒序。每条只留「改了什么、对使用者意味着什么」——设计论证、实现落点与守门明细以 docs/ 各文档与 git 提交历史为准。
> 条目里的守门/用例**数字是当时的快照**：权威口径一律以 `bash test/run_all.sh` 与 `./build/mybot_tests` 的实时输出为准（`docs/development.md` 单一事实源约定）。
> 2026-09-23：`docs/proposals/` 下的开发方案文档已全部移至本地 `.tmp/`（不入库），另有更早的三份 `.tmp/*_20260922.md` 计划文件已核实清理；下文对这两类文件的引用均为历史记录，历史版本见 git（截至 `46f90d8`）。

## v1.x（C++ 移植版）

### 2026-09-24 收尾 `d384a9b9`：HEAD 恢复可编译 + agent 守门按归属分流

> 守门快照：全量 `bash test/run_all.sh` **270 / 0 / 0 跳**（`rc=0`）；C++ 单测
> **634 / 0**（`d384a9b9` 删 `test_agentloop` 40 + `test_agentrepl` 11，本次再删 5 个
> 依赖已删 `AgentSession` 的用例，`690 → 634` 逐笔可对）；`mcp_e2e.sh` **210 → 242 项**
> （迁入 6 趟工具层断言）；`agent_e2e.sh` Rust 段 **68 项**（现为该文件唯一模式）。
> 基线对照：`aed97ca4`（那次删除的上一提交）是 270 / 0 / **1 跳**——少掉的那条 SKIP 是
> `agent-rust-e2e`，本次因构建出 `mybot-agent` 而真的执行了。

- **修一个不能编译的 HEAD**：`d384a9b9` 删了 `src/agentloop.{hpp,cpp}`，却留下
  `src/agentmode.cpp` 的 `summarizeSessionStatus`（对不完整类型 `AgentSession` 取成员、
  并引用全仓已不存在的 `SessionStats`）与 `runAgentMode`/`runAgentRepl` 两个桩、
  `kMaxReplInputBytes`、`AgentSession` 前向声明——该 TU 同时进 `mybot` 与 `mybot_tests`，
  8 个编译错误、两个目标全断，且已推到公开 `origin/dev_agent`。本次按「生产零调用者」判据
  删除（`main.cpp` 走自己的 `runAgentPipeline`；`--mcp-backend` 只经 `makeToolContext`）；
  `summarizeAgentConfig` 与 `makeToolContext` 是活码，保留。另有
  `test/cpp/test_agentdriver.cpp`、`test_sessionfile.cpp` 仍 `#include "agentloop.hpp"`
  且都挂在 `CMakeLists.txt` 上，一并摘除 include 并删掉其中依赖已删类的 5 个用例。
- **补回一处被连带删掉的判据**（比编译错误更值得注意）：`historyProtocolViolation`
  ——「assistant.tool_calls 与 tool.tool_call_id 一一对应」的可执行判据（SAN-1）——
  唯一实现原在 `agentloop.cpp`，随那次删除一起消失；而 `agenthistory.hpp` 里
  `loadSessionFile` 的契约注释仍以它为判据、`test_sessionfile.cpp` 有一个活用例在调它、
  `scripts/fake_llm_server.py` 的孪生实现也还在，**Rust 侧没有对等实现**。已把该函数
  逐字归位到 `agenthistory.{hpp,cpp}`（行为零改动）。教训：删一个「表面」时，
  判据与注释里的依赖者不会跟着报错。
- **agent 守门按归属分流**（不是删掉了事）：`scripts/agent_e2e.sh` 原 22 个 C++ 场景
  在桩上跑出 **119 条断言红**（每条都以「错误: C++ Agent 已移除」+ 退出码 2 开头）。拆开：
  * 属**后端工具面**的 6 条迁入 `scripts/mcp_e2e.sh`，由纯标准库 Python 帧客户端直连
    AF_UNIX 后端跑——不经模型、无假模型剧本参与，判据更硬：`apply_sql` 带电生效
    （INSERT 之后新变量真的出现在 `list_hardware` 里）、热删除（DELETE 真的摘掉变量且
    报出删了谁）、两者的门控对照（缺开关时工具不在 `tool.list`、调用得「未知工具」、
    清单零变更）、`read_variables` 带回驱动返回表的键、策略拒绝时 `run_command` 零副作用
    （marker 文件不落盘）；
  * 属**循环层**的（工具循环、超时、限流重试、取消、图片后注、stdout 纯净/写失败不 panic）
    归 `agent_e2e.sh` 的 R 段——该文件现在是 Rust 单一模式（1746 → 704 行），
    `--rust` 保留为兼容空参；
  * **仍未覆盖，如实记下**：`--session-file`/`--resume` 的两进程接力、`--agent` over TCP
    远程 REPL（Rust 侧尚无实现），以及 DeepSeek/Anthropic 协议差异在整条循环上的表现
    （驱动层报文仍由 `test_llm_drivers.lua` / `test_llm_wire_contract.lua` 守）。
  原场景 5b（配置文件的 `camera_src` 不得自动放行）经实测**不是**工具边界性质：
  `capture_image` 的 `src` 只能由宿主配置填（`buildCameraParams` 不读调用方参数），
  且 `--mcp-backend` 直接拒绝 `--llm-config`，在这条面上不可达——判据由 C++ 单测
  `agentconfig_camera_src_dir_is_cli_only` 守。
- **注释与数字归位**：十余处指向已删 `agentloop` 的注释改为指名真实归属
  （`DispatchGate::run` / `HistoryManager::trim` / `AgentModeConfig` / Rust
  `model.rs`、`budget.rs`）；`harness.hpp` 的 roster 去掉两个零用例前缀；
  `CMakeLists.txt` 去掉失效的注释条目；README 的「27 个 C++ 单测文件」按实测改为 25。
  过去时的沿革叙述（`agenthistory.hpp` 头部、`util.hpp` 的 C7 来源）保留不动。

### 2026-09-24 审计高危修复两连：掩蔽引号绕过（H1）/ Rust stdout 写失败 panic（H2）

> 守门快照：C++ 单测 **690 / 0**（+1 例引号值遮蔽），全量 **269 / 0 / 3 跳**（环境性
> 跳过不变）；`agent/` 四检全绿（cargo build --release / test **50** / clippy
> `-D warnings` / fmt --check，均 `--locked`）；`agent_e2e.sh --rust` 扩到 **68 项**绿
> （新增 R10）。两处新判据均已反向验证：引号值用例在旧实现上 9 处断言红（密钥明文
> 留存）；R10 在临时还原 `println!` 后 4 处断言红——退出码 101、panic 轨迹、**审计
> session end 行丢失**三症齐现，恢复后绿。

- **H1（掩蔽绕过）**：`maskSecretsInText` 把引号一律当值终止符，值**以引号开头**时
  掩蔽区为空——`export KEY="sk-x"` 输出 `KEY=***"sk-x"`，引号后的密钥留在明文。泄漏
  双通道：审计 `args_raw` 落盘（append-only 不可撤回）与 `run_command` 回执进模型上下文
  前的净化是同一函数（G1/M1 的修复由此被绕过）。修复：新增 `maskedValueEnd` 共用于
  三个形态（名=值 / `Bearer` 令牌 / 敏感头）——裸值到空白或引号为止；引号值连同定界
  引号**整体**遮蔽，扫到同种配对引号、未闭合遮到串尾（宁可多遮不可漏遮）；引号段与
  裸段可拼接（shell 连接语义 `"a"b'c'` 是一个值，全部吃掉）。既有用例
  `token="abc def"` 一直绿是因为断言只查 `token=***` 与尾部两个子串、泄漏恰好夹在
  中间——新用例 `agentaudit_mask_covers_quoted_values` 改为逐字断言并覆盖三种引号
  形态、未闭合、拼接段与 Bearer 双级遮蔽。
- **H2（stdout panic）**：Rust 运行时把 SIGPIPE 置 ignore，`println!`/`eprintln!` 对
  写失败（断管道 EPIPE、`>/dev/full` ENOSPC）直接 panic（退出码 101）——`--task`
  输出后的审计 session end 行随之丢失；C++ 的 printf 失败只设 errno 不崩溃，这是
  迁移引入的回归面。新增 `agent/src/out.rs` 把全仓 **46 处** stdout/stderr 写点收敛为
  检查式宏：`outln!`/`out!`（stdout，写+flush，失败经 `libc::write` 直写 stderr 告警
  **一次**——不走 `eprintln!` 以免引入第二个 panic 面）与 `errln!`/`stderr_text`
  （stderr，失败静默：stdout 是模型输出与交互界面的保留通道，不能混告警）。REPL
  提示符的显式 flush 并入 `out!`。E2E R10 用 `>/dev/full` 确定性复现「stdout 全程
  写失败」（`| head` 断管道依赖调度时序，不做判据）：退出码 0、stderr 告警可见、
  审计 start/end 齐全、模型轮次照常记账。

### 2026-09-24 Rust agent 循环层语义对齐 C++（rig 迁移·批次 C）

> 守门快照：`agent/` 四检全绿（cargo build --release / test **48** / clippy `-D warnings` /
> fmt --check，均 `--locked`；为单测构造 429/5xx 加了 `http` 的 dev-dependency）；C++ 全量
> **269 / 0 / 3 跳**（环境性跳过不变），C++ 单测 **689 / 0**；
> `bash test/run_all.sh agent-all` **16 / 0 / 0**；`agent_e2e.sh --rust` 扩到 **63 项**绿
> （新增 R6 单轮上限 / R7 限流重试与恒 503 / R8 单次超时 + SIGINT 取消 / R9 图片后注）。
> 新判据已反向验证（临时让单轮上限失效 + 去掉图片后注的说明文字 → R6 三条、R9 一条红，
> 恢复后绿）。

- **新增四个模块**（`agent/src/`）：`budget.rs`（任务内文本/图片预算，逐字镜像
  `src/agentbudget.cpp`：SAN-4 未执行不降级只记账、AG-21 降级说明自身计入、图片额度用尽
  的终态文案）、`history.rs`（会话历史按**整块**裁剪，最新块无条件保留；记账口径同 C++
  的 `messageWeight`，rig 侧把思维链与 `additional_params` 一并计入）、`hook.rs`
  （循环层钩子：墙钟两判定点、单轮调用上限、失败回滚边界、取消标志；含图片批次的
  **物化**）、`model.rs`（模型装饰器：图片后注 + 单次调用超时 + 网络重试）。
- **循环语义对齐**：任务开始前裁剪旧历史（`max_history_bytes`）并清零任务内预算（含墙钟
  起点）→ 轮循环开头判定「取消 / 墙钟耗尽」（文案逐字同 C++）→ 逐条派发前再判墙钟
  （未派发的回执逐字同 C++）→ 单轮超过 `max_calls_per_round` 的调用**不派发**（模型看到
  「未派发」回执，操作者看到 `[上限]` 行）→ 成功才把新消息并入历史，失败回滚到**钩子
  记下的边界**（已完整跑完的轮留在历史里，半截 tool_calls 序列不落盘）。
- **图片后注**：后端 `imageDataUrl` 在预算内时进入待注入队列，模型装饰器把「说明 +
  图片」追加到**含工具结果的那条 user 消息末尾**——provider 转换后线上次序正是
  `[tool…, user(说明+图片)]`，与 C++ 的独立 user 消息同序（OpenAI 协议不允许工具结果
  带图片，rig 侧同理）。注入只作用于**发出去的请求**，任务结束后按记录的位置物化回会话
  历史。
- **网络重试（U-1）**：尝试次数含首次（`max_retries`，0 合法）、本地指数退避
  base×2^(n-1) 单次钳到 60s、退避取「本地 vs 服务端 Retry-After」的较大值，日志文案
  逐字同 `agentdriver.cpp`；`--llm-timeout` 变成**可重试类**的单次调用超时。
- **取消（D4）**：SIGINT/SIGTERM 首次只置停止标志（不再发起新模型调用与工具下发，REPL
  循环退出、`--task` 以失败收场），第二次沿用 C++ 的强杀路径（还原缺省处置后重触发）。
- **配置面**：`max_history_bytes`（4 MiB）/`max_task_tool_result_bytes`（2 MiB）/
  `max_task_image_bytes`（4 MiB）/`max_calls_per_round`（16）/`max_retries`（3）/
  `retry_backoff_ms`（1000）落地解析与区间校验（区间逐值同 `llm_limits`；0 仍是
  「不限」或「不重试」的显式语义），`--check-config` 摘要同步。
- **审计**：model 行改为**逐次模型调用**一条（与 C++ 同口径），补上 `req_bytes`（请求体
  估算下界）、`resp_bytes`（模型可见文本字节）与 `attempts`（实际尝试次数，>1 = 发生过
  重试）。
- **守门夹具修正**：`fake_llm_server.py` 的 `manycalls` 剧本补 `reasoning_content`。
  该剧本此前只在收尾那轮带思维链，于是同一会话里出现「带 tool_calls 但无思维链」+
  「有思维链」的**混排**——真接入点上不可能出现（思考模式是接入点级开关，每轮都有思维链），
  却让夹具比真机更严：rig 会把思维链原样回传（C++ 的 OpenAI 驱动不回传该字段，故此前未暴露），
  第二个任务因此撞 400。补上后剧本自洽，C++ 场景 10 逐字复跑仍绿。
- **语义如实偏差（记录在案）**：① 单轮上限的呈现位置——C++ 从 `assistant.tool_calls` 剔除
  超限调用并把截断说明追加进 assistant 文本，rig 侧挂在**工具回执**上（「不派发」语义一致）；
  ② `req_bytes` 口径——C++ 量的是驱动组好的请求串，rig 量的是 `CompletionRequest` 的
  JSON 序列化（同为下界，量级可比但非逐字节相同）；③ 重试分类——C++ 用 curl 退出码表，
  rig 用 HTTP 状态码 + reqwest 连接类错误；④ 取消的时效——C++ 有取消哨兵能在约 1s 内掐断
  在飞的 libcurl 请求，rig 侧的在飞请求由 `--llm-timeout` 封顶（取消在两次判定点与退避
  循环里生效）。
- **尚未搬（批次 D/E）**：`--session-file`/`--resume` 会话持久化、TCP 远程 REPL、
  `docs/agent.md` 里的 sidecar 用法改写。`docs/testing.md` 的 CI 行（`gate:agent` 四检）
  不变，段计数不变。

### 2026-09-24 agent 本体迁移到 Rust（rig）：`mybot-agent` 骨架落地（rig 迁移·批次 B）

> 守门快照：`agent/` 四检全绿（cargo build --release / test **31** / clippy `-D warnings` /
> fmt --check，均 `--locked`）；C++ 全量 **269 / 0 / 3 跳**（环境性跳过不变），C++ 单测 **689 / 0**；
> `bash test/run_all.sh agent-all` **16 / 0 / 0**（含新段）；新 E2E `agent_e2e.sh --rust`
> **32 项**绿（R1-R5）、`mcp_e2e.sh` **57 项**绿。新断言已反向验证（临时把就绪行改走 stdout +
> model 行 usage 置空 → 「stdout 纯净 / stderr 就绪行 / usage 记账」3 条红，恢复后绿）。

- **目录与产物**：`mcp-sidecar/` → `agent/`，包名/二进制 **`mybot-agent`**；`handler.rs`（MCP
  stdio 协议层）删除——MCP 协议层不再由独立进程提供，agent 直连 `mybot --mcp-backend` 的
  AF_UNIX 帧（4B BE，同 `agent/src/frame.rs`）。`scripts/build_sidecar.sh` → `build_agent.sh`。
- **模块**：`main`（CLI/编排/退出码）、`config`（配置链：命令行 > 环境变量 > 配置文件 > 缺省，
  未知字段仍拒绝）、`provider`（rig 驱动按协议构造 + 接入点归一）、`bridge`（工具表→rig 工具、
  帧调用→工具回执）、`prompt`（内置段落 + `--prompt-file` 包自然排序分节）、`repl`（会话/一轮
  任务/内置命令）、`audit`（session/model 行；**工具行仍归后端**，agent 不重复记）。
- **语义如实差异（记录在案，批次 C/D/E 落）**：审计每次任务一条 model 行（C++ 是每次模型调用
  一条）；图片注入、历史裁剪/字节预算、`--session-file`/`--resume`、TCP 远程 REPL、SIGINT 取消
  尚未搬。`/hardware`、`/scripts`、`/sql` 一律经工具层（本进程不持有硬件），后端没给工具时
  **明确点名缺的开关**（如 `--agent-hot-sql`），不假装执行过。**`docs/agent.md` 里的 sidecar
  用法示例（`./mcp-sidecar/target/release/mybot-mcp`）已失效**，随批次 D 的文档更新一并改写；
  本批次只做最小事实修正（`docs/testing.md` 的 CI 行改名）。
- **E2E 与守门接线**：`scripts/agent_e2e.sh` 加 `--rust`（R1 工具循环含后端侧 Lua 驱动见证、
  R2 提示词包、R3 审计分工、R4 REPL + 能力门控、R5 `--check-config` 正负路径）；`run_all.sh`
  新增 **agent-rust-e2e** 段（`agent-all` 覆盖；缺 `agent/target/release/mybot-agent` 按 SKIP——
  沿用「只给 Rust 单独加 CI job」的口径）；`mcp_e2e.sh` **删 MCP stdio 段**，原判据（socket
  0700/0600、能力门控两趟对照、未知工具回执、fd 卫生 SOCKFDS=0、审计 session+tool、SIGTERM
  优雅停机、M5 无 policy 拒启）改由直连帧的独立 Python 客户端接住——两份实现各自跑绿不证明
  字节兼容，第三方尺子才证明；CI `gate:sidecar` → **`gate:agent`**（`cd agent`，四检不变），
  `docs/testing.md` 同步。
- **夹具适配**：`scripts/fake_llm_server.py` 的 OpenAI 形态应答补 `id`/`model` 外壳——rig 0.42
  把这两项定为必填（真网关也都带），C++ 驱动只取 `choices` 故不受影响，同一份夹具对两份实现成立。

### 2026-09-24 守门修复三连：curl POST 顺序 / 审计 exit_code / destroy 契约调和

> 守门快照：修复前在 HEAD（44ce196）上全量即红（3 段失败，先于本次 context.get 改动
> 存在，stash 复跑钉住）；修复后全量 **268 / 0 / 3 跳**（环境性跳过不变），
> C++ 单测 **689 / 0**（本条新增 1 例 agentaudit 定界回执解析）。agent-e2e 18 场景
> 全绿为 2026-09-23 dd1b415 以来首次。新用例已反向验证（临时恢复旧「必须是前缀」
> 判据 → `agentaudit_extract_exit_code_parses_quoted_receipt` 红，恢复后绿）。

- **`llm_http.lua` POST 选项顺序（回归自 dd1b415/M4）**：`CURLOPT_POSTFIELDSIZE` 被写在
  `CURLOPT_COPYPOSTFIELDS` **之后**——curl 以「设 COPY 时的 postfieldsize」决定拷贝字节并
  打内部定长标记，后设的 size 会把它打回未定长态，本机 curl 8.14.1 实测 POST 侧退化为
  读流语义、直接 `CURLE_READ_ERROR(26)`，**所有模型调用失败**（agent-e2e 全线红）。
  ctypes 双序对照钉住（copy→size rc=26 / size→copy rc=0）；M4 落地当天的 libcurl 对此
  顺序不敏感，故当时守门未抓到。修正为 size 先、copy 后。
- **审计 `exit_code` 字段消失（回归自 1aab7b2/M2）**：run_command 回执整体经
  quoteUntrusted 定界后，`exit_code=` 行带 `> ` 前缀且不再位于文本开头，`extractExitCode`
  的「必须是前缀」假设失效——工具执行照常、审计行却静默丢失退出码。改为逐行扫描
  （剥 `> ` 前缀后首个 `exit_code=` 行；stdout 里的同形文本必在其后，不会抢先），
  数值约束不变（负号、≤255）。此问题此前被 curl 回归挡在更早处，curl 修好后才暴露。
- **PCA9685/UART destroy 契约调和（回归自 11aacf4/M3）**：M3 为修 fd 泄漏整体摘掉了
  destroy 的 arg_table，结果 ①`PCA9685.destroy(nil)` 裸抛「attempt to index a nil
  value」；②UART.destroy(nil) 静默 OK——两者都违背 D2-1 契约 sweep「非表入参返 ERR
  （不抛错、不静默成功）」。调和：**arg_table 守卫放回 destroy**（lint-driver-entry
  样板要求在位），UART 的 fd 释放**先行**（M3 的无泄漏要求保住——fd 是 upvalue，任何
  早退不得跳过 uart_close）；表内资源缺失/写失败仍按 M3 语义静默 OK
  （test_pca9685_writefail 的 destroy 期望随 M3 由 ERR 改为 OK，另补非表 → ERR 用例）。

### 2026-09-24 帧后端新增只读路由 `context.get`（rig agent 迁移·批次 A）

> 守门快照：C++ 单测 **689 / 0**（本批新增 3 例 mcpbackend_context_*；另 1 例见上条）；
> mcp_e2e **50 项**全绿（帧对打段新增 context.get 3 项断言）；零告警构建。新用例已反向
> 验证（临时去掉 structureExcerpt 的 quoteUntrusted 定界 → `context_get_returns_quoted_material`
> 红 2 处，恢复后绿）。

- **`context.get`（新增路由）**：`--mcp-backend` 帧协议的第四条路由，**只读**——为
  Rust/rig agent（取代 C++ agent loop 的新主路径，后续批次落地）提供系统提示词素材：
  `robotName`/`structurePath`/`scriptsDir`/`allowShell`/`allowHotSql`/`hardwareCount` 与三段
  **已定界**磁盘内容（结构脚本 16 KiB 摘录、硬件清单、脚本清单）+ 内联 `readableVars`。
- **单一事实源**：素材读法与 `buildAgentSystemPrompt` 完全同款（有配置组装期成品就用成品；
  消毒、截断、`quoteUntrusted` 定界都在 C++ 单点）——Rust agent 只拼装不定界，「同一份磁盘
  内容，两条进上下文的通道必须有同一定界强度」。`structureDisplayName` 随之从 agentmode.cpp
  的匿名命名空间提升为共享声明（第三处使用）。
- **实时取材**：`hardwareCount`/`hardwareList` 每次调用现取注册表，热插拔后不漂移。
- **口径**：与 `tool.list` 同判据——不走 `DispatchGate`、不占每连接预算（单测钉住「预算耗尽
  后 context.get 照常成功」）；未绑定 cfg/robot 的手搓后端（单测形态）响亮回错误。
- **落点**：`McpBackend` 构造器加可空 `cfg/robot` 指针对（缺省 nullptr，既有调用零改）；
  `mcp_e2e.sh` 帧对打段与 docs/agent.md 同步。

### 2026-09-23 只读查询：`SHOW TABLES`

> 守门快照：C++ 单测 **669 / 0**（本批新增 5 例：sqlparse 4 / agenttools 1）；零告警构建。

- **`SHOW TABLES;`（新增能力）**：方言第七条语句，**只读**——在 agent 的 `query_sql`
  与 REPL `/sql` 里列出当前结构的**全部表名与数据行数**（数据行 = 除列头外的行数），
  形如 `SHOW TABLES (2):` 后逐行 `  表名 (N 行)`。走只读通道，故**不受 `--agent-hot-sql`
  门控**（与 `query_sql` 的 SELECT 同），回执经 `quoteUntrusted` 定界进模型上下文。
- **看见自动注入的表**：agent 模式下宿主会注入 `LlmOpenAI`/`Camera` 等表，它们不在结构
  脚本摘录里——`SHOW TABLES` 是「当前到底有哪些表」的唯一权威答案（此前无从查起）。
- **不带 `WHERE`/`LIKE`**：本方言取向「能一句话说清就够」，要筛某张表的行请用
  `SELECT * FROM 表 WHERE ...`；误写的尾随词元由 `nextEnd` 以带行列定位的词元级报错兜住。
- **落点**：解析层新增 `SHOW`/`TABLES` 词元与无载荷的 `SHOW_TABLES` 事件（只认
  `TABLES` + 分号）；渲染在 `agenttools` 的 `renderShowTables` 单点，`executeReadOnly`
  与 `runSqlStatement` 共用；结构装载/热插路径 `readSQL` 落 `default: break` 忽略
  （与 `SELECT_TABLE` 同处置，只读语句不改结构）。

### 2026-09-23 热更新：`UPDATE 表 SET 列 = 值 [, ...] [WHERE ...]`

> 守门快照：C++ 单测 **662 / 0**（本批新增 robot 热更新组 9 例、sqlparse 解析组
> 6 例重写/新增，agenttools 既有用例改断言）；零告警构建。

- **`UPDATE`（新增能力）**：方言第六条语句，在 REPL `/sql` 与 agent 的 `apply_sql`
  （沿用 `--agent-hot-sql` 门控）里按**断电重建**改写命中行——`UPDATE` 不再被
  `Unsupported statement` 拒绝，`isUnsupportedStatement` 随之整体移除（清单清空）。
  执行语义 = 「DELETE 命中行（物理断电摘除）+ INSERT 改后行（重新装配上电）」，
  readSQL 在影子上分解，改后行取 `inherited()` 解析值物化（`-` 继承格原样重插会对着
  不同的前一行重新继承、静默变义）。`name` 列可改（= 换名重建）；无 WHERE = 改全表。
- **先删后建执行序**：含 UPDATE 的批次把物理删除提前到插入之前（原 4.6 抽出为
  `executeHotDeletes` 成员函数，两个执行序共用）——注册表重名即抛，同名重建必须先摘
  旧件；备删计划前移到阶段 1.5（doomedNames 供重名预检豁免、hw 指针计划期取好防
  按名 find 摘错对象）。失败窗口如实：新件装配失败 ⇒ 已摘旧件不复原，错误点名丢失
  的变量并包装成 runtime_error（成因若是 IllegalArgumentException，paramError 口径
  会谎报「活体一字未动」）。
- **引用完整性预检扩面**：从「matrices_ 存活行」扩为「提交后合并视图」（含本批
  新增/改写行）——流引用是懒解析，预检不拦就是运行期的雷；对普通 INSERT 批次同样
  是净收益。被引用（controller/trigger/$/表达式）的变量拒改，与热删除同一条防线。
- **批内独占**：UPDATE 与该表的其它行级语句（INSERT/DELETE/再次 UPDATE）双向不得
  同批，本批次新建的表上也不可用——守门全部在 readSQL，报错指路「拆成两批」。
- 摘要/审计（G2）：生效摘要报「已热更新 N 行（断电重建）」，删除/新增计数照报
  （同一物理事实的两个维度）。

### 2026-09-23 整表撤下：`DROP TABLE ...`

> 守门快照：C++ 单测 **649 / 0**（本批新增 14 例：sqlparse 5 / robot 9，
> 另更新既有 3 例的 DROP 断言）；零告警构建。

- **`DROP TABLE 表;`**（新增能力）：在 REPL `/sql` 与 agent 的 `apply_sql`（沿用
  `--agent-hot-sql` 门控）里，把上电常驻机器人的**整张表**当场撤下——表内全部硬件变量逆序
  `destroy()` 断电 → 撤注册 → 擦结构行，再把**表壳**（列头）连同该表的元数据提供者、守护登记
  一起摘除。这是热删除落地时被明确「仍在册」的那条语句（当时理由是要连带拆 MetaProvider/守护
  登记并面对「表撤销后同名重建」的新语义面），本批把那两块补齐。
- **不带 WHERE**（标准 DDL 语义）：DROP 作用于整张表，WHERE 是 DML 的行级筛选，加在 DROP 上会
  「长得像在筛行、实际却整表删」。误写 WHERE 在**解析期**被词元级报错拒绝（`Token WHERE`）、
  活体一字未动。要按条件删一部分行用 `DELETE`；要确认删对了表，先 `SELECT * FROM 表 WHERE ...`。
  **空表（只有列头）可撤**——不按 DELETE 的「0 命中报错」处理。
- **实现落点分两段，顺序是死的**：`readSQL` 把整表数据行全删（走与 DELETE **完全相同**的
  擦除机器，故断电/撤注册/引用完整性/`destroy` 中途失败报数全部复用既有防线），`applySql`
  再摘空表壳。表壳摘除必须排在逐行删除**之后**——逐行删除用「矩阵地址 + 行号」定位
  MetaProvider，先 `erase` 表壳会让其后所有表的目标错位。
- **新增一条不变式：MetaProvider 重锚定**。矩阵容器是 `std::deque`（热插表选它的理由正是
  「追加不失效元素引用」），但**中间 `erase` 会让其后元素引用失效**——整表撤下第一次用到
  中间删除，故 `MetaProvider::matrix_` 从引用改为指针，撤表后按**表名**（唯一且稳定）把存活
  提供者重锚定回新地址，否则后续 read/destroy 踩悬垂指针。
- **批次内的互斥与专门路径**：DROP 必须是该表在本批次的唯一动作（与本批 `INSERT`/`DELETE`
  混排会打乱 applySql 靠 `insertedRows` 划「新增行尾部连续块」的算法）；`CREATE` 后同批
  `DROP` 同名表有专门路径（整表从未装配，净效果为空）。`Action` 是瞬态表（行只重放、不注册），
  明确拒绝 DROP。
- **打开的新语义面**：DROP 之后可**同名重建**（表名不再冲突），随后 `CREATE TABLE` 同名表会
  装配出全新变量。
- 词法：`DROP` 成为关键字（大小写不敏感），故 `drop` 不能再作裸标识符（带引号仍是合法
  NAME）；`isUnsupportedStatement` 只剩 `UPDATE`。文档同步 dsl.md（「方言只认四条」→ 五条、
  新增整表撤下安全边界一节）、`apply_sql`/`query_sql` 两个工具描述。

### 2026-09-23 守门检查点同步：`parse_cases` 95 → 96

- `test/run_all_selftest.sh` 的 `parse_cases` 条数检查点在 2026-09-23 那批新增
  解析用例时**漏改**（检查点停在 95，实际 96），使 pre-push 的守门自检恒为红。
  本批按 F3-2 契约把检查点同步到实际条数（字段数/去重/收尾引号三项本已通过，
  只是条数这一项滞后）。纯守门簿记，无行为变化。

### 2026-09-23 第三个协议驱动 LlmAnthropic（批次 3）

> 守门快照：`luajit test/test_llm_drivers.lua` **215 / 0**（本批新增 30 条
> Anthropic 断言）、agent E2E **21 场景**（新增场景 21：换驱动即换协议，
> 11 条线上报文/鉴权头断言）、全量 `run_all.sh` **267 / 0 / 3** rc=0、
> 零告警构建。
> **验收线即「宿主零改」**：`git diff --stat src/` 为空——本批一行 C++ 都没动。
> 反向验证与夹具加固：假模型服务新增 Anthropic 应答形态后，它的两处
> OpenAI 形状假设（工具 schema 校验、工具结果计数）先后**误判正确的报文**，
> 已按形状分派修正；其中一处（无参工具 `required` 被转成 `{}` 空对象）
> 正是新驱动**真实的缺陷**——夹具与真机一样严格，把它在上线前抓了出来。

- **`LlmAnthropic`**（`--llm-driver LlmAnthropic`）：Anthropic Messages 协议驱动。
  选它的理由不是「多支持一家厂商」，而是**验证规范化协议抽象是否真的成立**：
  它与 OpenAI 系的形状差异最大，若驱动层能独立承担，则「换模型 = 换 lua 文件」
  这条承诺对所有协议都成立；它反过来也检验了宿主抽象有没有漏（本批零改通过，
  说明没有）。
- **四处形状差异全部收敛在 `lib/llm_wire_anthropic.lua`**：① `system` 抽到顶层字段
  且不进 messages；② 工具调用表达为 assistant 的 `tool_use` 块、`input` 是**对象**
  （规范化侧是 `arguments` JSON 字符串，驱动解码，非法 JSON 响亮拒绝）；③ 工具结果
  并进**一条** user 消息的 `tool_result` 块（Anthropic 要求严格 user/assistant 交替，
  构建后另有一道交替自检）；④ 鉴权用 `x-api-key` + `anthropic-version` 头。
- **两处 provider 约束按「响亮拒绝不静默」处置**：`temperature` 只接受 `[0, 1]`
  （宿主允许到 2，越界即 ERR 点名，不静默钳制）；`max_tokens` 必填。
- **响应侧同样抹平**：`content` 块数组的 text 段拼接、`tool_use` 转规范化
  `tool_calls`、非文本非工具块（扩展思考的 thinking）丢弃但**计数入审计**
  （`extras.dropped_content_blocks`）、`usage` 的 `input_tokens`/`output_tokens`
  映射到审计认得的 `prompt_tokens`/`completion_tokens`、`stop_reason` 进 `extras`
  （排障要知道是不是被 `max_tokens` 截断）。
- **接入点无缺省（fail-closed，B2 同口径）**；API key 读取链在 OpenAI 系基础上
  追加 `ANTHROPIC_API_KEY`。
- 文档：`docs/agent.md` 的「内置三个协议驱动」表与形状差异说明、
  `docs/testing.md` 的用例描述、`README.md` 的多模型支持与「远程对话」节；
  「已知限制」里「无记忆持久化」与「只有两个协议驱动」两条**标记为已解决**
  （分别由批次 1、批次 3 消掉）。

### 2026-09-23 agent over TCP（`--agent --tcp`，批次 2）

> 守门快照：C++ 单测 **614 / 0**、agent E2E **20 场景**（新增场景 20 四段：
> 远程往返 / 错 token 拒绝 / 空闲超时收尾 / 断开即取消）、全量 `run_all.sh`
> **265 / 0 / 3** rc=0、E2E 32.7s（门禁预算 90s）、零告警构建。
> 反向验证：watcher 命中后不触发取消 → 场景 20d 的**两条**断言都转红
> （语义「请求已取消」+ 耗时阈值；耗时阈值最初写成 15s 时无判别力，
> 因慢速假服务自己睡 5s，已收紧到 4s）。
> 依据：`.tmp/agent-4th-plan.md` 批次 2；实施时按其缺口评审收口了两条
> （超时口径重设计、取消语义），第三条（日志流）在文档里写明复用方式。

- **`--agent --tcp <port>` = 远程 REPL**（`--agent` 与 `--tcp` 不再互斥）：
  对端每行一条任务，模型输出与工具回显按既有 REPL 格式回写同一条连接。
  现场机器人接 WiFi 后，人不必 ssh 上去（ssh 会引入另一套权限面）。
  仍被拒：`--task --tcp`（一次性任务没有长驻对话的意义）、`--agent --tcp --input`。
- **安全口径逐条继承动作源 TCP**：缺省只监听回环、跨机必须 `--tcp-token`
  （或 `BOT4CJ_TCP_TOKEN`）、口令失败上限与「未给凭据不计数」的口径相同、
  明文口令仅限可信内网（对外暴露仍靠进程外隔离）。取消哨兵、单任务墙钟预算、
  策略钩子、审计轨迹在远程形态照常生效。
- **超时口径重设计（缺口评审第 1 条）**：远程 REPL **不套用连接总时长预算**
  ——动作源的 600s 总预算是给「一次性脚本投递」设的，套到长会话上会在任务
  中途掐断连接。现在只保留空闲超时，缺省从动作源的 30s 提到 **600s**
  （`tcp_limits::kAgentIdleSecDefault`：动作源 30s 断的是"对端发不出脚本"，
  远程对话里 30s 断的是"人还在想"），且只在等新任务行时计时；模型调用与工具
  执行各有 `--llm-timeout` / `--llm-task-timeout` 两个维度，不受它限制。
- **远程取消（缺口评审第 2 条）**：客户端 Ctrl-C / 关终端 = 断开连接；宿主的
  观察线程（poll 等 POLLHUP/POLLRDHUP/POLLERR，**不消费任何字节**）命中后执行
  与本地 SIGINT 完全相同的两件事——置停止标志 + 创建取消哨兵，于是在飞的模型
  调用被 libcurl 进度回调**立即**中止（实测 2s，而非等满 `--llm-timeout 60`），
  随后按既有中断语义优雅收尾（写 end 记录、走 deactivate，哨兵无残留）。
- **接受连接在硬件上电之前**：没有对端时不让执行器带电空转。
- **日志流的复用方式（缺口评审第 3 条）**：REPL 的 `log` 流（工具/错误回显）
  仍走 stderr，**不**并入连接——只有提示符、模型回答与内置命令输出经连接回写
  （与 `--task` 形态的 stdout 口径一致：宿主自己的诊断不污染对端看到的回答）。
- 文案与文档：`--help` / `--help agent` / `--help tcp`、`docs/reference.md` 的
  `--tcp` 行、`docs/agent.md` 新增「远程对话：--agent --tcp」节（含三处刻意
  不同与安全口径）。

### 2026-09-23 会话持久化与恢复（`--session-file` / `--resume`，批次 1）

> 守门快照：C++ 单测 **610 / 0**（本批新增 13 例 test_sessionfile + 既有用例零修改）、
> `agent:` 作用域 **253**、agent E2E **19 场景**（新增场景 19：两进程对话接力 +
> 三条对照）、零告警构建。反向验证：① 去掉指纹核对 → mismatch 两条转红；
> ② 图片改回原样落盘 → 4 条转红；③ `persistNewMessages()` 挪进 catch 分支 →
> 边界用例转红（并因这次验证把该用例加强为「失败发生在完成一轮工具之后」，
> 原先的形态分辨不出这个注入）。
> 依据：`.tmp/agent-4th-plan.md` 批次 1；实施时按其缺口评审收口了三条
> （配置文件来源不授权读取、图片写入期排除、审计 schema 演进显式声明）。

- **`--session-file <路径>`（写入）**：每个任务块**完结**后把新增消息逐行追加进
  JSON Lines 会话文件（首行会话头：结构脚本名 / 工具集指纹 / 系统提示词指纹）。
  机器人在重启后不再「忘掉刚才聊了什么」——此前唯一的出路是重启进程，而重启
  会丢掉硬件常驻状态（动作位置、上电状态），现场代价不小。也可用配置文件
  `session_file` 字段（**只用于写入**）。文件含完整对话内容，权限宽于 `600`
  时启动告警。
- **`--resume`（恢复）**：读回会话文件后继续。**路径必须来自命令行**——恢复是把
  磁盘内容读进模型上下文（输入通道），配置文件不能授权一次读取（与
  `camera_src_dir` / `read_vars` 同判据）；文件必须已存在。三道判据全过才恢复，
  任一不符即退出 2 并点名差异：逐行解析（损坏行跳过并点名行号，首行非会话头
  则整份拒绝）、指纹核对（结构脚本 / 工具集 / 系统提示词）、工具调用协议自洽
  （`tool_call_id` 无孤儿、无重复、无漏答——恢复的历史按不可信输入对待）。
  恢复后仍受 `max_history_bytes` 预算裁剪；载入内容与内存历史的边界语义
  （半截 `tool_calls` 序列永不落盘；失败任务里已完成的那轮工具随后续任务一起
  落盘）都有用例钉住。
- **图片与 system 提示词不进文件**：图片字节以 `[图片 N 字节，未随会话恢复]`
  占位落盘（**写入期排除**——体积与隐私两个方向都要求字节不出现在磁盘上），
  system 提示词不落盘（恢复时由当前进程重建，再按指纹核对）。
- **`/clear` 联动**：向会话文件写 `session_clear` 分界标记，恢复时该标记丢弃
  此前的全部消息——否则「忘掉对话」会在下次恢复时复活。
- **审计 schema 演进（显式声明）**：`kind="session"` 的 `start` 记录新增
  `"resumed": true`（恢复的会话），并沿用被恢复会话的原 `session id`。
  这是**加法字段**：既有字段名、顺序、语义一个不动，老消费方按既有字段过滤的
  行为逐字节不变（第二轮方案曾宣告「不改审计 schema，演进走新提案」——本条即
  该提案，在此显式声明而不是顺手动）。
- **文案与帮助**：`--help agent` 与 `--help` 正文、`docs/reference.md` 选项表、
  `docs/agent.md` 配置文件字段表与新增「会话持久化」节同批更新；黄金测试
  （帮助 ↔ docs 双向一致）的选项计数 33 → 35。

### 2026-09-23 热删除：`DELETE FROM ... [WHERE ...]`（含 AND 多条件）

> 守门快照：C++ 单测 **622 / 0**（本批新增 25 例：sqlparse 7 / matrix 4 /
> robot 13 / agenttools 1；另有若干既有用例的断言随语义更新，如
> `parser_rejects_deleting_and_rewriting_statements` 拆分为只拒绝 DROP/UPDATE）、
> agent E2E **19 个场景**（新增 14c：同一门控下的热删除真的摘掉变量）、
> `bash test/run_all.sh` 汇总 **265 通过 / 0 失败 / 2 跳过**、零告警构建。

- **`DELETE FROM 表 [WHERE 列 = 值 [AND 列2 = 值2 ...]];`**（新增能力）：
  在 REPL `/sql` 与 agent 的 `apply_sql`（沿用 `--agent-hot-sql` 门控）里，
  把上电常驻的机器人**带电摘掉器件**——逆序 `destroy()` 断电 → 摘元数据/
  守护登记 → 擦结构行 → 撤注册。无条件写法删空该表。结构脚本 `.sql` 里
  写 DELETE 仍在解析期报错（机器人未上电，没有可变的结构）：`readSQL` 的
  `allowDelete` 缺省关，故启动路径的接受语言逐字不变。
- **`AND` 成为关键字**（大小写不敏感），WHERE 条件列表解析收敛到
  `Parser::nextConditionList` 单点，SELECT 与 DELETE 共用——两条语句的
  WHERE 语法、报错锚点、词元消费顺序不会漂移。`WhereEqual` 升级为
  `WhereClause{conds}`（AND 语义，空 = 无条件），新增 `DeleteTableEvent`。
  行为变化评估：robots/ 全部 144 个 `.sql` 里仅有的两处 `and` 都在**引号值
  内**（`robots/test/sqlquote.sql`），词法引号态在关键字判定之前消费字符，
  零破坏，并由新用例 `lexer_and_is_a_keyword_case_insensitively` 钉住。
  `OR` / 比较运算符 / `IN` 仍不在方言内。
- **原子性口径：逻辑原子、物理不可逆**。`destroy()` 是物理断电，删了没有
  回滚可言（回滚 ≠ 复活器件），故防线放在**事前判全**：目标表存在、行能
  命中、引用完整性、全结构判据（单消费者等，对「删除之后的视图」用与启动
  期同一实现复跑）全部在动第一个器件之前跑完，任一失败即整批拒绝、活体
  一字未动。过了这道关，剩下的失败只可能来自物理 `destroy` 本身——此时
  立即中止后续删除并如实报「已删除 N 个（名单）、M 个未删除」，结构与注册表
  只反映已成功摘除的部分，不假装回滚。
- **引用完整性**（本批新增的关键判据）：被别的行以 `controller`/`trigger`
  （含 `controller2` 这类编号列）、`$name` 引用或 `$(...)` 表达式引用着的
  变量**拒绝删除**，报错逐条点名「哪张表哪一行的哪一列引用了它」。理由不是
  保守而是必需：这些引用在框架内部是**终身缓存的裸指针**
  （`MtVars_Lua::ctrlPtrs_`/`trigPtrs_` 首用缓存、`MetaRef::target` 在装配期
  锁定），而那两条「指针终身有效」的注释成立的前提正是 **pool_ 只增不减**
  ——热删除第一次打破该不变式，故必须在这里把「不留悬垂」重新建立为构造
  保证。判据看的是**删除之后**的视图，故「引用方与被引用者一起删」合法。
  `HwRegistry::removeBatch` 与 `rollbackTo` 的差别正在于撤**任意位置**而非
  尾部：三容器按指针集合一趟 erase-remove（先全量验存在性再改），避免
  「先记下标再逐个 erase」在第一次 erase 后就错位。
- **配套不变式**：`MatrixObject::removeRows` 只擦不换序（行序即声明序，而
  销毁序 = 声明序倒序是总线 fd 的资源不变式，交换式删除会让「删一个中间
  器件」打乱整条拓扑序且症状到销毁期才显形）；`mt::matchAll` 与
  `matchRows` 共用一份比较语义（inherited 显示值精确文本）；删除按行号
  倒序执行，故被删行号严格大于任何存活 `MetaProvider` 的行号，其 `row_`
  不会因前移而错位。
- **几处刻意的报错选择**：条件命不中任何行时**报错而非静默成功**（拼错
  name 的 DELETE 若「成功」会让操作者以为器件已摘除）；`DELETE` 出现在
  REPL 的只读渲染入口时给一句指路而不是「不支持的 SQL 语句」（那是误导——
  DELETE 是支持的，只是不在这条通道上）；被引用而拒删时分类为**参数类
  失败**（校验拒绝 = 活体一字未动，不谎称「机器人可能停在半途」）。
- **删除后 `flowRefs_` 摘名**：「删掉再热插同名变量」是合法恢复路径，同名
  变量此后仍可被动作脚本别名覆盖（留着旧名字会永久拒绝覆盖，挡的却是一个
  不存在的引用）。
- 文档同步：dsl.md（**「方言只认三条语句」→ 四条**，新增热删除安全边界一节）、
  reference.md 与 agent.md（`--agent-hot-sql` 行 + 工具表 + 安全说明第 10 条
  补热删除）、README 工具表、两个工具描述（`apply_sql` 明确标注「破坏性且
  不可逆」——那是模型侧唯一的约束面）、REPL `/help` 与 `/sql` 用法提示。

### 2026-09-23 SELECT 支持基本 WHERE 等值条件（`=`）

> 守门快照：C++ 单测 **597 / 0**（本批新增 10 例：sqlparse 7 / matrix 1 /
> agenttools 1 / agentrepl 1）、`agent:` 作用域 **241**、零告警构建。
> 新用例已做反向验证（词法器去掉 `=` 分隔分支 → 指纹与紧贴写法两条转红；
> 渲染层去掉过滤 → agenttools/agentrepl 各一条转红）。

- **`SELECT * FROM 表 WHERE 列 = 值;`**（新增能力）：单条件等值、精确文本
  匹配，`query_sql` 工具与 REPL `/sql` 同一生效（共用 `runSqlStatement`
  单点）。AND/OR、比较运算符、多条件不在方言内——载荷结构与解析入口
  （`WhereEqual` / `Parser::nextWhere`）已按三语句共享写成，是将来
  DROP / DELETE / UPDATE 落地时直接复用的扩展缝。
- **匹配语义与渲染同源**：比对 `inherited` 显示值（`-` 占位继承解析后），
  查询结果与全列渲染看到的是同一份值；数值按文本比（`'0x40'` ≠ `'64'`，
  已知边界，已写入 dsl.md）。列不存在报 `Table 'x' has no 'y' column.`；
  0 行命中输出列头 + `（0 行匹配）`——空结果显式说破，模型不会把「没有行」
  误读成渲染故障。匹配单点是 `mt::matchRows`（matrix 层，参数用基本类型
  避免 mt 层反向依赖 sqlparse 层），未来 DELETE/UPDATE 选行复用同一处。
- **词法器 `=` 成为第五个分隔词元**（同 `; , ( )` 的「空直返/非空挂起」
  模式，紧贴写法 `col=value` 靠挂起标志切分）。行为变化评估：robots/ 全部
  144 个 `.sql` 的 `=` 均在引号值或注释内（逐一扫描），引号态在分隔符判定
  之前消费，零破坏；`$(...)` 表达式语法本就无 `=`（值必须引号包裹的既有
  约束不变）。**一处测试夹具随之修正**：热插安全用例的裸 `a=b` 列名此前
  靠 `isSafeIdent` 兜住、如今在词法层就报 `Token =`（防御提前一层），夹具
  改为引号形式 `'a=b'`——引号包裹的列名仍是一个词元、仍能走到
  `isSafeIdent`，那条注入防线继续被钉住。
- **DROP / DELETE / UPDATE 仍被解析期拒绝**（行为不变，报错文案补上
  WHERE 用法）；事务语义警示已留注记（sqlparse.cpp 与本条）：三语句都改
  结构，必须走 `applySql` 式「全成功或全回滚」，其中 DELETE 的 destroy 是
  物理断电、回滚 ≠ 复活器件，需单独的两段式设计。
- 文档同步：dsl.md（方言语句 + 引号内分隔符清单加 `=`）、agent.md（工具
  表 query_sql 行）、query_sql 工具描述与系统提示词、REPL `/help` 与
  `/sql` 用法提示。

### 2026-09-23 文档勘误：工具表 / 场景数 / 不存在的 --agent-verbose / 单测基线

> 纯文档（含一处注释），零代码行为改动。起因：第八轮审计通读文档时与实现逐条对不上。

- **Agent 工具表曾缺席两个新能力**：`README.md` 的表里只有 5 行，既没有**无条件注册**的
  `query_sql`，也没有两个门控工具 `apply_sql` / `read_variables`（README 全文此前搜不到
  这三个名字）。现补全为 8 行并标出各自的开关；「5 个内置工具」的表述改成实际口径
  （缺省 5 个、开关后最多 8 个），安全设计小节同步写明三个开关**缺省全关且只能命令行给**。
- **`--agent-verbose` / `--verbose` 从来不是本项目的选项**（选项表里没有它，工具日志是
  **恒开**的）：`README.md` 与 `docs/agent.md` 的引用改为如实描述，并同步
  `src/agentaudit.hpp` 的动机注释。
- **E2E 场景数**：`docs/agent.md` 两处与 `docs/testing.md` 一处原写「七场景」，实际早已是
  16（脚本头部与 `test/run_all.sh` 都写十六），本批新增 14/14b/15/15b 后为 **18**；已统一。
- **单测基线勘误**：G1/G2/G3 条目与 `docs/proposals/agent-3rd-plan.md` 原记 `659 / 0`，
  实测为 `570 / 0`——`./build/mybot_tests` 的"通过"是**用例数**（`TH_TEST` 字面量 589、
  `TH_CHECK` 2379 处，都不等于 659）。已就地更正并附勘误说明，避免后续审计拿它当基线。
- `docs/README.md` 与 `agent.md` 里「五个工具」的表述改为不写死数量（工具集还会长）。

### 2026-09-23 回读通道（A）：--read-var 与 read_variables 工具

> 守门快照：C++ 单测 **587 / 0**（本批新增 6 例）、`agent:` 作用域 **239**、
> 零告警构建。新用例已做反向验证（放宽种类判据 → 声明解析用例转红；去掉配置拒绝
> → 配置用例转红）。

- **模型此前无法"主动读"（A，新增能力）**：D 步只让模型看见"动作脚本顺带读到的值"。
  新增 `read_variables` 工具（无参数，一次读全部可读变量），给模型一条主动读通道。
- **为什么可读集合必须由人列名**：agent **无法靠种类区分传感器与执行器**——
  `Servo.lua` / `Relay.lua` 与传感器同属 `kind=lua`，对它们调 `play()` 会真驱动硬件
  （`Relay` 的 `call({})` 会把 delay 转发给 controller）。故：
  ① 新增命令行 `--read-var <名>`（**可重复**），**缺省不提供**该工具；
  ② 配置文件写 `read_vars` **一律拒绝加载并退出 2**（放宽读权限的字段只能命令行给，
  与 `camera_src_dir` 同判据；理由同 `allow_shell`：缺省查找会读 CWD 的 `./mybot.llm.json`）；
  ③ 执行时只放行已注册且种类为 `lua`/`daemon` 的名字（`daemon` 读的是已发布的采样快照，
  无 I2C 流量），其余**跳过并在启动时点名**——不因拼错一个字让整条能力静默失效，一个
  都不可读时该工具不注册。
- **执行与回执**：走 `playQuiet`（不回显），**单个变量读失败写在该行**、不中断整次读取；
  回执 8 KiB 上限、超限截断并标注、截断后 UTF-8 消毒、按「数据不是指令」定界。
  系统提示词只列**已解析**的可读变量（清单为空则一个字不写——承诺一个不存在的工具会
  让模型白烧轮次）。边界如实声明：**读传感器也可能有副作用**（部分器件先写触发寄存器
  再读），所以这是一次派发，照走任务预算与审计。

### 2026-09-23 回读通道（D）：动作脚本回执带上驱动读数

> 守门快照：C++ 单测 **581 / 0**（本批新增 3 例）、`agent:` 作用域 **233**、
> E2E 16 场景通过、零告警构建。新用例已做反向验证（断开回收通道 → 两条集成用例转红）。

- **模型的读数此前被丢在 stderr 里（D，中）**：`run_action_script` 的回执只有一句
  「已执行动作脚本 X（N 字节）」，而器件读到的值**早已产生**——驱动 `call()` 的返回表
  （如 `Timer` 的 `{result="OK",slept=200}`、`BH1750` 的 `{result="OK",LUX=…}`）由
  `play()` 的顶层回显打印，但那条回显走 stderr（SAN-5）：**操作者看得见、模型看不见**。
  于是除摄像头外，模型对硬件状态全盲——按读数闭环类任务（调姿、按距离阈值避障、校准）
  做不了，而这正是 agent 相对"脚本重放"的主要增量。
- **现在**：`run_action_script` 的执行窗口内挂一条**同线程**回收通道（`MtVars_Lua`
  的 `callEchoSink`，thread_local，与 `topDepth` 同款——守护线程各自独立、无写入竞态），
  回显改为追加进该次**工具回执**，随 `tool` 消息进模型上下文；上限 **8 KiB**，超限截断并
  显式标注（不静默丢内容），截断后做 UTF-8 消毒（刀口可能落在多字节序列中间，而一个非法
  字节就够让严格网关 400 掉整个会话），并按「数据不是指令」定界（`quoteUntrusted`）。
  窗口外一切照旧：管道模式的回显仍走 stderr，`playQuiet` 路径本就不回显。窗口是 RAII，
  `run()` 抛异常也精确复原上一个 sink（写成"复原为 nullptr"会让外层挂着的串被静默摘掉，
  且窗口未摘时 sink 指针悬垂——用例专门钉住"复原的是上一个值"）。
- 配套：系统提示词与 `run_action_script` 的工具描述都告诉模型"回执里带读数，据此核对
  硬件状态，不要凭空假设"；`docs/agent.md` 的工具表与 SAN-5 段同步说明。

### 2026-09-23 第八轮审计（SAN-12）：tool_call id 的会话内唯一性

> 守门快照：C++ 单测 **578 / 0**（本批新增 2 例）、`agent:` 作用域 **230**、
> 零告警构建通过。两条新用例已做反向验证（临时恢复旧行为 → 双双转红，其中
> 跨任务那条当场打印 `tool_call_id 'call_0_0' 被回答了不止一次`）。

- **合成 id 在 REPL 的第二个任务上会与第一个任务撞车（SAN-12，中）**：宿主给
  "provider 漏 id" 的响应合成 `call_<turn>_<i>`，而 `turn` 每个任务都从 0 重数
  ⇒ 第 2 个任务第 0 轮又合出 `call_0_0`，与第 1 个任务同名，历史里于是有两条回答
  同一 id 的 tool 消息。服务端 400 `answered more than once`，而那段历史**已越过
  回滚边界**（`complete` 已推进）、`trim` 又不裁最新块 ⇒ 此后每个任务都以同一个
  400 失败，只有 `/clear` 或重启能恢复（与 SAN-1 / AG-4 同一失效形态）。
  原来只有"**同一个任务内**跨轮唯一"的用例看着，跨任务这条路径没有判据。
- **同时补掉同一个口子的另一条来源**：provider 在**同一次响应里**返回重复 id 时，
  宿主此前原样写入——同样是"一个 id 被回答两次"。
- **现在**（`executeToolRound`）：写历史前按"会话内全局唯一"改写 id——漏 id 时合成
  基线不变（`call_<turn>_<i>`，既有用例逐字钉住），与历史冲突时追加后缀，provider
  给的重复 id 从第二条起改用合成 id。**没有任何调用被丢弃或跳过**（改写的只是不
  透明的 id，模型意图分毫未动）。改写成因（重复/冲突）打一行 stderr 告警，**不受
  `verbose` 闸门约束**——"provider 违反了协议"这类事实一旦发生就该看得见。
  说明：`docs/agent.md` 的"响应契约"与"出站一致性自检"两处描述同步更正（后者原写
  "`historyProtocolViolation()` 在写历史时守"，实际它只是可执行判据、写入侧此前没有
  保证，现由本项补齐）。

### 2026-09-23 第八轮审计（H2）：配置文件来源的提示词路径加根约束

> 守门快照：C++ 单测 **576 / 0**（本批新增 6 例，`agent:` 作用域 228）、
> `bash test/run_all.sh agent-all` rc=0、零告警构建通过。新用例已做反向验证
> （临时让判据失效 → 新增用例转红）。

- **`prompt_files` 此前是「读任意文件」的原语（H2，中高）**：提示词包内容会进系统提示词、
  随**首个**模型请求发往 endpoint，而 endpoint 同样由配置文件决定；读取又走 `read()` 循环
  （`util::readFileCapped`，不依赖 `stat` 长度），于是 `/proc/self/environ` 这类伪文件照样读得到。
  合起来是一条**不需要任何开关**的链路：一份落在 CWD 的 `./mybot.llm.json` 写
  `{"endpoint":"<攻击者>","prompt_files":["/proc/self/environ"]}` 就能把操作者环境块
  （含宿主重新注入的 `BOT4CJ_LLM_KEY`）整块外发。这与 H1 的 `camera_src` 同族
  （「配置文件不该授予读权限」），故同判据。
- **现在**：**命令行** `--prompt-file` 不受限（操作者本人授意）；**配置文件来源**的每一项必须落在
  当前工作目录或 `BOT4CJ_PROMPT_DIR`（冒号分隔，新环境变量）显式列出的目录之内，判定走
  `realpath`（`..` 与软链逃逸都拦），越界**在上电之前**报错退出 2，报错点名条目、允许的根与
  两个出路。不存在的条目不在这一层拦（仍由加载期报「路径不存在」，避免把拼写错误报成越界）。
  文档化的相对写法 `"prompt_files": ["prompts/spider_robot"]` 天然在 CWD 之内，照旧可用；
  现场把提示词包放在工作目录之外时用 `BOT4CJ_PROMPT_DIR` 显式放行（配置文件写不了该变量）。
  说明：`docs/agent.md` 安全说明新增第 11 条，`docs/reference.md` 补字段与环境变量登记。

### 2026-09-23 agent 第三轮定点收口（G1/G2/G3）

> 守门快照：C++ 单测 **570 / 0**（本轮新增 3 例）、零告警构建通过。
> （**勘误 2026-09-23**：本条原记 `659 / 0`，与实测不符——`./build/mybot_tests` 的
> "通过"是**用例数**（当时 570；`TH_TEST` 字面量 589、`TH_CHECK` 2379 处，均不是 659）。）
> 本轮是 agent 部分的定点收口，三处都是真实缺口，各配判别力用例并做了反向验证
> （G3 实测撤销修复后新用例转红）。方案见 `docs/proposals/agent-3rd-plan.md`。

- **`run_command` 回执的敏感值遮蔽（G1，高）**：子进程环境已按整词清洗，但命令**自己**打印到
  stdout/stderr 的 `KEY=value`（如模型执行 `env`、`cat` 配置文件）此前原样回传模型上下文，
  可经同一配置可控的 `endpoint` 外发。现在回执在**进模型上下文之前**过一次
  `maskSecretsInText`（与审计日志的 `args.command` 同一单点），并在发生替换时追加「已净化」提示——
  遮蔽是有损的，模型要知道结果被改过。注意这仍是文本级防线（编码后打印即可绕过），与既有两道同档。
- **`apply_sql` 的生效摘要进审计（G2）**：热插是与 `run_command` 同级的高危操作，此前审计只留
  SQL 原文与 ok/error，复盘「到底改了什么」要重跑。现在工具行的 `summary` 字段记工具自报的
  结构化生效信息（新增几张表/几个变量/几行）。只做加法，老工具行不出现该键，既有 schema 逐字节不变。
- **一次性任务非标准异常退出补齐会话结束记录（G3）**：`runAgentMode` 的 `catch(...)` 分支
  （Lua 错误的实际形态）原先直接 `return 1`，于是「任务因 Lua 错误结束」的会话在审计里
  **连一条 session 记录都没有**——正是 SAN-9 要消除的形态，只因 `catch(const std::exception&)`
  分支修过而遗漏。现在两条失败路径都补写 `status=error` 的会话结束记录。

### 2026-09-22 第七轮审计修复批次·第二批（H1 + 三条 P1 + P2 批）

> 守门快照（变基到 `fc5fd9b` 之后复测）：全量守门 **267 / 0 / 0**（`bash test/run_all.sh`，
> rc=0）、C++ 单测 **543 / 0**（`agent:` 作用域 173）、`llm 协议` 单测 **215 / 0**、
> agent E2E **十四场景**通过（本批新增 5b）、零告警构建（`-DMYBOT_WERROR=ON`）通过。
> 注：全量守门的条数不随 C++ 用例增长——`cpp-unit` 整段折成 1 条（这正是门禁轨道
> 报出的"分段 banner 折叠"问题，见 `.tmp/bot4cj-issues-r7-20260921.md` 12.3）。
> 本批修的是上一批**范围之外**的存量缺陷：r6 清单里的 H1，以及本轮三路并行审计
> （Lua 层 / 宿主核心 / 门禁）各自报出的 P1。每条都配了判别力用例并做了反向验证。
>
> **变基说明（变基到 `fc5fd9b`，含远端 SAN-1…SAN-11 与 PL 系列）**：本批写在 agent 重构
> （阶段 A1/A2/B1–B4/C1–C7）之前，落点已随重构迁移——A4 墙钟预算并入 `TaskBudget`
> （`maxSeconds` / `exhausted()`，`resetTask()` 同时重置墙钟起点）；A5 的 `catch(...)` 落在
> 唯一派发漏斗 `DispatchGate::run` 与 `AgentToolRegistry::execute`；H1 的 `src_root` 解耦落在
> 新抽出的 `buildCameraParams`；A6 的编译期开关落在 `runShell`（探针经 `ChildSpec::traceFd`
> 传递）。三处与远端**撞车**的改动按「同一缺陷只留一份修法」处理：
> ① **A1 让位给 SAN-1**——两边独立得出同一结论（截断说明另起一条 tool 消息并复用
> `tool_call_id` ⇒ 同一 id 被回答两次 ⇒ 网关 400，且落在不可回滚的历史里），修法取
> SAN-1 的那种（说明追加进本轮 **assistant 文本**）：它不给工具结果通道引入「可追加」
> 语义、也不牵动 `TaskBudget` 的记账口径。本批保留的是 **A1b**（`llm_wire_openai.
> check_tool_pairing` 出站自检），与宿主侧 `historyProtocolViolation()` 构成写历史 / 发报文
> 两道判据，不重复实现——测试里本地那份 `pairingViolation` 已删除，改用产品侧单点。
> ② **A5 的回执口径对齐 SAN-4**：`catch(...)` 不再一律填 `executed=false`，按 `argsOk`
> 分两种——参数解析阶段抛出 = 未派发（副作用必然未发生），工具/确认程序阶段抛出 =
> 可能已驱动硬件，回执如实写「副作用可能已发生，请先确认硬件状态」。
> ③ 用例 `agentloop_caps_calls_per_round_and_tells_the_model` 取远端 SAN-1 版本，
> `agentloop_round_cap_note_reaches_model` 改钉新落点（assistant 文本 + tool 消息保持纯净）。
> 另修掉一处继承自 TD-3 的**假绿**——`test_sensor_daemon_multi.lua` 的字段预算写死
> 「33 > 32」，而共享槽容量已 32 → 64，判据够不着（实测放行），改为按 `SLOT_MAXVALS`
> 取值并补「正好装满必须放行」的边界对照。

- **`capture_image` 的模拟图片白名单恒真（H1，高）**：驱动侧判据是「`realpath(src)`
  必须落在 `src_root` 之内」，而 `src_root` 由宿主用 `canonical(camera_src).parent_path()`
  推出来 —— **src 的所在目录当然包含 src**，判据恒真，白名单形同不存在。可达链
  **不需要 `--agent-shell`**：`allow_camera` 缺省为真、`cam0` 自动注入，一份落在 CWD 的
  `./mybot.llm.json`（缺省查找会读它）写 `"camera_src":"/etc/shadow"` 就能把该文件
  base64 读进模型上下文，再由**同一份配置**可控的 `endpoint` 外发；而
  `agenttools.cpp` 的注释此前明确声称「任意路径读（/etc/shadow 之类）被拒」——
  注释描述的防线恰恰是不存在的那条。现在 `src_root` 与 `src` **解耦**，三条口径：
  ① 新增 `--camera-src-dir`（显式根，**命令行独占**：它放宽读权限，配置文件写
  `camera_src_dir` 一律拒绝加载并退出 2，与 `allow_shell` 同判据）；② 没有显式根时，
  **只有命令行**的 `--camera-src` 才用其所在目录当根（保住"不接摄像头跑通全链路"的
  快速上手）；③ 配置文件来源的 `camera_src` **一律不推导**，交由驱动的
  `BOT4CJ_CAMERA_SRC_DIR` 判定，都没有即默认拒绝（驱动的「未放行」文案已改成点名
  三条出路）。守门：C++ 侧四种组合各一条断言 + 配置面来源标记/拒绝各一条 +
  agent E2E **场景 5b**（配置文件来源的 src 必须被拒、线上报文不含 `image_url`——
  这条覆盖的是 `buildAgentConfig → runAgentMode → 工具上下文` 的**接线**，两端单测
  各自绿也证明不了它在）。**反向验证已做**：把推导条件退回"无条件"，用例当场转红，
  且回执里真的出现了那张图。
- **`SINGLE_CONSUMER` 判据对守护行有三条静默旁路（P1）**：旧实现把「缺列」当成
  「不参与判据」直接 `continue`，而守护侧对 `proto`/`path` 都有缺省
  （`cfg.proto or "i2c"`、`proto=="spi" and "/dev/spidev0.0" or "/dev/i2c-1"`），
  多器件守护的器件清单又在 `devices` 列里（`driver@addr@fields[@k=v]`，`|` 分隔），
  宿主从不解析 ⇒ 三种被 `docs/daemon.md` 认可的写法（含最小写法
  `INSERT INTO Daemon (name, chunk)`）全部绕过判据，运行期却真的在总线上开了 fd。
  后果是五个声明者（`AS7262`/`MS5611`/`VL53L0X`/`VL53L4CD`/`AS3935`）可被
  「直连 + 守护」双消费，器件侧地址相位互踩、**双方都报 OK**——漏标没有任何运行期
  症状，只能靠装配期判据。现在缺列按守护侧同一规则补全后登记，`devices` 列逐个
  spec 解析（报错点名 `devices[N]` 与驱动名）；`driver` 列与 `devices` 列互斥
  （否则同一行会自己和自己冲突）。缺省值以 `HwFactory::kDaemonDefault*` 为单点，
  与 `sensor_daemon.lua` 的缺省表达式由新用例 `daemon_defaults_match_lua_source`
  **读 Lua 源两侧比对**（这类漂移没有运行期症状）。守门：省略 proto/path 的守护行
  必须判重、显式 `proto=virt` 仍放行、`devices` 里声明者驱动必须登记、未声明者驱动
  的双消费者照常放行。**反向验证已做**：分别退回"缺列即跳过"与"不解析 devices"，
  两条用例各自转红，而五条既有用例仍绿。
- **`@Include` 归并只数输入、不数产物（P1）**：输入侧已有三道限（`@Include` 行数
  ≤32、字节 ≤8 MiB、`|phase|` ≤100000），**产物侧一条都没有**：产物行数
  = `max(rowsAcc, rowsNext + phase)`、列数随包含数线性增长 ⇒ 几十字节的动作脚本
  写一行 `@Include path=..., phase=100000` 就能把终态撑到 ~1e5 行 × 数十列
  （数百 MB），树莓派上直接 OOM；而 OOM 是 SIGKILL 语义，`deactivate()` 整体被跳过
  ——正是「断电是生命周期不变式」要防的后果。现在 `mtmerge::mergeTwo`（两两归并的
  **单点**，mtmerge 工具与 `@Include` 展开共用）在**写任何一行之前**判产物预算：
  行数 ≤ `kMaxMergedRows`(20000)、单元格 ≤ `kMaxMergedCells`(200000)，报错点名两侧
  输入、实测行×列与可操作出路。取值给真实用量留了两个数量级以上余量（仓库内
  195 个 `.mt` 实测极值：200 数据行 / 20 列 / 760 单元格）。守门：相位放大、列宽放大、
  真实规模放行、恰好等于上限放行（钉住判据是 `>` 不是 `>=`）、以及 `@Include`
  层面的接线各一条。**反向验证已做**：摘掉两处 throw，三条用例转红、两条对照仍绿。
  顺带把既有的字节预算用例夹具从 `@Action` 换成 `@Alias`——6.8 万行的 `@Action`
  现在会先撞上产物预算，那条用例就测不到它要测的"字节总量"了（实测踩过）。
- **P2 批（六处，各自独立）**：
  - **内嵌 NUL 击穿"全串消费"判据**：`parseLongStrict` / `parseFiniteDouble` 走
    `c_str()`，`strtoll`/`strtod` 在第一个 NUL 处停下而 `*end` 恰为该 NUL ⇒
    `90\0junk` 报 OK/90，**尾随垃圾静默丢弃**。两个单点各加一道前置判据；
    agent 工具参数入口 `requireString` 同族补齐（`run_command` 收到
    `"id\u0000rm -rf /"` 此前只会执行 `id`，回执看不出命令被改过——`script` 参数
    早有 `checkRelPath` 的 NUL 判据，两个入口不该一有一无）。
  - **`DAEMON_CONFIG` 把矩阵空占位符 `-` 原样下发**：`-` 在本仓语义里是"未提供"
    （`docs/daemon.md` 与三处 `daemonConfigMissing` 判据同一口径），但拼进配置串后
    Lua 侧拿到字面值 `"-"`（truthy）⇒ `cfg.sign or 缺省` 的 `or` 分支不生效：
    `sign='-'` 让符号扩展静默失效、驱动 build 参数 `'-'` 经 `tonumber` 得 nil 后
    静默改用驱动自身缺省。现在占位符在**拼串之前**跳过，"校验看到的"与"守护拿到的"
    是同一份配置（空串不在此列：`interval` 为空串仍按"需要整数"报错，不静默取缺省）。
  - **`loadfile` 的错误返回值被沙箱包装吞掉**：`lua_call(L, 1, 1)` 把结果截到 1 个，
    而 `loadfile` 的契约是 `(chunk, errmsg)` ⇒ `sensor_daemon_multi.lua` 的
    `local f, err = loadfile(...)` 里 `err` 恒为 nil，"驱动加载失败"的 `-8` 日志永远
    只打 `(nil)`。改用 `LUA_MULTRET` 并回报实际结果个数。
  - **槽容量二次硬编码**：`sensor_daemon_multi.lua` 的 `MAX_VALUES = 32` 改读
    `P.SLOT_MAXVALS`（唯一事实源是 `daemonvar.hpp` 的 `SLOT_MAXVALS`，经
    `DAEMON_SLOT_LAYOUT` 注入）。写死 32 的后果是两侧各自漂移：C++ 侧改窄 ⇒ 预算
    检查放行、`pthread.lua` 再静默钳制 ⇒ 尾部字段**永不出值**且无报错。
  - **build 期不响应停止标志**：`active()` 的逐件 build 循环不查 `stopRequested()`，
    而信号处理器在它之前一行就装好了 ⇒ 多驱动上电期间第一次 Ctrl-C **毫无可见效果**，
    运维必然再按一次，而第二次是 `SIG_DFL + raise` 强杀：已 build 的器件原地带电、
    `deactivate` 一行不执行。现在第一次按键即中止装配并上抛，由 `runRobotAndCleanup`
    的 catch 走 deactivate 逆序断电。
  - **两处文档/注释与实现不符**：`mtinclude.cpp` 把自己列进"信任边界"硬限清单的
    `kMaxIncludeDepth` **全仓不存在**（深度实际由全树 `@Include` 行数预算间接界定），
    已按实况改写并补上新的产物预算；`shellKeyExposureWarning` 与 `actionsource.hpp`
    仍写着「`--agent-shell` 或 `allow_shell=true`」，而自 `f06b712` 起配置文件已不能
    开启 shell，两处文案改成与实现一致。
- **守门自身：产物陈旧不再静默（本轮亲历）**：`test/run_all.sh` 此前只判「产物存在」、
  不判新旧 ⇒ 改完源码直接跑守门，跑的是**上一轮的二进制**，汇总照样给出像样的
  「N 通过, 0 失败」。本轮实测：445 通过 / 1 失败，而 445+1 恰好等于改动前的用例数
  （那条失败还是"旧二进制 + 新 docs"的组合产物）。这类假绿比缺产物更危险——缺产物有
  `artifact_missing` 兜住并计入失败，陈旧产物不留任何痕迹。现在守门开头比对
  **编译期输入**（`src/`、`test/cpp/`、`CMakeLists.txt`；`docs/`/`modules/`/`robots/`
  是运行期读取的，纳入只会误报）与产物 mtime，发现陈旧即退出 2 并给出重建命令；
  逃生口 `BOT_ALLOW_STALE=1`。**实测**：`touch src/util.hpp` 后守门立刻拒绝（rc=2），
  带逃生口则放行。
- **本批未做（如实留痕）**：**弃管守护的"写能力掐断"（P1）仍未落地**。现状是
  `runTickLoop` 只在两拍之间查停止标志，tick 卡在不可中断的总线事务里就会被弃管，
  二段宽限封顶 30 s 后主线程继续逆序 destroy（关 fd、下发关断），而该线程醒来仍可能
  再写器件一次 ⇒ 进程退出瞬间器件可能重新带电；`bot4cj-fix-plan-20260920.md:23`
  把 1.1/1.2 一并记成 ✅ 属**过度申报**（1.2 的两层机制全仓零命中）。已定设计：
  ① HAL 侧 fd 登记表加 `revoked` 位，在**总线访问的单点**（而非逐驱动）短路成 -1——
  这是唯一的本质收口，因为驱动是直接在 tick 里写器件的；② 槽内 `dead` 标志让
  `publish` 短路（只挡发布，挡不住写器件，故必须与 ① 配套）。未在本批动手的原因：
  改动落在 `HAL.lua` 这个被全部 47 款驱动共用的单点上，且"停止请求后立刻让总线访问
  失败"会改变**优雅退出**窗口的行为（正在收尾的那一拍会拿到 -1），需要单独一轮
  配合真机夹具验证，不适合与上面这些改动混在一批里提交。

### 2026-09-21 第七轮审计修复批次（AG-27 + A1–A8：协议形状、墙钟预算、假红门禁）

> 守门快照（变基到 `fc5fd9b` 之后复测）：全量守门 **267 / 0 / 0**（`bash test/run_all.sh`，
> rc=0）、C++ 单测 **543 / 0**（`agent:` 作用域 **173**，本轮 +10）、`llm 协议` 单测
> **215 / 0**（本轮 +19）、骨架自证 8 / 0、零告警构建（`-DMYBOT_WERROR=ON`）通过。
> 本轮起点是 AG-26（`44f6ee1`）：它要修的两条缺陷（单轮 `tool_calls` 数量无界、提示词入口不消毒）
> **判据成立**，但两条修法各引入一个新缺陷，且是近期唯一一条「改语义 + 写新历史 + 零测试 +
> 无 changelog 条目」的提交。下面 A1/A2/A3 是对它的修正，AG-27 是本轮在门禁上抓到的假红。

- **AG-26 的截断说明写出一条协议不完整的 tool 消息（A1，高）**：超限调用被 `resize` 掉之后，
  说明被写成**额外一条** `tool` 消息，并复用最后一条已派发调用的 `tool_call_id` ⇒
  N 个 `tool_calls` 对上 N+1 条 tool 消息、且有一个 id 出现两次。原注释的前提
  （「tool 消息需引用已存在的 tool_call_id 协议才成立」）是错的：协议要的是**一一对应**，
  不是「引用一个存在的 id」。后果比原缺陷更疼——这块历史已被 `runTurn` 的 `complete`
  边界认作「可原样发送」，`trimHistory` 又不裁最新块，于是下一轮起**每条任务都以同一个
  请求体拿 400**（与 AG-4 描述的失效形态逐字同构）。现在把说明**并入最后一条 tool 消息
  的正文**，形状恒为 N⇔N，「如实告知被截断」的原意完整保留；说明本身按 AG-21 口径计入
  任务内预算。触发不需要攻击者：`maxCallsPerRound` 缺省 16，一句「把这 20 个脚本依次跑一遍」
  就会命中。
- **AG-26 的新语义零测试（A2，中高）**：`grep -rn maxCallsPerRound test/` 为空——而 A1 的形状
  缺陷正好落在既有 `agentloop_injects_tool_image_after_all_tool_messages` 那一族断言的能力范围内。
  本轮补 3 条（形状合规、说明到达模型且措辞不谎报、`0` = 不限），并新增一个与 Lua 侧
  `check_tool_pairing` **同判据**的 C++ 助手 `pairingViolation`（数量 / 顺序 / id 重复 / 多一条）。
  **反向验证已做**：把实现退回 AG-26 第一版（多写一条 tool 消息），两条用例当场转红。
- **出站一致性自检（A1b）**：`llm_wire_openai.check_tool_pairing` 在**发包前**校验
  `tool_calls` ⇔ `tool` 一一对应，不通过即 `error` → 驱动 `pcall` 折成 ERR 契约 →
  宿主给出点名「第几条消息、期望几条、实际几条」的报错，而不是把不配对的报文发出去换一个
  含混的 400。落点刻意排在消息转换**之后**：字段级缺失仍由 AG-15 的 `require_wire_string`
  先报（成因更具体），自检只管形状。新增 14 条离线用例（少一条 / 多一条 / id 不匹配 /
  id 重复 / 缺 id / 图片消息夹在回执之后 / 纯文本历史）。**反向验证已做**：摘掉
  `build_request` 里的调用，2 条接线用例转红。顺带把 AG-17 两个夹具补成协议合规形状
  （原先 assistant 带 `tool_calls` 却没有配对回执，被自检正确拒绝）。
- **脚本清单把「消毒名」当「寻址名」（A3，中）**：AG-26 给清单里的相对路径补 `sanitizeUtf8`
  是对的，但它消毒的是**同一个串**、随后又拿它去 `open` ⇒ 文件名含非法字节时
  ①首注释静默消失（连告警都没有）②清单交给模型的是磁盘上**不存在**的名字，模型照抄去
  `run_action_script` 必然「无法打开文件」，直接违反该工具「script 必须来自清单」的约定。
  现在 `raw`（寻址）与 `shown`（展示）分开保存、按展示名排序，两者分叉时在清单里显式标注
  「该条不可用于 `run_action_script`」。新增 1 条用例（造一个名字带 `0xff` 的 `.mt`，
  断言清单整体是合法 UTF-8、含 U+FFFD 与标注、且**首注释仍在**）。
- **单任务墙钟预算（A4，中）**：体积预算（AG-4/AG-21）与单轮条数上限（AG-26）管的都是
  「往上下文里灌多少」，**没有一条管时间**，而 agent 模式全程 `robot.active()` 带电常驻：
  `maxTurns(≤100) × 单次模型调用 timeout(≤600s) × 单轮工具数` 相乘无封顶 ⇒ 最坏一个任务
  能让执行器带电十几小时，其间唯一出路是 Ctrl-C，而第二次 Ctrl-C 是强杀（恰好跳过
  `deactivate`）。新增 `--llm-task-timeout` / 配置 `max_task_seconds`（缺省 **900s**，
  `0` = 不限；区间 0–86400 收在 `llm_limits` 单点）。判定点两处、语义不同：
  ① 轮循环开头不再发起新的模型调用，按「达到预算」如实失败；② 一轮**中间**耗尽时剩余调用
  不派发、但每条各留一条「未派发（不是执行失败，也没有产生任何副作用）」回执——少一条就是
  服务端 400，所以这里不能 `break`。实际超限量因此最多是「一次模型调用 + 一条工具」。
  新增 5 条用例（两个判定点各一条 + `0` = 不限 + 配置三层优先级与越界 + CLI 解析与
  agentOnly 拒绝）；`--help`、`docs/reference.md`、`docs/agent.md`（含「已知限制」里的
  三条确切边界）、`mybot.llm.json.example` 同步。
- **agent 会话路径只接 `std::exception`（A5，低）**：本仓自陈「本机 LuaJIT 用 C++ 异常实现
  `luaD_throw`，从没有 Lua CFRAME 的 C++ 帧抛出的 Lua 错误**不是** `std::exception`」
  （`main.cpp` 的 `runRobotAndCleanup` 正是为此才 `catch (...)`），但
  `AgentToolRegistry::execute`、`AgentSession::dispatchOneCall`、`runAgentMode` 的
  一次性任务与 REPL 四处都只接 `std::exception` ⇒ 这类抛出会把「单轮失败」升级成
  「整个 REPL 结束」，且该轮已执行的硬件动作被回滚出历史（模型被告知「什么都没发生」，
  与 AG-4 口径相反）。四处各补 `catch (...)`，工具层折成 tool 回执（那是唯一保持
  N⇔N 形状的选择）。
- **fd 自报探针在生产二进制里也活着（A6，低）**：AG-11 的探针此前只靠「运行期不设
  `BOT4CJ_TRACE_FDS`」这一条巧合与生产隔离，于是 `mybot` 也带着「按环境变量向任意路径
  追加写（0644）」的能力。现在加编译期开关 `MYBOT_TEST_TRACE_FDS`（CMakeLists 只给
  `mybot_tests` 定义；`MYBOT_CORE_SOURCES` 被两个目标**各自编译一份**，所以真能做到生产
  二进制里没有这条路径）。只 `ifdef` 环境变量读取那一处而不 `ifdef` 整个函数：后者会让
  两个函数在关掉开关时变成未使用函数，撞 `-Wall -Werror`。
- **provider 回传的 `id` / `name` 无长度上限（A7，低）**：两者都来自远端、却会进历史并
  **逐轮原样回传**。新增 `proto_limits` 单点（id ≤ 128 B、name ≤ 64 B，均远大于真实用量：
  最长内置工具名 `run_action_script` = 17 B），超限即整条响应拒绝并点名实测字节数——
  id **不能截断**（截断会破坏与 `tool_calls` 的配对），所以只能 fail-closed。
- **AG-16 门禁在树莓派上是假红（AG-27，中）**：`bash test/run_all.sh` 在本机长期
  **261 通过 / 1 失败**，失败项是 `test_llm_drivers.lua` 的「post 之后句柄已复位」。
  根因不在产品代码，而在**读句柄的库不是建句柄的库**：用例 `ffi.load("curl")`，
  而 `llm_http` 按 `CANDIDATES` 先试 `libcurl.so.4`；本机（Raspberry Pi OS）
  `libcurl.so → libcurl-gnutls.so` 与 `libcurl.so.4 → libcurl.so.4.8.0` 是**两个不同文件**，
  于是进程里有两份 libcurl 实例（实测 `curl_easy_init` 地址 `0x7fff802a6d80` vs
  `0x7fff7eb65c60`）。用 B 实例的 `getinfo` 读 A 实例的 easy handle，等于按另一套结构体
  布局解引用不透明指针——实测读出错位垃圾（端口串 `"1"`）；同一句柄改用 A 实例读，
  reset 前是完整 URL、reset 后为空，**产品代码一直是对的**。CI（bookworm：两个名字指向
  同一文件）只有一份实例，所以那边绿。修法：`llm_http` 新增 `M.libname()` 暴露实际加载的
  库名，用例一律经它拿**同一份**实例；并补一条**阳性对照**（同实例下武装后非空、reset 后
  为空），否则常量写错时「post 之后为空」会空洞地通过。两份实例并存时向 stderr 注记一次
  （环境事实，不判失败）。**反向验证已做**：删掉 `Session:post` 里的 `curl_easy_reset`，
  该用例转红。这条也说明「红门禁长期没人修」的代价：它从此不再具备判别力。
- **三处瑕疵（A8）**：① `agenttools.cpp` 的 `sniffImageFormat` 里遗留 `( void )0;` 死语句；
  ② `shellKeyExposureWarning` 与 `actionsource.hpp` 的注释仍写着「`--agent-shell` 或
  `allow_shell=true`」，而自 `f06b712` 起配置文件**已不能开启** shell——正是
  `formatRunResult` 刚修过的「文案与实现漂移」同类，两处都改成与实现一致；
  ③ `llm_wire_deepseek.build_request` 只声明两个形参、`M.request` 却按三个传，
  那个 `plan` 是**死参**（签名与 openai 库对称、语义却不同），改成接受可选 `plan`
  并优先使用（行为不变）。

### 2026-09-22 响应形变矩阵（SAN-11，纯测试 + 契约成文）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-11。
> **只补测试与文档，不改对外行为**（除下面那一条归一化）。

- **缺口形态**：契约一直存在但**分散**——宿主只读 `result`/`content`/`tool_calls`/
  `extras`/`usage`，驱动负责把 provider 报文映射成这个形状。问题是历史补法：
  PL-3（数组形态 `content`）与 PL-6（`tool_calls` 非表）**都是线上撞到才补一个**，
  补完各自加了用例，但**没有矩阵**——所以下一个形变还要等线上撞。
- **新增 `test/test_llm_wire_contract.lua`**：表驱动形变矩阵，**261 条断言**，
  两个内置驱动（`openai` / `deepseek`）各跑一遍（它们是同一份 `parse_response`，
  这样"某天有人给 DeepSeek 换了自己的实现"会被立刻发现）。覆盖：
  `choices` 缺失/空/非数组/元素非表、`message` 缺失/null/非表、`content`
  null/数字/空数组/纯 text 数组/含 image_url 段/单 part 对象、`tool_calls`
  字符串/数字/空数组/元素非表/元素 null/缺 `function`/`function` 非表/缺 `id`/
  `id` 非字符串/`arguments` 为对象或数字/合法与畸形混合、`reasoning_content`
  空串与非字符串、`usage` 缺失/非对象/对象，外加 `perform` 层六种 HTTP 形态
  （200 非 JSON / 200 JSON 标量 / 200 + error 信封 / 空 body / 429 / 404）。
  每行标结局：**点名报错** / **降级但记账** / **照常处理**。
- **一处归一化（本次新增的行为）**：`content` 为数字/布尔等非字符串非数组值时
  **归一成空串**——原样透传会让宿主只认字符串的判据读成"没有文本"，最终报
  「既没有返回文本也没有工具调用」，报错完全不指向真因（与 PL-3 同一个坑）。
- **宿主侧对应矩阵**：`test/cpp/test_agentproto.cpp` 加
  `agentproto_host_side_shape_matrix`——缺 `name` 的调用丢弃后如实报数、
  有文本时安静丢弃、非字符串 `id`/`arguments` 的容错、`tool_calls` 非数组落在
  既有兜底错误上、`extras`/`usage` 透传与缺失、`result != "OK"` 带上驱动 message。
- **契约成文**：`docs/agent.md` 的「自己写第三个驱动」一节补上完整的**响应契约表**
  （必选/可选键、结构化错误判据四键、形态判据清单），并写明维护约定：
  **遇到新的 provider 形变先往矩阵里加一行**。
- **注册**：新 Lua 用例已登记进 `agent_lua_roster`（SAN-2 的反向判据会检查
  `test/test_llm_*.lua` 是否全部登记，漏登记即红——这正是那条自证的价值）。
- **判据**：本项**不改对外行为**；若某一格行为不可接受，另开一项而不是顺手改
  （保持"补测试"与"改行为"两个提交分离，便于回滚）。


### 2026-09-22 REPL 新增 `/status`（SAN-10）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-10。

- **问题**：现场最常见的三个问题——"为什么它忘了之前说的？""为什么拍照没进来？"
  "刚才失败了几次、花了多久？"——**都要翻 stderr 全文或重开会话**才能回答。
  答案其实都在内存里（`HistoryManager` / `TaskBudget` / 会话统计），只是**没有出口**：
  历史大小只在**裁剪发生的那一刻**打一行日志，预算用量只在**超限时**才出现。
- **修法**：REPL 新增 `/status`，读**已经存在的状态**并渲染成一览：
  模型/接入点/超时/轮次上限、本会话模型轮数与耗时、工具调用数与失败数、
  **provider 最近一次给的 token 用量**（SAN-9 的 `usage` 透传）、任务块数与历史字节
  （含"是否裁过、裁了多少"）、任务内文本/图片预算用量、审计落点与 session id、
  硬件变量数、脚本目录。
- **三条如实口径**（不显示假数字）：预算为 `0`（= 不限）时显示"不限（已用 …）"
  而不是 `0 / 0`；历史未裁剪时显示"未触发裁剪"而不是"裁过 0 条"；
  审计未开启时显示"(未开启)"，与 `--check-config` 摘要里 endpoint 的
  "(未配置，首次调用将失败)" 同口径。渲染是**纯函数**（`summarizeSessionStatus`），
  与 `summarizeAgentConfig` 同款契约：只渲染、不判定、无副作用。
- **钉子用例**（+2）：① `/status` **绝不触发模型调用**（fake transport 一次都没被
  调用）——这是新命令最容易写错的地方：少了 `continue`，用户想看一眼状态就会真的
  启动一次模型调用（可能还带工具动作）；② 空会话显示 `模型 0 轮`、预算不限时显示
  "不限"、审计未开时显示"(未开启)"，且不得出现 `0 字节 / 0 字节` 这种看起来像
  "额度已满"的假数字。
- **反向验证**：把 `/status` 分支的 `continue` 去掉 → 第一条用例转红
  （实测 `tr.requests` 有 1 次调用）。
- **顺带**：`HistoryManager::accountedBytes()`（与 `trim` 的记账口径同源）、
  `AgentSession::stats()` 与三个只读读数——都不新增计数器语义，`/status` 只是它们的渲染。


### 2026-09-22 审计轨迹补上会话边界与模型轮次（SAN-9，加法演进）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-9。
> 这是**提案+实现**：`agent-refactor-plan-2.md` 曾声明"不改审计 JSONL schema"，
> 故本项用**加法**兑现——既有工具行的字段名、顺序、语义**一个不动**，
> 新记录靠新增的 `kind` 字段区分。

- **问题（读码 + 实测）**：审计此前**只记工具调用**（唯一写入点是派发漏斗）。
  两个后果：① "模型说了什么、花了多久、这一轮有没有真的发出请求"完全不可查；
  ② 一次在**模型调用阶段**失败的会话（正是 SAN-1/SAN-3 讨论的那类失败）在日志里
  **一条记录都没有**——而那恰恰是最需要复盘的时候。
- **新记录类型（`kind` 显式写在每行，工具行也写 `"tool"`——不做"缺省即工具"的
  隐式约定）**：
  - `kind="session"`：会话头（`status:"start"`）与尾（`"end"` / `"error"`），
    带 `driver` / `model` / `endpoint` / `api_key_source`。
    **绝不写密钥**——只写来源枚举名（`env` / `config_inline` / `config_file`），
    与 `--check-config` 摘要同口径。
  - `kind="model"`：每轮模型调用一条，含 `ms` / `req_bytes` / `resp_bytes` /
    `attempts`（>1 即发生过重试）/ `prompt_tokens` / `completion_tokens`
    / `status`（`ok` / `error`）。**失败也记**——那是复盘"这一轮为什么没跑起来"
    的唯一线索。
  - `kind="tool"`：既有形态，字段与顺序逐字不变。
- **接入点脱敏**（顺带堵住一个真实泄漏面）：`endpoint` 落盘前剥掉 **query 与
  userinfo**——部分网关把凭据放在 URL 里（`https://user:tok@host/v1/…?key=…`），
  而审计是 append-only 的落盘文件。实测：配置里写
  `…/chat/completions?key=SECRET&u=admin`，日志里只剩 `…/chat/completions`。
- **token 只记 provider 给的数**：`usage` 由驱动**原样透传**（宿主不理解字段名），
  宿主只挑 `prompt_tokens` / `completion_tokens` 记账，**不做任何推算或计费估算**；
  provider 没给就不出现这两个键。
- **兼容性钉子**：新增两条用例——① 三类记录各自的字段集合（含"会话记录不带工具
  字段"）；② **老消费方的过滤表达式语义不变**（`tool == "run_command"` 仍只命中
  那一条工具行）。E2E 的场景 7 也据此收窄：原断言「每行都有 tool/ok」在 SAN-9 后
  不再成立，改为「每行都有 ts/session/kind」+「工具行的字段齐备（按 kind 过滤）」。
- **反向验证**：把 `kind` 从工具行里去掉 → 兼容性用例转红（老过滤器会把
  session/model 行误判成工具行）。
- **实测样例**（一次 `--task` 跑完）：
  `session(start) → model → tool → model → tool → model → tool → model → session(end)`，
  共 9 条；`?key=SECRET` 与 URL userinfo 均未落盘。

### 2026-09-22 参数错误不再说成"执行失败"，丢弃的调用如实报数（SAN-8）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-8。
> （该条目的第①项「截断写日志」已随 SAN-1 一并落地。）

- **问题①（参数错误被说成执行失败）**：`run_action_script` 把
  `requireString(args,"script")` 放在**执行用的 try 之内**，于是模型漏传参数时收到
  「动作脚本执行失败 []: 缺少字符串参数 script」——而该工具的描述里写着
  「报错时机器人状态**可能停在半途**」。模型据此以为可能发生了物理后果，实际
  什么都没跑。同样的口径问题存在于 `run_command`（「工具执行失败: 缺少字符串参数
  command」）。
- **修法①**：`AgentToolResult` 增 `paramError` 标志，由 `AgentToolRegistry::execute`
  按**异常类型**（`IllegalArgumentException`）标出——参数校验统一抛它，而它只在
  "调用还没到达硬件"时出现。`DispatchGate` 据此把回执改成「工具参数错误: …」并把
  `executed` 置 `false`（顺带与 SAN-4 接上：这类结果不会被预算降级改写成
  "副作用已发生"）。**判据是异常类型，不是文案匹配**——后者正是 SAN-3 证明会静默
  失效的形态。参数提取同时移出执行 try，两条路径各归其位。
  注册表直接调用方（少数单测）的文本一字未变，所以那 4 条既有用例零修改。
- **问题②（丢弃调用后报了句假话）**：`parseDriverResponse` 丢弃 `name` 为空的工具
  调用后，整轮为空时抛「模型既没有返回文本也没有工具调用」——**而模型确实返回了
  工具调用**（只是缺 `function.name`）。且片段给的是规范化后的表，看不到 provider
  的原始形状。
- **修法②**：如实报数——「模型返回的 N 条工具调用全部无法派发（缺 function.name
  或形态非法，已丢弃），且没有文本内容」，N 为**宿主丢弃数 + 驱动自报的
  `dropped_bad_tool_calls`**（SAN-6 引入的计数）。真的一条都没有时保留既有文案。
- **钉子用例**（+2）：走**派发漏斗**（不是注册表直接调，否则拿到的是另一句泛化文案、
  测不到模型实际看到的那句）断言缺参数/类型不符都是「工具参数错误」且
  `executed=false`；**对照**——脚本不存在仍是「动作脚本执行失败」且 `executed=true`
  （脚本可能跑到一半，那个警告必须留给模型）。另一条断言丢弃计数的合并报数。
- **反向验证**：把 `paramError` 标志去掉（恢复单一 catch）→ 前一条用例 3 个断言
  转红（实测文本回到「工具执行失败: 缺少字符串参数 script」）。

### 2026-09-22 中断语义与代码对齐（SAN-7，含一次自我推翻）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-7。

- **问题（文案与代码相反）**：三处旧文案写着「模型调用期间（最长 600s）**不可中断**，
  此时再按一次 Ctrl-C 即立即强杀」——`docs/agent.md`、`docs/reference.md` 与
  `src/main.cpp` 的处理器注释。而 P2a 取消哨兵（`6bffb3e`）落地后，**首次** Ctrl-C
  就能在约 1 秒内中止在飞的 libcurl 请求。`src/main.cpp` 里甚至同一文件两段注释
  互相矛盾（`:25-31` 讲哨兵、`:62-64` 说不可中断）。
  **后果不是"文档不准"这么轻**：那是一句会导致物理后果的操作建议——运维照它
  多按一次 Ctrl-C，就跳过 `deactivate()` 把执行器留在带电状态。
- **修法**：`docs/reference.md` 的信号一节重写为**唯一权威描述**（首次 SIGINT 的
  四步：置标志 → 建哨兵 → 窗口内钩子同样看标志 → 退出前走清理链；第二次是
  **逃生口而非常规操作**，并写明它的存在理由是"总能停下来"）。`docs/agent.md`
  只留一句指向它、不再复述数字（三处说法带具体数字各写一遍，正是漂移的成因）。
  `src/main.cpp` 的处理器注释同步改写，与 `:25-31` 的哨兵说明一致。
- **新增判据（E2E 场景 13）**：首次 SIGINT 之后断言退出码**恰好 1**——
  被信号按缺省处置打死是 130、段错误 139、早退 0，三者都不是 1，所以"恰好 1"
  证明走的是正常返回路径（`runRobotAndCleanup` → `deactivate` → `return 1`）；
  并断言取消哨兵不残留。**反向验证**：把处理器改成首次即强杀 →
  三条断言全红（`rc=130`、无"请求已取消"、哨兵残留）——它确实在区分优雅与强杀。
- **一次自我推翻（如实留痕）**：场景 13 的第一版设计是用
  `test/cpp/LifetimeProbe.lua` 的 `destroy` 标记证明"`deactivate()` 真的执行了"。
  反向验证当场推翻——把 `main.cpp` 里那行 `robot.deactivate()` 注释掉，
  marker **照样出现**：因为 `~RobotStruct` 会在对象销毁时补一次断电（这正是
  源码注释里写明的兜底）。所以 marker 只能证明"硬件最终被去初始化了"，
  **分不出**显式 `deactivate` 与析构兜底——拿它当"清理链跑过"的判据是**假绿**。
  判据因此改成退出码，并把这段经过写进场景注释与本文档：
  **能被反向验证推翻的设计才是被验证过的设计**，推翻过程本身也要留痕。

### 2026-09-22 驱动不再合成 tool_call id，并补齐响应形态判据（SAN-6）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-6。
> **行为变更**：协议驱动不再自己编 `call_<i>`，id 缺失时透传空串由宿主填。

- **问题①（id 合成遮蔽宿主方案）**：`parse_response` 里 `id = tc.id or ("call_" .. i)`
  ——宿主**故意**把合成方案设计成轮次相关的（`agentloop` 的 `call_<turn>_<i>`，
  注释明写"个别流式/兼容实现会漏 tool_call id"**就是为这种网关准备的**），
  但驱动先生成的 `call_1` 把那条路径**永远遮住**，而驱动的 `call_<i>` **每轮从 1 重来**。
  复现（同一份无 id 的响应体连跑两轮，再喂给仓内自带的协议校验器）：
  历史里 `tool_call_id=call_1` 出现两次 → `answered more than once`。
  **同一条消息内也会撞**：provider 给 `[{id:"call_2"}, {无 id}]` → 第二条合成出 `call_2`。
- **修法①**：驱动只**透传**（`type(tc.id)=="string" and tc.id or ""`），空串由宿主填。
  请求侧的必填判据不受影响——宿主在组包前一定已经填过 id
  （`agentloop.cpp` 的合成在写历史之前）。
- **问题②（PL-6 的未修兄弟）**：PL-6 只守了 `tool_calls` 的**容器**类型，
  而 `choices` / `message` / **元素** 仍会抛裸 Lua 错误。实测（修复前）：
  `{"choices":42}` → `attempt to index field 'choices'`；
  `{"choices":[{"message":42}]}` → `attempt to index local 'msg'`；
  `tool_calls:[42]` → `attempt to index local 'tc'`；
  `[{"function":42}]` → `attempt to index local 'fn'`。
  `M.perform` 调 `parse_response` **不在任何 pcall 里**，于是这些错误逃出驱动
  "返 ERR 表、绝不抛"的契约，宿主最终只看到一句 `attempt to index local 'tc'`
  ——不点名字段、不给报文片段，与 PL-6 修好之前**逐字同类**。
- **修法②**：`choices` / `choices[1]` / `message` 非表 → **点名返回**
  （`响应字段 X 形态非法（期望 对象，实际 number）`）；`tool_calls` 的**元素**非表、
  或 `function` 非表 → **跳过并计数**（`extras.dropped_bad_tool_calls`），
  与 PL-3 丢弃非 text 段同口径——丢弃但如实记账，不静默、也不让整轮失败。
  计数只在真丢了时才挂（保持既有响应形态不变）。
- **一处修，两个驱动受益**：DeepSeek 复用同一份 `parse_response`，故两组用例
  都在两个驱动上跑（`for _, lib in ipairs({"openai","deepseek"})`）。
- **守门**：Lua +28 条（形态点名 ×3 ×2 驱动、元素丢弃计数、合法/畸形混合、
  全合法时字段不出现、id 透传三种形态、arguments 非字符串 tostring）；
  C++ +1 条（漏 id 的网关下宿主合成的 id **跨轮唯一**，并用
  `historyProtocolViolation` 钉住整段历史可被服务端接受）。
- **覆盖边界（如实声明）**：那条 C++ 用例走的是 `AgentFakeDriver` 夹具（**不经**
  Lua wire 层），所以它钉的是**宿主的合成方案**；"驱动不再合成 `call_<i>`"这一半
  由 Lua 用例钉——反向验证：把驱动改回 `tc.id or ("call_" .. i)` →
  两条 Lua 断言转红（实测输出 `{"name":"t","id":"call_1",...}`）。

### 2026-09-22 动作执行回显改走 stderr（SAN-5），并修好两处会假绿的断言辅助

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-5。

- **问题（已实测复现）**：`ActionRunner::runAction` 调的是会回显的 `play()`，
  回显落在 `MtVars_Lua::callImpl` 的 `std::cout`（`----------` 分隔线 + 驱动结果表
  `{"result":"OK",...}`）。于是 `--task` 的 stdout 里混进这两样，而 `docs/agent.md`
  与 `src/main.cpp` 都写着「stdout 只放模型回答」——后果是 `--task` 不能安全地
  接管道/重定向（`mybot --task … | jq`、写文件、被脚本消费全部受影响）。
  E2E 此前全程 `2>&1` 合并两流，**结构上看不到**这件事。
- **修法**：`MtVars_Lua::callImpl` 的三处 `std::cout` 改 `std::cerr`——回显是
  **诊断输出**，它该在 stderr。**回显没有消失**（终端上两股都是屏幕），
  管道模式的观感不变；唯一差异是把 stdout 重定向到文件的人会发现那两行不在文件里。
  没有全局关掉回显：管道模式下那两行是**有用的进度输出**（动作边界）。
- **如实声明的残余**：Lua 驱动自己 `print()` 的进度行（`Relay.lua` 的
  `Relay: test=… -> delay=…`、`drv_common` 的 `VIRTUAL HARDWARE` 横幅）**仍然走
  stdout**——48 个在库驱动都是这个写法。因此 stdout 契约收窄为**"宿主不往里写东西"**，
  而不是"里面只有模型回答"；`docs/agent.md` 已按这个口径改写（原话过强）。
  要彻底干净得让驱动经统一的输出辅助打印，属独立一项（未做，见重开条件）。
- **顺带修好两处会假绿的断言辅助**（本项实现时实测踩到）：`scripts/agent_e2e.sh` 的
  `check_present` / `check_absent` 用 `grep -qF "$2"` 时**少了 `--`**——模式以 `-`
  开头（`"----------"` 正是）会被 grep 当选项串解析并静默返回非 0：
  `check_present` 于是把"找到了"报成"没找到"（本次实测：separator 明明在文件里、
  `head` 看得见、`grep -c` 得 1，函数却报 FAIL），而 `check_absent` 会**永远通过**
  （方向相反，同样是假绿）。两处都补上 `--`。
- **守门**：E2E 新增场景 12（stdout 纯净性，**不合并两流**）：断言
  `----------` 与 `"result":"OK"` **不在** stdout、`----------` **在** stderr、
  模型回答仍在 stdout。
- **反向验证**：把三处 `std::cerr` 改回 `std::cout` → 场景 12 的两条"不在 stdout"
  断言转红。

### 2026-09-22 预算降级不再把「没执行」说成「副作用已发生」（SAN-4，安全口径）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-4。
> **行为变更**：任务内文本预算的降级说明，不再覆盖三类「根本没执行」的工具结果。

- **问题（已实测复现）**：`TaskBudget::admit` 对**所有**工具结果一视同仁，超限时
  无条件追加「工具本身已执行完毕（副作用已发生）。」——而策略拒绝
  （`--agent-policy`）、未知工具、参数错误这三类结果**从未被派发**。
  实测（策略拒绝 `run_command` + 任务内文本预算 100 字节）：模型收到
  「…工具本身已执行完毕（副作用已发生）…」，而**同一轮**的审计日志写着
  `ok:false, error:"policy_denied: 确认程序拒绝（退出码 1）"`。两条通道给出相反
  的事实，模型可能据此认为命令已经跑过而不再重试。对物理机器人，谎报状态是
  安全相关的错误——本项目把「不向模型谎报状态」写进过多处设计说明（AG-4′、AG-21）。
- **修法**：`AgentToolResult` 增 `bool executed`（缺省 `true`）。三条"没派发"的
  路径显式置 `false`：策略拒绝、未知工具、参数错误；工具**内部**失败仍是 `true`
  ——脚本跑到一半报错时机器人可能停在半途，那种情况**必须**保留「已执行」口径
  （方向相反的谎报同样不可接受）。`admit(text, image, executed)` 在
  `executed=false` 时**不做降级**：这类文本本身 ≤200 字节，且它就是要给模型的
  事实（「没执行、为什么」），必须原样到达。
- **为什么不用 `ok` 代替**：`ok` 把"没执行"与"执行了但失败"混在一起，
  而这两件事对模型的意义完全相反——所以判据由产物显式给出，不靠解析文案
  （与 SAN-3 同一条纪律）。
- **记账不变**：`executed=false` 的结果**仍然计入额度**——否则"反复被拒"
  就成了往历史里无界灌文本的免费通道（AG-21 的同一口径）。
- **钉子用例**（+3）：未执行的调用不得出现"副作用已发生"且原裁决文本必须原样到达；
  执行过但失败的调用**必须**保留"已执行"口径（对照，防一刀切删掉那句话）；
  `executed=false` 仍然记账。**反向验证**：把 `executed` 传成常量 `true` →
  第一条用例 3 个断言转红（实测文本里出现「工具本身已执行完毕」）。

### 2026-09-22 `agent-all` 真的覆盖 agent 全表面了（SAN-2，守门假绿修复）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-2。
> **更正**：AG-23（2026-09-21）落地的那张分段-过滤器映射，其 Lua 那一格指向一个
> **从未存在**的文件模式——本文档 584 行附近那条记录的「Lua `test_agent*.lua`」口径
> 与实际不符，此处更正，历史条目保留不改。

- **问题（已实测）**：`test/run_all.sh` 的 `agent-all` 映射写着 `filter_lua="agent"`
  （注释称 `test_agent*.lua`），但该模式**从未存在**——`git log --all -- 'test/test_agent*.lua'`
  零命中，`ls test/ | grep -i agent` 无输出，`for t in test/test_*.lua` 的 `*agent*`
  匹配 0 个文件。于是「agent 全表面」入口的 **Lua 段静默跑了 0 个文件**，
  而 Lua 循环对"过滤器命中 0 个文件"是**合法状态**（不报错、不计入汇总）。
  实测汇总只有 **11 条**（9 个 E2E 场景 + 2 条加固负路径）+ 8 条 harness 自检。
- **真正的协议面在哪**：`test/test_llm_drivers.lua`（169 条）——它测两个内置协议
  驱动的报文整形与响应映射，正是 PL-3（数组形态 `content`）、PL-6（`tool_calls`
  形态非法）落过缺陷的那一层，也是 SAN-3 改动的三个文件所在的那一层。
  在旧入口下，改这三个文件跑 `agent-all` 会**全绿**：一条 Lua 用例都没执行。
- **修法**：`filter_lua` 由 `"agent"` 改为 `"llm"`（实测只命中 `test_llm_*.lua`，
  无副作用），并把自证从「过滤器变量非空 + e2e/cpp 认得这个输入」升级为
  **按 roster 核对真实文件**：
  - ① `agent_lua_roster` 里每个文件必须**存在**，且在本过滤器下**真的会被跑到**；
  - ② 反向——`test/test_llm_*.lua` 下每个文件都必须登记进 roster（漏登记即红）。
  这与 C++ 侧 `test_util.cpp` 的「非 agent 面 roster 带理由登记」是同一条纪律。
  旧自证检查的是**映射的形状**，新自证检查的是**被映射到的文件**。
- **口径说明**：`agent-all` 的汇总数从 11 → 12（**按文件计**，不是按断言计），
  但真正被跑到的断言多了 **169 条**——「汇总涨了 1」这个数字本身容易误读，
  故把两个口径都写在这里。
- **反向验证（两条都真跑过）**：① 把 `filter_lua` 改回 `"agent"` → 自证响亮失败
  （`rc=2`，点名"`test/test_llm_drivers.lua` 不在过滤器 'agent' 的命中范围内"）；
  ② 往 roster 里塞一个不存在的文件 → 同样 `rc=2`。
- **不在内**：`test_camera.lua`（`capture_image` 依赖的摄像头协议面）**未**纳入
  `agent-all`——它会连带 `test_camera_v4l2.lua`（真机用例）。它在 `docs/testing.md`
  里注明"改动摄像头驱动请单独跑"。

### 2026-09-22 重试真的认识真实错误了（SAN-3，含 429/5xx 与 Retry-After）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-3。
> **行为变更**：`max_retries` / `retry_backoff_ms` 从「事实上不生效」变成真的生效——
> 429/5xx 与真实 curl 连接错误现在会重试，且限流时按服务端给的 `Retry-After` 退避。

- **问题（已实测）**：重试判据是**匹配人话子串**（`CURLE_COULDNT_CONNECT` /
  `CURLE_OPERATION_TIMEDOUT` / `curl error` / `网络` 等六个），而生产路径产出的
  错误是 `curl rc=7: Could not connect to server`（`curl_easy_strerror` 返回英文
  散文）与 `HTTP 503: {...}`——**六个子串一个都不匹配**。全仓唯一产出匹配串的地方
  是测试夹具 `test/cpp/AgentFakeDriver.lua`，于是**这个重试循环只在夹具上跑得动**。
  实测：503 / 429 / 真实超时（rc=28）/ 连接被拒（rc=7）**各只发 1 次请求**；
  配置里写着 `max_retries: 3` 也照旧一次就放弃，且没有任何用例会发现。
- **顺带**：`Retry-After` 完全不可达——`llm_http.lua` 的 `ffi.cdef` 里没有任何头
  回调声明，响应头在 libcurl 里就被丢掉了。限流时"服务端说了等多久"这个最关键
  的信息丢失，本地指数退避（1s/2s/4s）与真实限流窗口毫无关系。
- **修法（结构化判据优先，字符串只作回落）**：
  - `llm_http.lua` 挂一个只认 `Retry-After` 一行的头回调（`OPT_HEADERFUNCTION`，
    编号进 `test_curl_consts.c` 与 `gen_const_check.cmake` 的常量自检），
    解析 delta-seconds 并**只在真解析出数字时**挂 `retry_after_ms`；
    失败路径补 `curl_rc`（数值退出码）。
  - `llm_wire_openai.lua` 的非 200 分支给出 `http_status` / `retriable` /
    `retry_after_ms`：**429 与 5xx 可重试，其余 4xx 不可**——两者此前长得一模一样，
    要么限流不重试，要么把 401 也重试（烧配额）。
  - 宿主新增 `classifyDriverError()`（纯函数、可离线穷举）：429 → `Throttled`，
    5xx 与连接类 curl 码（6/7/28/35/52/55/56）→ `Transient`，其余 → `No`；
    字段缺失时回落到字符串判据（旧驱动仍可用，且现在认得真实形态 `curl rc=`）。
  - 新增 `MatrixVars_Hardware::playQuietRaw`（缺省 = 调 `playQuiet` 并照常抛，
    只有 Lua 驱动覆写）：**ERR 时返回表而不是抛**，让判据能读到结构化字段。
    公共的 `ensureOk` / `playQuiet` 语义零变更。
  - 退避时长：`Throttled` 时取「服务端 Retry-After」与「本地指数退避」的**较大值**，
    日志如实点名来源（`退避来源: 服务端 Retry-After`）；Retry-After 钳到
    `[1s, 5min]`——坏网关回一个天文数字不能把 agent 挂死一天。
- **超时场景的口径调整**：E2E 场景 3（`--llm-timeout` 生效）原先靠"1 秒超时后
  4 秒内失败"判定，修复后缺省 `max_retries=3` 会让超时与重试叠加（>4s）而转红。
  该场景现在显式配 `max_retries: 1`：**它量的是单次超时是否被 curl 尊重，
  重试由场景 11 专门守**——两条判据各测各的，互不掩盖。
- **守门**：E2E 场景 11 两条——`429x2ok`（前两次 429 + `Retry-After: 1`，第三次
  正常回答）断言任务最终成功、日志点名 429、退避来源是服务端；`http-503` 断言
  重试用尽后**如实失败**（不谎报成功）。夹具新增 `429x2ok` / `http-503` 模式，
  并新增 `_error_status`（能产出带状态码与响应头的错误，与"400 请求错了"区分）。
  Lua 侧 +12 条、C++ 侧 +4 条（穷举判类与钳制）。
- **反向验证**：把非 200 分支改回旧形态（只有一句 message）→ 429 场景实测
  `rc=1`、重试 0 次、服务端只收到 1 个 POST（即修复前的行为）。

### 2026-09-22 单轮截断不再复用 `tool_call_id`（SAN-1，阻塞级缺陷修复）

> 依据 `.tmp/AGENT_NEXT_PLAN_20260922.md` 的 SAN-1。
> **行为变更**：单轮工具调用超上限时的截断说明，从「新增一条 tool 消息」改为
> 「追加进该轮 assistant 的文本」。这是一条**已文档化的钉子用例被改写**的变更，
> 理由见下（旧断言的期望本身是错的）。

- **问题（已复现）**：一轮里模型返回的调用数超过 `max_calls_per_round`（缺省 16）时，
  多余的被折叠成一条 `tool` 消息，挂在 **`calls.back().id`** 上——而那是**最后一条
  已派发**的调用，它两行之前已经拿到过自己的 `tool` 回传。于是历史里出现
  **两条同 `tool_call_id` 的 `tool` 消息**，服务端以 400
  `tool_call_id 'c2' answered more than once` 拒绝。
- **为什么是永久性的**：该轮派发**已经完成**，`runTurn` 的回滚边界（`complete`）
  已经越过那条坏消息——异常路径只裁「半截的尾部」，而这里没有半截。此后
  **每个新任务都重发同一段历史、拿同一个 400**，只有 `/clear` 或重启能恢复。
  这与 AG-1 记录过的失效形态同源：一条坏消息落进不可回滚的历史。
- **修法**：截断说明写进 `assistant.content`（该轮本来就带一句过渡语），
  历史**不再新增任何 `tool` 消息**。选它而不是「把说明追加到最后一个工具结果里」，
  是因为后者要给工具结果通道引入「可追加」的新语义（会波及 `TaskBudget` 的记账口径），
  而 `assistant.content` 是唯一既不违反协议、又不与 `tool_call_id` 一一对应绑定的位置。
- **顺带**：截断事件现在**写日志**（`[上限] 本轮 N 条…已丢弃 K 条未派发`）。
  此前这个安全相关的事实只到达模型，stderr 与审计轨迹里一个字都没有——
  这也正是本缺陷能长期潜伏的成因（另见 SAN-8）。
- **新增判据 `historyProtocolViolation()`**：把「assistant.tool_calls 与
  tool.tool_call_id 一一对应」从注释里的约定变成**可执行判据**，三条：
  ① 无孤儿 tool 消息；② **同一 id 只能被回答一次**（本缺陷的正判据）；
  ③ 无漏答。它与 `scripts/fake_llm_server.py` 的 `protocol_error` 是同一套判据的
  两份实现（C++ 侧供单测、Python 侧供 E2E）。
- **被改写的钉子用例**：`agentloop_caps_calls_per_round_and_tells_the_model`
  原先断言 `h[5].toolCallId == "c2"`（即把重复 id 当成期望行为），且该断言与
  同一个函数里「一一对上」的注释自相矛盾。现改为断言历史自洽 + 截断说明在
  assistant 文本里。**不是「用例跟着实现改」，是旧断言的期望本身是错的。**
- **守门**：新增 E2E 场景 10（`max_calls_per_round: 2` + 夹具一轮返回 3 条调用）：
  全程无 400、截断事件可见、**第二个任务照常完成**（会话没被永久污染）。
  夹具 `scripts/fake_llm_server.py` 新增 `manycalls[:N]` 模式——此前所有场景每轮
  只返回 1 条调用，截断分支在守门里从未执行，这是缺陷能潜伏的第二个成因。

### 2026-09-22 动作脚本执行预算（PL-2，原 AG-12 子项）

> 依据 `.tmp/IMPLEMENTATION_PLAN_20260922.md` 批 C。
> **行为变更**：动作脚本此前可以无限期执行；现在有缺省 180 秒的预算（可配置、可关闭）。

- **问题**：`run_action_script` 是本工具集里**唯一没有任何上界**的执行路径，而它
  **默认开启**（有超时的 `run_command` 反而默认关闭）。动作脚本本身是线性矩阵、
  DSL 无循环（只有 `@Action`/`@Vars`/`@Include`/`@Alias`），所以挂死只可能来自
  Lua 驱动自己的 `call()`：死循环，或阻塞在 ffi 系统调用里。一旦发生，会话永久卡住；
  按行派发的 `stopRequested()` 检查只在**行与行之间**生效，一行内部的挂死不响应 Ctrl-C。
- **机制**：窗口内给每个 Lua 驱动状态挂**指令计数钩子**并**关掉 JIT**，超期即以
  Lua 错误中止本轮脚本。错误经既有管道（`call_lua` 的 `pcall` → ERR 契约 →
  `ensureOk` 抛 → `run_action_script` 的 catch）变成给模型的失败文案——不需要新错误通道。
  同时把 `stopRequested()` 纳入判据，**Ctrl-C 在窗口内也能中断失控驱动**（此前只能
  连按两次升级为强杀，而强杀会跳过 `deactivate`、把执行器留在带电状态）。
- **「必须先关 JIT」是实测结论，不是实现细节**：LuaJIT 的 count 钩子只在解释器派发
  路径上触发；热点循环约 56 次迭代后就被编译成 trace，此后钩子永不触发。实测
  `while true do end` 在 JIT 开、`N=100000` 下钩子触发 **0 次**（原方案的 N=100000
  等于死配置）。**本仓库内的反向验证**：把 `jit.off` 那行去掉 →
  `agenttools_action_script_budget_aborts_runaway_driver` 直接挂死到 30 秒 alarm
  被杀（signal=14）；加回去则毫秒级中止。
- **代价（实测，真实驱动 `modules/Relay.lua`）**：窗口内解释执行约 3.4× 慢，绝对值
  0.11 → 0.44 µs/次驱动调用；最重的在库动作脚本约 550 次调用 → 窗口总代价 ~0.2 ms，
  而脚本自身耗时以秒计（硬件延时主导）。且 10²–10³ 次调用量级下 JIT 根本没热身
  （1000 次以下 `jit=on` 反而更慢），实际无可损失。窗口只覆盖一次 `run()`，
  **窗口外一切照旧**——`test_jit_warmup.lua` 锁的是 JIT 编译后的 HAL 变参 ABI，
  它不经过 agent 工具，不受影响。
- **边界（已写进 `docs/agent.md` 已知限制）**：钩子由字节码派发驱动，只能打断
  **解释器里跑的 Lua 代码**。驱动阻塞在 `read()`/`ioctl()`/`usleep()` 里时 VM 不派发
  指令，钩子没有执行机会——**这一条无解**。本仓库唯一能兜住阻塞 C 调用的是子进程 +
  deadline + SIGKILL（`agent_subprocess`），但动作脚本必须在宿主进程内操作已上电的
  硬件状态，走不了那条路。
- 配置：`action_script_timeout_sec`（**配置文件**，0-3600，缺省 180，0 = 显式关闭）。
  不加 CLI 选项，沿用 B4 口径；缺省 180 与单次模型调用缺省取齐（同为「agent 面向的
  单次阻塞操作」上界）。0 走 optional 的 has_value 语义——写成 `if (*opt)` 会让
  「配成 0」静默退回缺省。
- 钉子用例：`agenttools_action_script_budget_aborts_runaway_driver`（死循环驱动被
  预算中止且点名，夹具 `test/cpp/InfiniteLoopDriver.lua`）、
  `agenttools_action_script_budget_off_really_hangs`（**反向对照**：关闭预算后同一
  脚本确实挂死到 alarm——证明上一条测的不是别的东西）、
  `agenttools_action_script_budget_window_is_scoped`（工具返回后预算指针已清、
  JIT 与钩子已复原、同一进程内可继续正常执行）、
  `agentconfig_action_script_timeout_round_trips`（0 穿过「未配置」判定 / 缺省 180 /
  越界与负值拒绝）、`luaexec_arm_disarm_restores_jit_engine`（**窗口退出必须把 JIT
  装回去**——`test_jit_warmup.lua` 锁的是 JIT 编译后的 ABI，它需要真机 I2C、本机跳过，
  所以那条前提在这里直接钉住；用例首版因为 `jit.status()` 返回 4 个值、只读栈顶而
  恒为真，括号截断后才真正生效）。
- 验收：C++ 单测 **503 通过 / 0 失败**（498 → 503）；agent 作用域 182/0。
- 顺带修正文档：`docs/drivers.md` 的驱动契约表补一句「`data.mode` 是宿主注入的保留
  字段，结构脚本不要用它当数据列名」——本次开发踩到过（宿主在 build 期
  `metadata.put("mode", "build")` 会覆盖同名列，症状是驱动永远读到 `"build"`）。

### 2026-09-22 网关数组形态 content 归一化（PL-3）

> 依据 `.tmp/IMPLEMENTATION_PLAN_20260922.md` 批 B。
> 只影响**数组形态 content** 的响应；字符串/null 形态一字不变。

- 新版 OpenAI 协议允许 `content` 为 `[{type="text",text=…},{type="image_url",…}]`
  数组形态，部分兼容网关也这么返回。宿主只认字符串（`src/agentproto.cpp` 的
  `content->isStr()`），原样透传 → 文本为空、`tool_calls` 也为空 → 整轮以
  「既没有返回文本也没有工具调用」告终，**报错完全不指向真因**。
- 现由驱动在 `parse_response` 归一化：text 段按序拼接；**非 text 段丢弃但计数**
  记进 `extras.content_parts_dropped`——「模型给了别的内容而宿主没看见」这件事
  必须在审计里可见（项目口径：不谎报状态）。顺带处理两种网关变体：直接回单个
  part 对象（非数组）、以及畸形表（后者归一化为空文本 + 计数可见，不静默吞掉）。
- **单点收益**：DeepSeek 驱动复用同一份 `parse_response`，一处修复覆盖两个驱动。
- 钉子用例：`test/test_llm_drivers.lua` 新增 18 条（两个驱动 × 拼接顺序 / 非 text
  段计数 / 空数组 / 单个 part 对象 / 畸形表 / 字符串不变 / null），139 → 157 通过。
- 反向验证：短路归一化（直接用 `msg.content`）后 12 条转红，恢复后全绿。
- 验收：`luajit test/test_llm_drivers.lua` **157 通过 / 0 失败**。
- 文档：`docs/agent.md` 驱动契约节补一句「`content` 对宿主永远是字符串」。

### 2026-09-22 `--check-config` 配置校验模式（PL-1，原 U-4）

> 依据 `.tmp/IMPLEMENTATION_PLAN_20260922.md` 批 B。
> 排障体验改进：以前配置文件写错要**真起一次 agent 会话**才报错退出 2——接了硬件还得反复上下电。

- **`--check-config`**（agent 专属）：`buildAgentConfig` 校验通过后打印一份配置摘要
  并以 **0** 退出，**不装载结构脚本、不上电、不发起任何网络请求**。因此它连
  `--robot` 指向的脚本都只读「脚本头 16 KiB」（三段体积预检本来就要读的那段），
  不走 SQL 解析与 `active()`——E2E 用「结构脚本可读但不是合法 SQL」对照钉住了这一点：
  同一脚本 `--check-config` 得 0、真跑必然非 0。
- 摘要内容都是操作者排障真正要看的：配置文件路径 / 协议驱动与变量 / 模型 / **接入点**
  （缺省时明写「未配置，首次调用将失败」——B2 起不再有内置缺省，这句话不联网也能摆出来）/
  采样与超时 / 轮次上限 / API key **来源**（只报来源，值绝不出现）/ 结构脚本 / 脚本目录 /
  提示词包与三段预检字节数 / 历史与图片预算 / 摄像头 / `run_command` 开关 / 审计与策略落点。
- **不另立判据**：摘要只渲染、不校验——判据单点仍在 `buildAgentConfig`（失败时调用方
  已 `return 2`），再造一套只会漂移。落点是 `agent::summarizeAgentConfig()` 纯函数，
  放在可链接的 TU 里（`main.cpp` 不参与 `mybot_tests` 链接，写进 `main` 就没法单测）。
- 失败路径一律 rc=2：非 agent 模式（agent 专属选项那条）、漏 `--robot`（否则会打印
  帮助退出 0，CI 会读成「配置没问题」）、配置本身不合法。摘要会**打开但不写入**
  审计日志文件，顺带验证其可写。
- 钉子用例：`cliargs_check_config_is_rejected_outside_agent_mode`（三条 CLI 路径）、
  `agentconfig_summary_reports_config_without_leaking_key`（含「摘要不得出现密钥值」断言）、
  `agentconfig_summary_flags_missing_endpoint`；`test/run_all.sh` 新增 4 条 E2E
  （正向摘要 / 不联网 / 只读脚本头 / 拼错字段 rc=2）。
- 反向验证：摘掉 `main` 里的短路分支后，E2E 的正向两条转红（rc 由 0 变 1）。
- 验收：C++ 单测 **498 通过 / 0 失败**（495 → 498）；`bash test/run_all.sh` 全绿。
- 文档：`docs/reference.md` 与 `docs/agent.md` 选项表各加一行（帮助计数黄金测试
  28 → 29，且「帮助 ↔ docs 双向一致」守卫会强制两处同步）。

### 2026-09-22 两处名不副实的测试钉子修正（PL-4）

> 依据 `.tmp/IMPLEMENTATION_PLAN_20260922.md` 批 A。
> 无行为变更（等价重构 + 补钉子），改的是「用例看起来在守、其实没守」的两处。

- **`--help` 优先于校验：以前没有真的守门。** 用例
  `cliargs_help_precedes_validation_in_main` 名字写着 `in_main`，函数体却只调
  `parseArgs`——而 `main.cpp` 不在 `MYBOT_CORE_SOURCES`、链接不进 `mybot_tests`，
  于是把 `main` 里的 help 检查挪到校验之后，用例照样绿。现在把这条**顺序判定**
  抽成可链接的 `cliargs::preflightCli(ParsedArgs&) -> Preflight{Proceed,Help,ConfigError}`
  （判定与打印分离：帮助文本仍由 `main` 打印，单测里不刷屏），`main` 只按返回值分发；
  用例改为断言三种输入下的判定结果。
- **2 MiB 图片上限以前没有字面量钉子。** 两处超限用例的夹具尺寸都由**被测常量**
  `kMaxImageBase64Bytes` 推导（`kMaxImageBase64Bytes / 4 * 3 + 3`），常量被改错时
  夹具跟着走、用例仍然绿——它测的是「越过自己」，不是「越过 2 MiB」。新增
  `agenttools_image_cap_boundary_is_pinned_by_literal`：编译期 `static_assert`
  钉住常量字面等于 2 MiB（改了就编译不过，逼改动者面对 `docs/agent.md` 与工具描述），
  运行期再用字面量算出的边界验证「恰好 1.5 MiB 原始字节放行 / 多 3 字节即拒」。
- 反向验证：① 把 `preflightCli` 里的 help 判定挪到校验之后 → 对应用例转红；
  ② 把 `kMaxImageBase64Bytes` 改成 1 MiB → `static_assert` 编译期失败并打印指引文案。
- 验收：C++ 单测 **495 通过 / 0 失败**（494 → 495）。

### 2026-09-22 文档/注释残留四处修正 + 工具 schema 深度加锚点（PL-5）

> 依据 `.tmp/IMPLEMENTATION_PLAN_20260922.md` 批 A。
> 纯文档/注释与一条新增钉子用例，无行为变更。

- **`modules/LlmOpenAI.lua` 头注释自相矛盾**：一处把 `api_key` 列进「宿主传进 `call()`
  的参数表」，另一处又说「宿主不把密钥写进参数表」。以事实为准（`src/agentmode.cpp`
  的 `makeDriverConfig` 确无该字段），并顺手把**非空时才挂**的 `endpoint` /
  `reasoning_effort` / `thinking` 三个可选字段写进注释。
- **`modules/lib/pthread.lua` 回退基准停留在 TD-3 之前**：注释里的槽布局仍是
  `values[32]` / `SLOT_SIZE=336`，与 `src/daemonvar.hpp`（TD-3 后 592B / `values[64]`）
  差了一倍。回退基准是「宿主未注入布局时」的比对依据，数字错了就失去意义。
  同时修正 `src/daemonvar.hpp` 里 `static_assert` 的消息文本（仍写 "336 bytes"）。
- **非 agent 模式的拒绝文案漏列 camera 四项**：用户敲 `--no-camera` 却看到一份
  不含该选项的选项名单，像是撞上了另一个错误。文案现与 `docs/agent.md` 的
  agent 专属选项表逐项对齐。
- **「工具 schema 最深处为 6 层」此前无锚点**：这句话是 `docs/agent.md` 已知限制里
  人工统计的数字，给参数加一层嵌套不会有人发现，与 `kMaxLuaNesting`（8）的余量会被
  静默吃光。新增钉子用例
  `agenttools_request_nesting_depth_is_pinned_against_lua_limit`：从驱动请求根起数
  容器层数并断言恰为 6、且显式断言余量为 2；文档同步写明口径与用例名。
- 反向验证：临时给 `toolSchema` 注入一层多余嵌套，该用例转红（2 条断言失败），
  恢复后绿。
- 验收：C++ 单测 **494 通过 / 0 失败**（493 → 494）；`test_llm_drivers.lua` 139/0、
  `test_pthread.lua` 14/0、`test_slot_addr.lua` 16/0 均不受影响。

### 2026-09-22 响应整形：tool_calls 形态非法时报点名原因（PL-6）

> 依据 `.tmp/IMPLEMENTATION_PLAN_20260922.md` 批 A。
> 只影响**畸形响应**的报错质量，正常报文一字不变。

- 旧行为：`parse_response` 直接 `ipairs(msg.tool_calls)`。网关若把 `tool_calls`
  返回成字符串/数字/布尔，抛的是 `bad argument #1 to 'ipairs'`——经 `lua_pcall`
  折成 ERR 后只剩一句 Lua 内部报错，既不点名字段也不给报文片段，与同一函数
  「响应缺少 choices[1].message」的诚实口径不一致，排障时看不出是哪个字段坏了。
- 现行为：非表即 `return nil, "响应字段 tool_calls 形态非法（期望数组，实际 <type>）"`；
  调用点 `M.perform` 既有的 `nil+原因 → ERR + 报文片段` 路径原样复用（未改）。
- **单点收益**：DeepSeek 驱动（`modules/lib/llm_wire_deepseek.lua:85`）复用同一份
  `parse_response`，一处修复同时覆盖两个协议驱动。
- 钉子用例：`test/test_llm_drivers.lua` 新增 14 条——两个驱动 × 三种非法形态，
  每条都断言「不抛裸错误」**且**「返回点名原因」，另加一条合法数组的对照用例
  （确认新判据没有把正常路径一起挡掉）。
- 反向验证：去掉判据后 12 条转红（失败详情正是旧的 `bad argument #1 to 'ipairs'`），
  恢复后全绿。
- 验收：`luajit test/test_llm_drivers.lua` **139 通过 / 0 失败**（原 125 条零修改）。

### 2026-09-22 CLI 超时/上限参数化（F-1/F-2/F-3，原 TD-2 剩余部分）

> 依据 `.tmp/NEXT_STEPS_20260922.md`（核实两轮 agent 重构
> 全部落地后梳理出的真实剩余项）。三个此前硬编码的运行期数值改为可选 CLI 参数，
> 行为保持型：不传 = 现状缺省值，零变更。

- **`--tcp-max-auth-attempts <N>`**（F-1）：TCP 口令连续失败上限，原写死 5
  （`kTcpAuthMaxFails`），现可调（区间 1-1000，`util::tcp_limits`）。达到上限后
  服务端放弃等待并退出，语义不变，仅阈值可配。
- **`--tcp-total-timeout <sec>`**（F-3）：TCP 连接总时长预算下限，原写死 600
  （`tcpTotalBudgetSec` 内），现可调（区间 1-86400）。生效值仍是
  `max(该下限, --tcp-idle × 20)`——「总预算」的放大语义不变，只是下限可配。
- **`--daemon-grace-ms <ms>`**（F-2）：`RobotStruct::deactivate()` 二段屏障（守护
  线程弃管后的额外等待窗口）宽限上限，原写死 30000（`kMaxGraceMs`），现可调
  （区间 0-300000）。全局选项，不限于 `--agent`/`--tcp`/`--input` 任一模式——
  任何结构脚本声明了 `@Daemon` 都可能触发该屏障；在 `robot.active()` 之前完成
  解析与校验（配置错误不碰真机）。

三者都不属于 agent 配置面（daemon/TCP 是全局能力），故不进 `mybot.llm.json`，
走普通 CLI 选项；区间常量单点（`util.hpp` 的 `tcp_limits`/`daemon_limits`
命名空间），与 `llm_limits` 同一惯例，`--help`/校验共用避免判据漂移。

- 钉子用例：`cliargs_parses_tcp_auth_and_total_timeout_and_daemon_grace`（解析层）、
  `cliargs_new_timeout_options_missing_value_is_rejected`（缺值拒绝）、
  `open_action_source_rejects_bad_tcp_max_auth_attempts` /
  `open_action_source_rejects_bad_tcp_total_timeout`（区间校验）、
  `robot_daemon_grace_ms_setter_affects_deactivate_barrier`（行为验证：小宽限值
  下二段屏障确实提前完成，stderr 点名新值而非旧缺省）。`scripts/tcp_adversarial.sh`
  新增场景 8（`auth_limit_custom`）：`--tcp-max-auth-attempts 2` 下 2 次失败即放弃
  （不是缺省的 5 次），端到端证明选项真的接管了判定，而不仅仅是解析层收了值。
- 反向验证：三处分别临时改回硬编码值，对应新用例/E2E 场景转红，回滚后绿。
- 验收：C++ 全量 493/0（新增 5 例）；`bash test/run_all.sh` 261/0/1 跳
  （`test_jit_warmup.lua`，既有已知跳过，与本次改动无关）；ASan/UBSan 产物下
  `mybot_tests` 493/0 无泄漏报告；`scripts/tcp_adversarial.sh` 8 场景全绿。
- 文档：`--help`（含分段 `--help tcp`）、`docs/reference.md`（CLI 参数表 + TCP
  安全设计段落）同步；帮助选项计数黄金测试 25→28。

### 2026-09-22 agent 重构阶段 C6 + C7（提示词素材单次读取 / F6 三条小路径）

> 依据 `docs/proposals/agent-refactor-plan-2.md` 阶段 C6（F5）与 C7（F6）。行为保持型 + 死分支清理。

**C6：预检读到的两段成品被系统提示词复用。** 原先「不依赖硬件的三段」体积预检读一次
结构脚本头与动作脚本清单就丢弃，拼提示词时再读一次：每会话多一次全树遍历 + N 次文件读，
且两次读之间文件若变动会让「预检口径」与「提示词口径」分叉（预检按旧内容放行、提示词
按新内容生成）。

- `AgentModeConfig` 新增 `structureHead` / `scriptsList` 两个 `std::optional<std::string>`：
  预检处**读一次存两份**（字节数与原先逐字一致），`buildAgentSystemPrompt` 优先复用；
  无值（离线单测手搓配置、不走 buildAgentConfig 的调用方）回退实时读取——读法与消毒是
  同一个函数，差别只在「什么时候读」。
- 用 `optional` 而非空串：空串是**合法的读取结果**（结构脚本不可读、脚本目录为空），
  拿它表示「没缓存」会把已读到的空结果误判成未命中再读一遍。
- 缓存只覆盖**会话开场的系统提示词**：REPL 的 `/scripts` 与工具的 `list_action_scripts`
  仍实时遍历——它们是「现在有哪些脚本」的按需查询，本该看到最新状态。
- 钉子用例 `agentconfig_prompt_reuses_precheck_reads`：预检之后把结构脚本与脚本目录改掉，
  提示词仍按预检时的内容生成（复用真的发生）；手搓配置走实时读取（回退通道）。反向验证：
  改回实时读 → 该用例转红（2 条断言），回滚后绿。

**C7-1：`escapeLogMarkers` 挪到可测位置。** 原先在 `agentloop.cpp` 的匿名命名空间，只能经
verbose 日志间接覆盖——下游解析器真正关心的是「行首标记」这条语义。现挪到 `util.hpp`
（inline，与 `sanitizeUtf8` / `echoSource` 同一处的文本处理单点），新增直接用例
`utils_escape_log_markers_only_touches_line_start_brackets`（行首转义含多行、非行首的
`[` 原样保留、空串不变）。**顺带证实 AG-22 守门有效**：第一版用例名前缀写成 `util_`（未
表态）被派生式比对当场拦下，改名 `utils_` 复用既有 roster 条目。

**C7-2：`call_capped` 死分支删除。** `excess > 0` 只在 `maxCallsPerRound > 0` 分支产生，
那里已把 `calls` 截到 ≥1 条，`calls.empty()` 永不可达。删掉兜底的同时补上这条**可达路径**
的首个用例 `agentloop_caps_calls_per_round_and_tells_the_model`：第 3 条调用不派发（工具
执行计数为 2）、说明文本如实点数、说明挂在**最后一条已派发**调用的 id 上、发给服务端的
历史里 assistant.tool_calls 与 tool 回传仍一一对上。反向验证：去掉 `resize` + 把说明 id
写死成 `call_capped` → 9 条断言转红，回滚后绿。

**C7-3**（确认程序 EPIPE 路径用例）已在 A2 落地，见该条。

- 验收：C++ 全量 488/0（`agentconfig_prompt_` 5、`agentloop_` 27、`utils_` 10）。
  存量用例零修改。
- 端到端（阶段 C 收口）：`bash scripts/agent_e2e.sh` 在 Release 与 ASan/UBSan 产物
  （`cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMYBOT_SANITIZE=address,undefined`）
  下各跑一次全通过、sanitizer 无报告；`./build-asan/mybot_tests`（带
  `LSAN_OPTIONS=suppressions=asan.supp`）488/0 同样无报告。
  `bash test/run_all.sh agent-all` 在本机仍不可跑（无 luajit 解释器，脚本入口即 `exit 1`）
  ——本轮改动不触 Lua 面，C++ 段与 agent E2E 是这台机器上能拿到的全部证据，不冒充全绿。
- 文档：本文件；`docs/agent.md` / `docs/reference.md` 无字段或行为变化需改。

### 2026-09-22 agent 重构阶段 C5（重试日志可注入）

> 依据 `docs/proposals/agent-refactor-plan-2.md` 阶段 C5（对应审计 F4）。行为保持型：缺省仍是 stderr。

传输层（`agentdriver.cpp`）的重试日志原先硬编码 `std::cerr`——离线用例抓不到它，宿主也没得选。

- `AgentDriverConfig` 新增 `std::ostream* log`（空 = `std::cerr`，即改动前的行为）；
  `main` 的装配点显式写 `dcfg.log = &std::cerr`，与 `runAgentMode` 的日志流是同一条。
- 重试信息（第几次尝试、退避时长、驱动给的成因）经该 sink 输出；**stdout 只放模型回答**。
- **不改审计 JSONL 的 schema**（不加 `retries` 字段）：审计 append-only 且有外部消费方，
  字段演进走新提案（方案「不做」节已声明）。
- 钉子用例：`agentdriver_retry_goes_to_injected_log_sink`（夹具 `AgentFakeDriver` 增
  `netfail-once` / `netfail-always` 两种 model）——首次网络类失败被重试、日志进注入 sink；
  同时钉住 `max_retries = 0` 仍**发一次**（抛驱动给的成因、无重试日志），而不是旧循环的
  「一次都不发落到 `logic_error`」。反向验证：去掉 `std::max(1, ...)` 钳制 + 日志改回硬编码
  `std::cerr` 两处注入 → 该用例转红，回滚后 `agentdriver_` 7/0。
- 验收：C++ 全量 485/0（`agentdriver_` 7）。
- 文档：`docs/agent.md` 补一句「重试写日志流（stderr），stdout 只放模型回答」。

### 2026-09-22 agent 重构阶段 B4（五个运行期数值配置文件化）

> 依据 `docs/proposals/agent-refactor-plan-2.md` 阶段 B4（对应审计 F3）。新能力，缺省行为不变。

原先只写死在三个下游结构体里的五个数值，现可由**配置文件**覆盖（仅配置文件——
不加 CLI 选项：选项面已 20+，且它们都是「一次定好、长期不变」的部署参数）：

| 字段 | 区间 | 缺省 | 去向 |
| --- | --- | --- | --- |
| `max_calls_per_round` | 1-1024（**拒绝 0**） | 16 | `AgentOptions` |
| `max_retries` | 0-6 | 3 | `AgentDriverConfig` |
| `retry_backoff_ms` | 0-10000 | 1000 | `AgentDriverConfig` |
| `run_command_timeout_sec` | 1-600 | 10 | `AgentToolContext` |
| `run_command_max_timeout_sec` | 1-3600 且 ≥ 上一个 | 60 | `AgentToolContext` |

- 链路：`llmconfig`（严格解析 + 区间单点进 `llm_limits`）→ `AgentModeConfig` →
  `maxCallsPerRound` 进 `AgentOptions`、重试两参数经 `makeDriverConfig` 进
  `AgentDriverConfig`、两个超时进 `AgentToolContext`。
- **`max_calls_per_round` 拒绝 0**：0 在下游是「一轮不限量下发动作」的旧语义，对物理
  机器人没有正当用法（与 `allow_shell` 同源的「能力只能命令行开、配置只能收紧」口径）。
- **`max_retries: 0` 的语义修对了**：该字段是「含首次的尝试次数」，旧循环直接拿它当
  上界——放宽到 0 会变成**一次都不发**并落到末尾的 `logic_error`（「不重试」反而
  什么都不做）。现在驱动侧钳到 ≥1：0 = 只发一次不重试。
- **退避单次封顶 60s**：缺省区间够不到，但放宽 `max_retries`/`retry_backoff_ms` 后
  `backoff << attempt` 会长到小时级——对「重启一下网关就好」的抖动，等待比失败更糟。
- **跨字段关系按生效值判**：只配 `run_command_max_timeout_sec` 而它低于缺省超时
  （例如 5 < 10）会被拒绝加载——否则模型不传 `timeout_seconds` 时按缺省请求 10s 会
  越过操作者声明的 5s 上限。`run_command` 的工具描述里那组「默认/最大」也改为读
  生效值（原先写死「默认 10，最大 60」，配置一改就对模型说谎）。

- 钉子用例：`llmconfig_runtime_fields_parse_and_reject`（合法值 + 10 组越界/类型不符 +
  拼错仍按未知字段拒）、`agentconfig_runtime_fields_reach_config_and_driver`（合流与
  驱动搬运点 + 未配置回缺省）、`agentconfig_run_command_timeout_cap_below_default_is_rejected`、
  `agenttools_run_command_schema_reports_effective_timeouts`。反向验证：注入
  `kMaxCallsPerRoundMin=0` / `maxRetries>0` 判定 / 关掉跨字段校验 / 描述写死四处破坏，
  对应用例全部转红，已回滚。

- 验收：C++ 484/0（`llmconfig_` 16、`agentconfig_` 17、`agenttools_` 40，其余作用域不变）。
- 文档：`docs/agent.md`（字段表 + 已知限制）、`mybot.llm.json.example` 同步；
  `docs/reference.md` 的配置文件一节只指向 agent.md 的字段表，无字段清单需改。

### 2026-09-22 agent 重构阶段 A1 + A2（子进程骨架单点化 / 确认程序一致性补齐）

> 依据 `docs/proposals/agent-refactor-plan-2.md` 阶段 A1/A2（对应审计 F1/F2）。一处真缺陷修复 + 行为保持型重构。

**F1（真缺陷，已修）**：`run_command`（`agent_subprocess.cpp`）与 `--agent-policy` 确认程序
（`agentpolicy.cpp`）各写了一份「fork + exec + 超时 + 进程组终止」，且已漂移——有界回收、
子进程 fd 吊销只落在 `run_command` 一侧。确认路径超时后打的是**无超时** `waitpid`
（还被 `retryEintr` 包着，连 Ctrl-C 都打断不了），SIGKILL 未生效（D 态 / 权限）时
**agent 会话永久挂死**。

- **A1（骨架单点化）**：`agent_subprocess` 新增 `runChild(ChildSpec)`，差异全部经选项表达
  （envp 清洗/继承、stdin payload 或 /dev/null、stdout 捕获截断或透传、超时/宽限、
  fd 吊销、是否响应 REPL 中断）。`runShell` 与确认程序都改为薄封装；
  `runShell` 侧行为逐字不变（存量用例零修改全绿），确认路径随之获得**有界回收**
  （WNOHANG 轮询到 2s 宽限，超时放弃并响亮告警 pid/宽限/留僵尸后果）与 fd 吊销。
- **A2（确认程序一致性的显式决策，不是顺手统一）**：确认程序的**环境保持继承缺省**
  ——它是操作者自己写的可信程序，合法的审批/通知脚本可能依赖自有 `…_TOKEN` 类变量，
  套用子进程密钥清洗会**静默砸掉**它们。钉子用例断言确认程序能看见 `BOT4CJ_LLM_KEY`；
  决策理由同时写进 `agentpolicy.cpp` 注释与 [docs/agent.md](agent.md) 的策略钩子节。
- **顺带修掉的真缺陷（EPIPE 待决信号）**：宿主往确认程序 stdin 写 payload 时对方已退出
  不读（EPIPE），内核同时产生一条 SIGPIPE——`pthread_sigmask` 屏蔽只是让它**待决**，
  解除掩码那一刻按默认处置**杀掉整个 agent 进程**（实测把整个测试进程以 rc=141 带走）。
  修复：恢复掩码前用 `sigtimedwait` 零超时消费掉「自己制造的那条」，宿主原有的待决
  SIGPIPE 不动。
- 内部改名与文案：`execShellChild` → `execChild`（它已不是 shell 专用），`runShell` 罕见的
  setup 失败文案带上 errno，`docs/testing.md` 的引用同步；不改审计 JSONL schema。

- 钉子用例：`agenttools_reap_with_grace_collects_exited_child`（宽限内回收）、
  `agenttools_reap_with_grace_returns_immediately_on_echild`（无子可收立即返回，不空等）、
  `agentpolicy_confirm_timeout_returns_promptly`（超时后及时返回）、
  `agentpolicy_confirm_env_is_inherited_for_trusted_program`、
  `agentpolicy_confirm_does_not_inherit_parent_fds`、
  `agentpolicy_confirm_survives_epipe_from_child_ignoring_stdin`。
  反向验证：注入 `cleanEnv=true` / `revokeFds=false` / 忽略 ECHILD 三处破坏，对应用例
  全部转红，已回滚。

- 验收：C++ 480/0（`agenttools_` 39/0、`agentpolicy_` 11/0）。本轮改动不触 Lua 面；
  `bash test/run_all.sh agent-all` 需要 LuaJIT，在无解释器的机器上只做 C++ 段验证。

### 2026-09-22 agent 重构阶段 B3（图片传输层降副本）

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 B3（对应 R4）。验收判据为 `VmHWM` 实测（`.tmp/memaudit/b3_measure.sh`，AG-19 方法；`.tmp` 不入库）。

**实测**（拍照场景，同一脚本前后对照）：

| 场景 | 改动前 | 改动后 |
| --- | --- | --- |
| 786KB 图 | ~40 MiB（方案口径） | **24 MB** |
| 1.2MB 图 | 49 MB | **28.5 MB** |
| 小图底噪 | — | 14.5 MB |

**三处修复**：

1. **游标过边界**（宿主侧）：`messageToJson(imageRefs)` 把消息里的图片换成
   `{"__bot_img": N}` 游标——字节本体不再进请求 Json，`callImpl` 的
   `jsonMergeInto` 参数合并与 `push_lua_value` 压栈这两份全量拷贝随之消失。
   传输层（`agentdriver.cpp`）在 `playQuiet` 前后 RAII 挂/摘游标表；
   `MtVars_Lua` 经受保护帧注册全局 `__bot_img(N)`（build 期急切 + setHostImages
   惰性，幂等），窗口外/越界/非数字游标一律 fail-closed 报错。
2. **wire 组包期落值**（驱动侧）：`llm_wire_openai.lua` 的 `to_wire_message` 遇
   游标表即经 `__bot_img(N)` 取字节——这是 payload 从宿主进 Lua 的唯一一次
   拷贝，随后 `json.encode` 拼进请求体（最后一次拷贝），POST 用指针零拷贝。
   通道未注册/取值报错/非字符串结果都响亮失败，不发半截报文。
3. **两个隐性的量级放大点**（Lua 侧）：
   `Camera.lua` 的 base64 曾把 ~2N 个单字符字符串塞进一张 Lua 表——1.2MB 图
   就是 ~160 万表项，LuaJIT TValue 数组瞬态 ~25MB（改动前后差值的主力）；
   改为 ffi 预分配字节缓冲一次性出串（瞬态 2×）。`lib/json.lua` 的
   `encode_string` 曾对无需转义的串也 `gsub` 整串拷贝；base64 是 JSON 安全
   字符集，加 `find` 快路径（零分配扫描）跳过。

同 payload 的全程副本从 ~7 份降到 3 份（存储落值 + 宿主→Lua 落值 + 请求体组装）。

**钉子用例**：`agentproto_image_cursor_mode_places_placeholder`（游标占位/引用登记/
字节不进请求）、`luavar_image_cursor_channel_window_bounds_and_failclosed`（窗口/
边界/摘表，真实临时驱动夹具）、`test_llm_drivers.lua` B3 组（落值/通道缺失/
取值报错/非字符串/字符串 url 透传）；agent_e2e 场景 5 原有断言（线上报文 data URL
形态）继续兜住端到端正确性。

- 验收：C++ 474/0、Lua 驱动单测 125/0、`agent-all` 7 段 0 失败。

### 2026-09-22 agent 重构阶段 C3（REPL 循环可离线单测）

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 C3（对应 R7）。

REPL 循环此前内联在 `runAgentMode`，与装配逻辑（工具表/审计/上电/一次性任务分支）
耦合，循环行为只能靠 agent_e2e.sh 六场景兜底。拆出 `runAgentRepl`（session + 只读
配置视图 + robot + 三条流），I/O 全经参数注入，循环/命令/错误恢复可离线驱动。

- 行为零变化：`runAgentMode` 只保留装配，REPL 段原样搬入 `runAgentRepl`。
- 新增 [test_agentrepl.cpp](../test/cpp/test_agentrepl.cpp) 六用例：`/exit` 不触发模型
  且提示符格式正确、EOF 退出码 0、内置命令（/help //hardware //scripts）不触发模型、
  单轮模型失败不结束会话、输入超限响亮拒绝且会话继续、`/clear` 后请求不含旧历史。
- harness 作用域名册补 `agentrepl_`（AG-22 自检机制照常起效——漏登记立刻红）。
- 验收：472/0（agent: 156/0）、`agent-all` 7 段 0 失败。

### 2026-09-22 agent 重构阶段 C4（历史预算口径诚实化）

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 C4（对应 R8，按方案倾向选「改注释」而非改实现）。

历史字节预算的记账**不含** system 提示词（messages_[0]，恒不被裁剪、另有 64 KiB
独立上限）与工具 schema（注册期固定、逐请求重复发送）；单条估重（`messageWeight`）
不计 role 字符串、JSON 语法开销与非 ASCII 的 `\uXXXX` 转义膨胀（CJK 约 2×），是
线上字节的**下界**。原注释只说「不计 JSON 语法开销」，未披露这两类系统性偏差。

- 注释修正（[agenthistory.cpp](../src/agenthistory.cpp) messageWeight、agenthistory.hpp
  trim、agentloop.hpp maxHistoryBytes），实现零改动。
- 钉子用例 `agentloop_history_budget_excludes_system_prompt`：system 50KB + 两块各
  ~10KB + 预算 25KB——按真实口径不裁；若误把 system 计入（70KB > 25KB）会裁掉旧块，
  用例在两种口径下结果不同，能抓住口径回退。
- 验收：466/0（agent: 150/0）、`agent-all` 7 段 0 失败；docs/agent.md 预算字段段同步。

### 2026-09-22 agent 重构阶段 C1（REPL /clear）

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 C1（对应 R6）。

此前上下文被一次错误结论污染后，唯一出路是重启进程——重启会 `deactivate()` 释放硬件，
执行器停在归零位，现场代价不小。新增 REPL `/clear`：只裁会话历史（保留 system 提示词），
机器人保持上电。

- `AgentSession::clearHistory()`：`messages_` 截断到 system 一条；任务内预算计数随下一轮
  `runTurn` 开头的 `resetTask` 归零，无需在此处理。
- REPL 新增 `/clear`（确认文案「已清空对话记忆（硬件保持上电，系统提示词不变）」），
  `/help` 与欢迎行同步。
- 验收：构建零告警；全量 465/0（+1，`agentloop_clear_keeps_system_only` 钉住
  历史归零/system 保留/清空后请求干净/幂等）、`agent:` 149/0；`agent-all` 7 段 0 失败。
- docs/agent.md REPL 命令段同步。

### 2026-09-22 agent 重构阶段 B2（LlmOpenAI 不再内置缺省接入点）【破坏性】

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 B2（对应 R3）。

**影响**：此前 `LlmOpenAI` 在 CLI / 配置文件 / 环境链都没配 endpoint 时会静默回退到
内置缺省 `https://moma.cmecloud.cn/v1/chat/completions`——「什么都没配就跑起来」的
会话把含结构脚本、硬件清单、摄像头画面的上下文发往该网关，仅 stderr 注记缓解。

- **新语义（fail-closed）**：endpoint 未配置时**第一次模型调用即失败**，ERR 点名
  三种配置途径（`--llm-endpoint` / `BOT4CJ_LLM_ENDPOINT` / 配置文件 `endpoint`），
  不发出任何报文；原网关用户显式配置即可（判据单点 `openai.require_endpoint`）。
- `LlmDeepSeek` 保留缺省（api.deepseek.com 是厂商官方 API 域），首次回退仍打
  stderr 注记（AG-13② 行为不变）。
- 缺省模型不受影响（显式配了 endpoint 而没配 model 的会话不连带失败）。
- 验收：`luajit test/test_llm_drivers.lua` 116/0（B2 钉子用例 4 条：无 endpoint
  组包失败、报错点名途径、DEFAULTS 不再含 endpoint、DeepSeek 回退不变）；
  `bash test/run_all.sh agent-all` 7 段 0 失败；C++ 464/0。
- 文档同步：README 四处快速开始补 `BOT4CJ_LLM_ENDPOINT`；docs/agent.md 快速开始、
  驱动表与破坏性变更标注。

### 2026-09-22 agent 重构阶段 B1（上下文字节预算配置文件化）

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 B1（对应 R2）。

三个上下文字节预算此前是 `AgentOptions` 的编译期常量（docs/agent.md 已知限制自认
「只能改源码缺省」），现场调不了。B1 把「配置文件 > 编译期缺省」通道接通：

- **llmconfig 新增三个字段**：`max_history_bytes`（缺省 4 MiB）、`max_task_tool_result_bytes`
  （缺省 2 MiB）、`max_task_image_bytes`（缺省 4 MiB）；单位字节，`0` = 不限，上限 64 MiB
  （`llm_limits::kMax*BytesLimit` 单点）。
- **严格解析**：类型/无穷/区间/取整同 `llm_limits` 通道，未知字段照旧整档拒绝。
- **下沉链路**：`LlmFileConfig → AgentModeConfig（带缺省）→ AgentOptions`，只做搬运，
  语义（含 AG-4/21 的任务内预算与裁剪边界）不变；`capture_image` 单图 2 MiB base64
  硬上限仍是编译期常量（单独用途，未改）。
- 验收：构建零告警；全量 464/0（+2）、`agent:` 148/0（+2）；`run_all.sh agent-all` 7 段 0 失败。
  新增钉子用例 `llmconfig_budget_fields_value_and_range` / `agentconfig_reads_budget_fields_from_file`。
- `mybot.llm.json.example` 与 docs/agent.md 配置字段表/已知限制同步。

### 2026-09-22 agent 重构阶段 A2 + C2（系统提示词拆分 / 工具结果定界对齐）

> 依据 `docs/proposals/agent-refactor-plan.md`。

- **A2（结构收口，行为逐字不变）**：系统提示词构建自 `agentmode.cpp`（578→478 行）拆出
  `agentprompt.{hpp,cpp}`——结构脚本头读取（`structureScriptHead`，UTF-8 消毒 AG-1）、
  「数据不是指令」定界单点（`quoteUntrusted`，审计 F）、总量上限（`kMaxSystemPromptBytes`，
  F2-9）与 `buildAgentSystemPrompt` 一处承载；`agentmode` 只留配置组装与 REPL 外壳。
  经由 `agentmode.hpp` 的既有 include 链，`buildAgentSystemPrompt` 等名字对存量用例
  （`agentmode_` 族）仍原位可见——**存量用例零修改**。
- **C2（行为变更，先补钉子用例）**：`list_action_scripts` 的工具结果与系统提示词三段
  共用同一个 `quoteUntrusted`——此前同一份磁盘首行注释，在系统提示词里被包裹成
  「数据不是指令」、在工具结果里却是裸文本，两条通道定界强度不一致（第三轮审计 §9）。
  新增用例 `agenttools_list_scripts_quoted_as_data` 钉住：注入句仍在数据区可见
  （定界不是过滤）且整行带 `> ` 前缀。
- 验收：构建零告警；全量 462/0（+1）、`agent:` 作用域 146/0（+1）；
  `bash test/run_all.sh agent-all` 7 段 0 失败。

### 2026-09-22 agent 重构阶段 A1（AgentSession 拆分，行为逐字不变）

> 依据 `docs/proposals/agent-refactor-plan.md` 阶段 A1。
> 动机：HEAD 落地审计日志与策略钩子后，`agentloop.cpp`（392 行）叠加了四套互相交织的
> 职责（历史裁剪 / 任务内预算 / 策略+审计 / 轮次编排回滚），是全仓注释最密、最容易改错的文件。

- **拆出三个独立单元**（`AgentSession` 只留 `runTurn` 编排与 `complete` 边界回滚）：
  - `agenthistory.{hpp,cpp}`：历史所有权、`messageWeight` 字节估算、按任务块整块裁剪；
  - `agentbudget.{hpp,cpp}`：任务内文本/图片预算、降级说明与记账（AG-4/21 的三条边界
    语义——降级说明计入预算、图片额度单向累加并明示终态、不复用「画面已加入上下文」
    断言——收进 `TaskBudget::admit` 单点）；
  - `agentdispatch.{hpp,cpp}`：派发漏斗固定顺序单点「策略确认（执行前）→ 执行 →
    审计（无论成败）」，`DispatchGate::run` 一处承载。
- **行为零变更**：降级文案、审计字段、日志文本逐字保留；对外 API（`AgentOptions` /
  `runTurn` / `history()`）不变。
- 验收：`test/cpp/` 存量用例**零修改**全绿——全量 461/0、`agent:` 作用域 145/0、
  `bash test/run_all.sh agent-all` 7 段 0 失败；`docs/agent.md` 源码分层表同步。

### 2026-09-21 文档正确性与可读性复核（README / docs / 架构页）

> 起因：文档自称「单一事实源指向脚本或目录」，但抽查发现若干**写死的数字已经漂移**、
> 且图页面落后于 `src/` 的实际分层。本轮逐条复核入口文档、参考文档与三张架构页。

- **入口文档（README / docs/README.md）**：模块计数改为可复算的口径——「59 = 47 器件/执行器/
  系统接口 + 3 总线 + 9 框架与模型驱动」（原写法把 `GPIO`/`PWM` 归在"器件"、把总线并入"框架模块"，
  读者无法复算）；新增 `bash test/run_all.sh agent-all` 入口并说明**为什么**裸 `agent`/`agent:`
  各只覆盖一半（AG-23）；「三份脚本设计与用法完全不变」补一句限定——移植过程修掉过几处
  **旧行为本身有问题**的地方（内联 `@Alias`/`@Vars` 作用域、SQL 未闭合引号），并指向 FAQ 与
  标「破坏性」的 changelog 条目；构建节把一个代码块里的两套构建拆开（避免读者把
  sanitizer 构建当成 Release 的续行）。
- **架构页（`architecture_analysis.html` + 两张 SVG）**：复核并纠正——`main.cpp` 行数 209→249、
  `robot.cpp` 132→197（`wc -l` 实测）；L4 行的 lib 清单按 `ls modules/lib/*.lua`（23 个）重写
  （原清单漏了 `hal_backend_glibc`/`bus_contract`/`llm_*`/`mjpeg_frame`/`utf8` 等，且把**不在 lib 里**的
  `bitcompat` 列了进去）；守门快照 251/0/1→260/0/2 跳、C++ 单测 378/0→446/0；分层图里
  「47 款器件/执行器驱动 + 框架库，共 59 模块」同样改为可复算口径，「agenttools 五工具」补上
  「（2 个条件提供）」——`capture_image`/`run_command` 缺省不注册，写「五工具」会让读者以为它们
  一定可用；日期口径改为「2026-09-20 重绘、2026-09-21 逐条复核更新」。
- **参考文档**：`testing.md` 的用法节补 `agent-all` 与过滤器形态说明（原说明只出现在 C++ 单测
  条目里、位置不显眼）。
- **复核过、确认无误（留痕以免重复核对）**：CLI 选项表与帮助文本的**双向**一致（既有机械守门
  `cliargs_help_option_set_matches_docs`/`cliargs_env_vars_documented_in_reference` 覆盖）；
  `--tcp-idle` 缺省 30s/范围 1-86400、连接总预算 600s 且随 idle×20 放大、口令失败 5 次才退出；
  `--help` 优先于未知选项（实测 rc=0）、无参数打印帮助 rc=0、未知选项 rc=2；
  `SINGLE_CONSUMER` 名单五只驱动与 `grep -l '^SINGLE_CONSUMER' modules/*.lua` 逐字一致；
  守护槽布局（336B / `values[32]@72` / `stop@328` / `status@332`）与 `daemonvar.hpp` 的
  `static_assert` 一致；virtbus 替换全局函数的数量（i2c 12 + spi 9 + uart 5 = 26）与
  `virtbus.lua` 的 `VFUNCS` 逐项一致；全部文档链接与 markdown 锚点可达（0 断链 / 0 失效锚点）；
  两张 SVG 的 XML 与 HTML 的标签结构均合法。

### 2026-09-21 第三轮审计修复批次（AG-19/AG-20/AG-22：图像内存、连接复用、作用域守门）

> 守门快照：C++ 单测 446 / 0（`agent:` 130）、骨架自证 8 / 0、`llm 协议` 单测 113 / 0 / 1 跳、agent E2E 通过、全量守门 260 / 0 / 2 跳。
> 本轮的判据是**实测**：峰值 RSS 用 `/proc/<pid>/status` 的 `VmHWM` 采样（脚本见 `.tmp/memaudit/`）。

- **`capture_image` 的整串拷贝与「先编码再拒绝」（AG-19，高）**：宿主里唯一搬运 MiB 级字符串的地方，此前对同一串 base64 调了 **6 次** `Json::getValue()`——那个访问器**按值返回整串**，于是每次拍照白白拷 5–6 份 2 MiB；而 2 MiB 判据又在**拿到 base64 之后**，驱动侧仿真上限 16 MiB 意味着「一张注定被拒的图」要先被编码、跨 Lua 边界才丢掉。现在：
  - 判据调用点一律借用 Json 节点自己的 `s`（零拷贝），空判/超限判/魔数校验/拼 data URL 全走引用；
  - 体积判据改成**两道**且第一道在编码之前：先用驱动声明的原始字节数判（上限 2 MiB×3/4 = 1572864 字节），base64 长度只作「驱动漏报/算错」的兜底（`checkImageSize` 单点，两种文案可区分命中了哪一道）；
  - 实测：10 MiB 图（超限）峰值 **219 MiB → 163 MiB**，且不再做无用的 base64 编码；1.5 MB 图 56 → 55 MiB。
  - **未做**（如实留痕）：单张图峰值仍是原图体积的数十倍——`驱动缓冲 → Lua 字符串 → base64 → Json 节点 → data URL → 历史 → 请求体` 上的副本属传输层架构问题，要真正压下来得让图片以引用/流式形态穿过这条链。已在 `docs/agent.md` 的「已知限制」写明实测数字。
  - 新增用例 2 条：早退判据（`…_rejects_oversize_raw_before_base64`，按文案区分命中了哪一道）与兜底判据（`…_falls_back_to_base64_length_when_undeclared`，用新夹具 `test/cpp/AgentFakeCamera.lua` 造一个**不报 `bytes`** 的驱动）。**反向验证已做**：把第一道判据置为 `false`，前者转红、后者仍绿——两道判据各自承重。
- **HTTP 连接每轮重建（AG-20，高）**：`llm_http.post` 原实现每次请求都新建 curl 句柄、返回前销毁，于是 `M.session` 存在的理由（「保留连接与 TLS 会话，省掉每轮重新握手」）在**任何生产路径上都没生效**——两个内置驱动都走 `M.post`。后果两层：每轮模型调用重做 DNS/TCP/TLS；历史里那张 base64 图每轮重新编码上传一遍。现在：
  - 整颗驱动共用一份**池化会话**（驱动持 `lua_State` 到 destroy，生命周期天然对齐）；新增 `M.close_pooled()` / `M.pooled_active()` / `M.pooled_handle()`；
  - 连接层失败（`rc≠0`）**自愈换新句柄**（把一次性网络抖动升级成会话级故障才是更糟的），HTTP 状态码非 200 **不**重建（那是应用层答复，连接本身是好的，换掉白丢 keep-alive）；
  - `Session:recreate` 必须排在 `curl_easy_reset` + `cb:free()` **之后**：recreate 会 `curl_easy_cleanup` 旧句柄，而旧句柄此刻还压着作用域内的回调——顺序写错就是「在已释放的回调上销毁句柄」，与本项目 AG-16 修掉的是同一个内存安全问题（**本轮实测：顺序写错直接 SIGSEGV，池化用例当场抓住**）。
  - **顺带修正 AG-16 的判据**：原用例「post 之后直接对同一句柄 `curl_easy_perform`，只要返回即证明已 reset」本身就在做 UB 的事——`cb:free()` 已把回调槽还掉，句柄若没被 reset 干净，这一 perform 就是踩在已释放对象上（旧实现「恰好没崩」只是 libcurl 早退，不是安全性证据）。改成只读句柄状态：`CURLINFO_EFFECTIVE_URL` 在 reset 后必须为空。
  - 新增离线用例：会话**创建计数**跨请求不得增长（旧实现每次 +1）、连接层失败至多触发一次自愈重建、关闭幂等、关闭后**不建池**的活跃探针；agent E2E 另加一条真报文链路上的同源判据（`BOT4CJ_HTTP_PROBE=1` 的见证行，2 轮调用只允许建 1 份会话）。**反向验证已做**：临时让 `pooled_session` 每次都新建，E2E 判据报「请求=2 不同计数=2」转红。
  - **判据为什么用计数而不是句柄地址**：libcurl 每次 `curl_easy_init` 申请同样大小的块，`free` 之后 malloc 常把**同一地址**还回来——反向验证时「每次新建」竟然通过了地址判据（假绿）。计数没有这个歧义。
  - **两个由守门逼出来的自身缺陷（都已修）**：① 池化后句柄不再随请求释放，`destroy` 里不关就等于把 libcurl 缓存留给进程退出——**LSan 实测 95 KB/进程**，已在两个驱动的 `destroy` 补 `http.close_pooled()`，回落到 261 字节（LuaJIT `require("ffi")` 的一次性成本，原实现同样有）；② 我最初还做了「连接层失败即 `recreate` 换新句柄」，**LSan 实测每次失败请求泄漏约 87 字节 / 4 处分配**（handle 的 share/DNS/TLS-session 缓存收不干净），断网重试下线性增长——而它本来就不必要（libcurl 自己会把失败连接标记为不可复用），已删掉并在 `Session:recreate` 上写明「请求路径不再自动调它」。
  - **净结果：ASan 构建下 agent 路径零泄漏**（`destroy` 关池 + 不重建句柄两条都到位后，连跑 5 次失败请求无 LSan 报告；`bash scripts/agent_e2e.sh` 在 ASan 产物下功能断言全绿）。**两条缺陷都是「反向验证/守门逼出来的」，不是设计时想到的**——这也是本轮最值得记的一点：AG-20 的收益（连接复用）本身简单，代价却藏在生命周期与错误路径里。
- **`agent:` 作用域表漏了 15 条用例（AG-22，中，AG-11 的回归）**：`harness.hpp` 的 scope 表没有 `agentconfig_` / `agentmode_` 前缀，于是 `test_llmconfig.cpp` 里配置优先级、`allow_shell` 缺省、提示词拼装与注入包裹等 15 条**一条都不在 `agent:` 里**，而原守门（「每个成员都有命中」+「总数 ≥100」）结构上不可能发现漏登记。
  - 表补两行；守门改成**派生式比对**：把注册表里所有用例名前缀反向列出，每个要么在作用域表、要么在 `test_util.cpp` 的「非 agent 面」roster 里**带理由**登记——漏登记一整族即红，而 roster 的反向检查顺带清理陈旧条目；
  - 覆盖边界如实写明：roster 的键是**前缀**，所以「给已登记族新增用例」自动随族走，「新增前缀却忘了表态」才会被拦；文件级归属无法从注册表推出（`Case` 只有名字），故不假装能测。
  - **反向验证已做**：撤掉那两行，守门报出 `'agentconfig_'`/`'agentmode_'` 未表态并转红。`agent:` 命中数 108 → **130**。
- **任务内预算的记账与终态（AG-21 + AG-4′，中）**：三处「看起来在正常工作、实际没兜住」的地方：
  - **降级说明自己不记账**：超限时替换进去的那条说明（约 380 字节）不计入 `taskToolResultBytes_`，于是「反复超限」可以往历史里持续灌说明文本。现在说明本身也计入（第一次降级即把额度顶满，后续行为幂等）。判据用说明**自己报的账**：第二次降级的「已用」必须等于第一条说明的长度——不依赖魔数长度，也不会把「说明被计入」与「载荷被丢弃」混为一谈。
  - **图片额度用尽后模型不知道**：图片预算逐任务单向累加，一旦某张装不下，本任务后续拍照**必然**全部失败；而旧文案只说「请改用更小的步子」，模型于是反复重试到轮次上限（实测一次任务连拒 3 次后以「达到工具轮次上限」收场，那 3 轮全白烧）。现在追加硬事实「本任务的图片额度已经用尽，**再调用也不会成功**；请立即改用文字结论或换一个新任务」。判据写成 `used >= cap` 是错的——本次 87 / 上限 100 / 已用 87 时 `used < cap`，但额度事实上已经用尽（**用例当场抓住**），已改成与触发降级的判据逐字一致的 `已用 + 本次 > 上限`。
  - **降级说明自相矛盾（AG-4′）**：图片降级时原样拼进 `capture_image` 的成功文案，而那句末尾是「画面已加入你的上下文」——与说明开头的「未注入上下文」直接冲突，而图片确实没注入。现在剥掉那句成功断言，改成「已把工具的描述性文字给你（画面本身没有进上下文）：…」。**这条的用例第一版是假绿**：夹具把工具文案写成「已拍照」，而真实驱动返回「已拍摄照片。画面已加入你的上下文。」——反向验证时用例照样绿，暴露夹具没复刻真实文案；改成逐字一致后才真能抓到。
- **门禁的 agent 过滤器口径（AG-23，低）**：一个 raw 过滤器要在四类分段上用**不同形态**匹配（Lua 段按 `test_*.lua` 文件名子串、C++ 段按 `scope:` 作用域语法、加固段按分段门、端到端按段名），此前没做区分，于是两条推荐命令**各只覆盖一半表面**：
  - `bash test/run_all.sh agent` → Lua 段命中 0 个文件（文件名里没有 "agent"）、C++ 走子串只命中 96 例（作用域里那 15 条 `agentconfig_`/`agentmode_` 因名字不含 "agent" 被漏）、4 条 agent CLI 负路径与 agent E2E 都跑不到；
  - `bash test/run_all.sh agent:` → 反过来：加固段比的是 `"agent"` 与 `"agent:"`（不相等）⇒ **跳过 agent E2E**。
  现在把「哪个分段拿哪种过滤器」收敛成 `test/run_all.sh` 顶部的**单点映射**，并新增覆盖全表面的入口 **`bash test/run_all.sh agent-all`**（Lua `test_agent*.lua` + agent E2E + C++ 作用域 `agent:` + 4 条 agent 负路径；实测汇总 7 通过 / 0 失败，C++ 130 + E2E 33 断言 + 加固 4 条）。裸 `agent` / `agent:` 的语义保持不变。
  - 映射本身带一条**自证**：任何一格被改坏（分段改名、变量清空、映射写错）都在入口处 rc=2 响亮失败，而不是等使用者发现某个分段悄悄没跑——AG-23 的成因正是「四处分散判断、没有一处对整体负责」。**反向验证**：把 `filter_lua` 清空，入口立刻报「以下分段不会执行：lua(空值=跑全部文件)」。
  - 顺带修掉段内的粗粒度：agent 负路径原先整段共用一个门，于是 `agent_missing_key` 会把 4 条全跑一遍；现在每条用例各自过 `case_matches`（单条名过滤实测只跑 1 条）。
  - `docs/testing.md` 与 `docs/agent.md` 改为推荐 `agent-all`，并写明裸过滤器为什么只覆盖一半。
- **文档如实写明预算的三条边界**（AG-21 余部）：`docs/agent.md` 的「已知限制」补上——裁剪只在任务开头做一次，所以任务是**稳态** 4 MiB 上限而非瞬时上限（进行中可达约 6 MiB）；图片额度单向累加；降级说明计入预算。
- **一条「看着在测、其实没测」的用例（AG-24，低）**：`test_agenttools.cpp` 的高编号 fd 用例在 `RLIMIT_NOFILE` 偏低时是裸 `return;`——前面已经计过一次断言，于是 AG-5 的「零断言即失败」防线不会触发，用例报 `[PASS]` 而真正的判据（≥4200 的 fd 不泄漏给子进程）一次都没跑。改成 `TH_SKIP("RLIMIT_NOFILE 过低…")`：**跳过必须显式**，不能混进通过位。
- **两条 agent 记忆行为此前零覆盖（AG-25，低）**，补上并各自反向验证：
  - 「传输在**第 2 轮**失败 → 第 1 轮已完整回传的 `tool_calls`+`tool` 必须留在历史」——原用例只覆盖 `complete == base`（turn-0 失败，待回滚的只有刚 push 的 user 消息），回滚边界本身没被检验；**反向验证注掉 `messages_.resize(complete)` 后转红**。
  - 「**最终回答**的 `extras` 必须随 assistant 消息留在历史（下一轮带 tools 的请求要完整回传，否则 DeepSeek 400）」——loop 级此前零覆盖（一次性 `--task` 永远不会再发第二轮，只有 REPL 的第二条任务看得到），用第二个任务当观察窗；**反向验证注掉 `asst.extras = resp.extras` 后转红**。
  - 覆盖边界如实说明：「assistant(tool_calls) 已写、随后的 tool 消息写了一半才抛」那种半截尾部**离线测不了**——`dispatchOneCall` 与 `AgentToolRegistry::execute` 把 `std::exception` 全吞了，要走到那条路径需要非 std 异常 / 抛错的 log 流 / `bad_alloc`，单测里构造不出来。`docs/agent.md` 的那条声明因此仍只有代码路径可读，没有可红的用例（不假装测了）。

### 2026-09-21 第二轮审计修复批次（AG-11：覆盖率口径 + fork/exec 盲区）

> 守门快照：C++ 单测 439 / 0、骨架自证 8 / 0、Lua 单测 260 / 0 / 2 跳（以实时输出为准）。
> **结论：不加行覆盖阈值**——实测该指标既漏链路又有结构性盲区，设阈值只会把测试压力挤到测得到的区域。

- **fork/exec 区块改用行为断言**：`agent_subprocess.cpp` 的 `execShellChild` / `revokeInheritedFds` 在 gcov 报告里**恒为 0%**，且不是「没测」而是「测了测不到」——gcov 计数在进程退出时落盘，而这两个函数永远以 `execve` 结束（exec 用新映像的计数页覆盖当前进程）。AG-3（`CLOSE_RANGE_CLOEXEC` 标志写成 `1U`、快速路径从未执行）正在这个盲区里藏了很久：该区块对所有读者都显示 0%，所有人都当它是「有意不测的部分」。解法不是去测量它，而是让它**自己报告**：子进程在 fork 后、exec 前把走了哪条路径写进 `BOT4CJ_TRACE_FDS` 指定的 fd（经参数传递，避免宿主守护线程并发 fork 时互相踩），父进程读轨迹断言。**反向验证：把标志退回错误的 `1U`，轨迹只剩 `close_range-attempt`，用例立刻红。**
- **覆盖率口径修正**：三处问题——① 不清理陈旧 `.gcda`，重建后 libgcov 报 `different checksum` 并**混合两套构建的数据**；② `mybot` 未插桩，导致「叠加守门」的数字一点不涨（极易被读成「e2e 没有覆盖价值」，实际是它压根没被计入）；③ gcov 采集循环对同一源文件的两份 `.gcno` 互相覆盖，可能覆盖成没有 `.gcda` 的那份（报告退化成 `0.00%`）。修完后新增 `COV_WITH_GATE=1`（额外跑全套守门，并显式把 `BOT`/`BOT_TESTS` 指向插桩产物）。实测同一份源码：**仅单测 77.90%（`main.cpp` Runs:0）→ 全链路 79.96%（Runs:177）**，e2e/TCP/Lua 链路第一次真正进入统计。
- **`gate:coverage` 的转硬条件改写**：原条件「设定 C++ 侧行覆盖基线」被上述实测否定，改为「覆盖数据可追溯 + fork/exec 盲区确实由行为断言覆盖」。
- **过滤器改用作用域匹配**：`docs/agent.md` 推荐的 `./build/mybot_tests agent` 是子串匹配，只命中用例名含 "agent" 的（实测 86/436），漏掉 `promptpack` / `llmconfig` / `cliargs` 这些同属 agent 配置面的用例——而同一行注释写着「proto / driver / tools / loop / config」。现在 `agent:` 走显式作用域表（108 例），裸子串语义保留；表的完备性由一条主套件用例守着（成员必须真有用例、命中数 ≥100），新增 agent 面测试文件忘记补表即红。
- **文档写明盲区**：`docs/testing.md` 新增「覆盖率数字的口径与已知盲区」小节，讲清口径、fork/exec 恒 0% 的成因、以及为什么不该在此指标上设阈值。

### 2026-09-21 第二轮审计修复批次（AG-4/AG-8/AG-18）

> 守门快照：C++ 单测 436 / 0、骨架自证 8 / 0、Lua 单测 260 / 0 / 2 跳（以实时输出为准）。

- **任务内聚合体积预算（AG-4）**：`maxHistoryBytes` 只管「已完结的旧任务块」，且最新块无条件保留——于是**正在进行的任务**没有任何体积上限：模型只要多调几次工具，就能在一个任务里把 `maxTurns × 每轮调用数 × 单次上限` 的字节灌进请求体（`capture_image` 单张就 2 MiB base64）。一旦网关以 413/400 拒绝，那块历史既不在回滚范围内、也不能被裁掉（`trimHistory` 不裁最新块），REPL 从此每条任务都以同一个请求体失败——与 AG-1 同一失效形态。现在给任务加两条上限（`maxTaskToolResultBytes` 2 MiB / `maxTaskImageBytes` 4 MiB，0 = 不限制）：
  - 超限的那一次工具结果**不进历史**，换成一句说明回给模型——它据此知道「这一步没拿到数据，换小一点的步子」，而不是让整个会话进入不可恢复状态。
  - 为什么是「换掉」而不是「截断」：截断会让模型看到半截内容却以为完整（比没有更危险）。为什么不在执行前判：体积要拿到结果才知道，而工具可能已产生副作用，此时谎称「没执行」会误导模型，所以文案如实交代「工具已执行、结果因体积未注入」。
  - 预算按**任务**清零（跨任务由 `maxHistoryBytes` 管），文本与图片分别计账。
- **提示词体积检查前移（AG-8）**：总量上限原本只在 `buildAgentSystemPrompt` 里查，而那时 `robot.active()` 已经跑过——与「配置错误不碰真机」的口径不符。现在把**不依赖硬件**的三段（结构脚本头 / 动作脚本清单 / 提示词包）在 `buildAgentConfig` 就查一遍（同一常量、同一口径），于是这类超限在**上电之前**以 rc=2 失败（顺带让 AG-1 的「结构脚本头/脚本首注释带坏字节」也在配置期就暴露）。含**硬件变量清单**的那部分必须等 `active()` 装配完才算得出来，仍留在原处作为最终判据——这是接口本身的约束，文档已如实写明这个例外。
  - **破坏性**：超大提示词的退出码由 1（上电后失败）变为 2（配置错误，不碰真机），报错文案对应更新；守门 `agent_prompt_over_cap` 已同步。
- **相机抓帧的边角语义（AG-18）**：三处「看起来在正常工作、实际丢掉了关键信息」的分支：
  - `poll` 的 `pr <= 0` 一律报「等待帧超时」，**errno 被丢弃**——`EINTR`（被信号打断）与 `EBADF`（句柄失效）全被说成「摄像头卡住」，排障方向被带偏。现在三种返回分开报，`EINTR` 与 errno 各自点名。
  - `V4L2_BUF_FLAG_ERROR`（内核明说「本帧不可信」）**从不检查**，完整性只靠 EOI 启发式。现在置位即整帧丢弃（与截断帧同一处理路径）。
  - buffer `index` 越界与 `bytesused` 越界原本共用一条文案，现在先校验索引再查表，两类成因分别点名，且无论哪条路径都把缓冲还回队列。

### 2026-09-21 第二轮审计修复批次（AG-9/AG-13 余部）

> 守门快照：C++ 单测 432 / 0、骨架自证 8 / 0、Lua 单测 260 / 0 / 2 跳（以实时输出为准）。

- **`capture_image` 的 MIME 改由内容决定（AG-9）**：宿主此前恒拼 `data:image/jpeg;base64,`，而 `--camera-src` 可以是任意图片——传一张 PNG 就得到「JPEG 标签 + PNG 字节」，严格网关按图片解析直接 400。现在：
  - `mjpeg_frame.lua` 新增 `image_format()`：按**魔数**判定 jpeg/png/gif/webp（webp 是 RIFF 容器，必须连第 9-12 字节的 `WEBP` 一起看，否则 WAV/AVI 会被误认）；驱动仿真路径据此返回真实 `format`，**不认识的格式响亮拒绝**（`unsupported sim image format`），而不是猜一个 MIME 发出去。
  - 宿主采用驱动声明的 `format` 拼 MIME（`image/<format>`），并做一次**自洽交叉校验**（base64 前几字节解出的魔数必须与声明一致，不符即拒绝发出）——只信声明则驱动写错就发错标签，只信魔数则宿主得自带一份与驱动必然漂移的格式表。
  - 校验点收敛到 `load_sim` 单点、`build` 与 `call` 共用：格式不对在**上电前**就失败（`build` 返回 ERR）。
  - **破坏性**：`--camera-src` 指向非 JPEG/PNG/GIF/WebP 的文件，从「照发」变为「build 阶段拒绝」；仓内三处测试夹具原本用 `"foobar"`/`"IN"` 这类假字节冒充图片，已改为带真实魔数的样本（这类夹具此前正好掩盖了缺陷本身）。
- **文档与实现对齐（AG-13 余部）**：
  - 图片消息契约曾写成「宿主传 `{"images":[...]}`、由驱动展开成 content 数组」——**实际宿主直接产出 content 数组**（`agentproto.cpp` 的 `messageToJson`），全仓没有 `images` 键。照旧文档写第三方驱动会永远收不到图片；现已改正，并写明驱动只需透传/映射 `content`。
  - 「驱动 `build` 不打开设备、也不返回 ERR」措辞过宽：真机路径成立，但显式给了仿真来源时路径越界/文件缺失/格式不支持会**刻意**在 `build` 返回 ERR（已加限定语并说明理由）。
  - 非 agent 模式下被拒的选项清单补齐 `--camera-var` / `--camera-device` / `--camera-src` / `--no-camera`（此前只列 5 类，实际这 4 个同样退出 2）。
  - 随包提示词 `prompts/spider_robot/3-命令行.md` 教了一个**会被拒绝**的开启方式（「或配置 `allow_shell: true`」）——那样写的配置文件会被拒绝加载并退出；现已改为只讲 `--agent-shell`，并说明为什么配置文件不能开。

### 2026-09-21 第二轮审计修复批次·测试骨架（AG-5）

> 守门快照：C++ 单测 430 / 0、骨架自证 8 / 0、Lua 单测 260 / 0 / 2 跳（以实时输出为准）。

- **零断言即失败（AG-5）**：`harness.hpp` 此前只按「有没有断言失败」判定通过，于是一条断言都没跑就结束的用例照样记 `[PASS]`——「环境不满足就 `return`」的守卫整条静默通过，什么都没验证却占了通过位。现在记录每用例的断言数，零断言**判失败**并要求显式表态：新增 `TH_SKIP("原因")`（记 `[SKIP]` 并计入跳过数，必须带原因）；**断言之后**再 `TH_SKIP` 属语义矛盾，判失败。全仓早退点逐条审计后只有一处真受影响（`test_util.cpp` 的软链策略用例，已改为 `TH_SKIP`），其余早退点此前都已有断言。
- **骨架自证（新的守门分段 `harness-selftest`）**：`test/cpp/harness_selftest.cpp` 用一份**独立的 th 状态**把已知形态喂给骨架，断言判定符合预期——零断言必失败、`TH_SKIP` 不算失败、断言后跳过判失败、正常通过/失败仍被认出、过滤器零命中返回 -1。它守的是**所有其它守门都依赖的那把尺子**：尺子失真时全部用例照样全绿，没有别的机制会发现。独立二进制的理由是那些「例句」用例故意含失败形态，混进 `mybot_tests` 会造成永久失败项。
- **写自证用例时暴露并修掉的三个骨架自身缺陷**（都不是预想中的，是被自证逼出来的）：
  1. `markSkipped` 无条件清失败位，会把**已经发生的真实失败**抹掉，再洗成 `[SKIP]`；
  2. 「断言后跳过」只打印一句 `reportFailure` 就 `continue`，失败数不增——判失败却没记账；
  3. 全跳过的过滤被报成「过滤器拼错」并返回 -1（`passed+failure` 判据漏了 skip），会对 `run_all` 的空匹配防线产生误报。
  另外把 `runAll` 的统计从全局单例改为**每次运行一份**（嵌套调用不得抹掉外层的账），并让 `quiet` 模式**整段**静音——两处都是自证用例读不到正确计数时查出来的。
- **反向验证**：临时注入一条零断言用例，`mybot_tests` 报 `(零断言)` 且 rc=1、`run_all.sh` 的 cpp-unit 段判失败（修复前该形态一路绿灯）；验证后已删除注入。

### 2026-09-21 第二轮审计修复批次·续（AG-6/AG-10/AG-13②/AG-17）

> 守门快照：C++ 单测 430 / 0、Lua 单测 259 / 0 / 2 跳（以实时输出为准）。

- **`run_command` 的超时/整组终止有了有效断言（AG-6）**：原用例只 grep「超时」文案，不看 `exit_code`、不检查子进程是否真死、**没有任何孙进程用例**——而 `kill(-pid)` 的「整组」正是超时语义的实现前提，此前无一条断言钉住它。新增两条：
  - `agenttools_run_command_timeout_kills_whole_process_group`：子 shell 用 `( ... ) &` 起**同组**孙进程（它把自己的 stdout/stderr 重定向到 /dev/null 后持续写文件——重定向是关键，否则子 shell 死后孙进程会撞 EPIPE/SIGPIPE 而停止，判据被副作用掩盖），断言 ① 超时路径**不得**触发硬上限兜底（有后代吊着管道写端才会，即「只 kill 直接子进程」的指纹）、② 输出停止增长、③ `exit_code=-1`、④ 耗时远小于命令自身时长。
  - `agenttools_run_command_timeout_receipt_is_actionable`：回执必须同时给出 `exit_code=-1` 与「超时」，且**不得**出现硬上限那句（两者是不同失效语义，混了就看不出成因）。
  - 反向验证：把 `kill(-pid)` 改成 `kill(pid)`，两条判据同时转红。
- **放弃回收不再静默（AG-10）**：`reapWithGrace` 在宽限内收不回子进程时直接 `return false`，注释写「交由 init 收尸」——但常驻 agent 自己还活着，init 不会接手，该 pid 会一直是僵尸，反复触发（D 态挂死的命令）就在宿主生命周期内累积。现补一行 stderr 告警，点名 pid、宽限时长与「会保持僵尸」的后果（与 `RunResult::hardStopped` 一起，操作者与模型两侧都能看见）。告警文案抽成 `reapGiveUpWarning` 单点：该路径的**行为**在离线环境构造不出来（需 D 态进程，或 SIGKILL 无效——后者要求 PID 1/受限进程，容器里测试二进制不是 PID 1，`trap "" KILL` 实测无效），故钉住「说了什么」而不是假装测了行为。
- **DeepSeek 缺省端点也有注记（AG-13②）**：`note_default_endpoint` 原先是 `llm_wire_openai.lua` 的**文件内局部**函数，于是只有 OpenAI 有这行提示，而 DeepSeek 的缺省端点恰恰指向外部厂商 API——最该让操作者看见「数据流向哪里」的链路反而静默回退。现提到模块上供两库共用（同一份「每进程只注记一次」状态与同一条文案）。
- **`thinking = false`（布尔）能关思考模式了（AG-17）**：`thinking_enabled` 此前只认字符串 `"disabled"/"off"/"false"`，传 JSON 布尔 `false` 时请求体照样带 `{"type":"enabled"}`——与该函数自己的注释矛盾。宿主走 `wantString` 故配置文件路径到不了这里，但 `.mt` 动作脚本与结构脚本参数可以直接传布尔，而 `false` 是最自然的一种「关掉」写法。修法与开关联动：关闭思考模式时不再回传 `reasoning_content`（有状态协议的另一半，非思考模式回传该字段会被真机拒）。

### 2026-09-21 第二轮审计修复批次（AG-1/AG-2/AG-3/AG-14/AG-15/AG-16，agent 子系统）

> 守门快照：C++ 单测 427 / 0、Lua 单测 259 / 0 / 2 跳（以实时输出为准）。
> 本轮由一次独立复审驱动：对上一轮报告抽样复现 7 条（全部为真），同时**纠正 1 处机制归因**、**下调 1 处严重度**。

- **非法 UTF-8 不再进请求体（AG-1，高）**：JSON 字符串只允许合法 UTF-8，而两端编码器对 `0x80-0xFF` 都是**字节透明**的（那是合法多字节透传所必需）。于是 `run_command` 的 stdout/stderr（`cat` 到二进制、Latin-1 文件名、非 UTF-8 日志）、动作脚本首行注释、结构脚本头里任意一个坏字节，都会让严格校验的网关直接 400——而那条消息已落在**不可回滚**的历史里（回滚边界与按块裁剪都碰不到它），REPL 会话从此每次以同一字节偏移失败。现分两层收口：
  - **宿主侧消毒**：新增 `util::isValidUtf8` / `sanitizeUtf8`（RFC 3629 判据：拒过长编码、拒 UTF-16 代理区、拒 > U+10FFFF），在「数据进入模型上下文」的三个边界调用——子进程输出（`finalizeShellRun`，**收尾时整串消毒**，避免增量排水把多字节序列从中间切断）、动作脚本首注释（`firstComment`）、结构脚本头（`readHead`）。非法字节替换为 U+FFFD；连续坏字节折叠成一个替换符 + 计数说明，避免 64 个坏字节膨胀成 64 个替换符。
  - **发包前兜底**：新增 `modules/lib/utf8.lua`（与 C++ 侧同判据、同文案），`llm_http.post` 在设置 `POSTFIELDS` 之前校验请求体，非法则写 stderr 告警并就地替换。它兜的是宿主没覆盖到的路径（例如驱动 `extras` 里由 provider 文本原样带回的字段）。
  - **归因修正（重要）**：上一轮报告称坏字节由 `src/json.cpp` 的 `escapeString` 送出，实际报文由 `modules/lib/json.lua` 的 `encode_string` 编码（宿主把请求作为 Json→Lua **表**交给驱动）；`Json::toString()` 只用于日志与报错文案，**不在 HTTP 路径上**。修复落点据此确定。
- **假网关补齐真机规则（AG-7 核心）**：`scripts/fake_llm_server.py` 此前用 `except ValueError` 兜 `json.loads`，而 `UnicodeDecodeError` 是 `ValueError` 的子类——非法 UTF-8 的请求被判成 `{}` 后照常 200，**假网关比真网关宽松**，AG-1 在离线守门里结构上不可见。现补三条真机规则：body 非 UTF-8 → 400（点名首个坏字节位置）、`reasoning_content` 有状态要求（请求中出现该字段时，每条带 `tool_calls` 的历史 assistant 都必须带上）、`tool_call_id` 与 `assistant.tool_calls` 严格一一配对（孤儿 / 重复 / 漏回传都 400）。
- **`Camera.lua` 抓帧重试不再写全局 `err`（AG-2，高）**：`payload, err = grab_one()` 把 `drv_common` 的**全局** ERR 构造器覆盖掉——实测三条后果：成功路径赋成 nil、抓帧失败赋成字符串，此后**同一 lua_State 里所有驱动**的 `err(...)` 全部报 `attempt to call a nil/string value`；而那句「8 consecutive incomplete frames」诊断自己先撞上 nil，**100% 不可达**（循环体每轮都先给 `err` 赋值），`gerr` 是从未赋值的死变量，`grab_one` 的真实原因因此丢失。现改用局部变量，并在入口加「构造器缺失即返回同构 ERR 表」的前置校验（fail-closed，驱动对宿主永不抛裸 Lua error）。契约 sweep 增一条「加载完全部驱动后全局 `err` 仍是可用构造函数」的断言，单条覆盖全部模块。
- **`close_range` 标志修正 + 硬上限如实上报（AG-3）**：`CLOSE_RANGE_CLOEXEC` 是 `1U<<2`，旧实现写 `1U` → 内核 EINVAL → `run_command` 的快速路径**从未执行**，每次都白跑一遍按 `RLIMIT_NOFILE` 的退化循环（实测 soft=524288 时约 40ms），而注释写着「一次系统调用」。安全结论不受影响（退化路径是真 close，且父进程侧 `markAllFdsCloexec` 独立承重），注释与实现不符已修正。另：`hardDeadline` 兜底 break 此前不带任何标记，回执看起来像「正常结束但退出码 -1」；现新增 `RunResult::hardStopped` 并在回执里如实写「超过硬上限仍未确认退出……命令可能仍在运行」。
- **API key 拒绝控制字符（AG-14，中）**：密钥会被拼进 `Authorization: Bearer <key>` 交给 `curl_slist_append`，而 libcurl 对 slist 里的 CR/LF **不做拒绝**——实测一个多行密钥文件会把换行后的内容变成**额外的请求头**发给网关。现两条路一起收口：宿主侧 `validateApiKeyChars`（`llmconfig` 单点，在环境变量 / `api_key` / `api_key_file` 三源**汇合之后**校验，报错点名字节值与位置），驱动侧 `api_key_has_control_chars`（`llm_wire_openai`，站在建头那一行的第二道，不假设调用方已校验）。
- **必填字段显式断言取代死代码（AG-15，中）**：`json.lua` 里「字段为 nil 即报错」的分支是**死代码**（Lua 表不可能有「键在、值为 nil」的项），于是必填字段缺失时被**静默丢掉**，真机只回一个含混 400。现删掉该分支并说明原因，改由 `llm_wire_openai.require_wire_string` 在组装处点名断言（字段名 + 第几条消息 / 第几个工具调用）；两个 LLM 驱动的 `call()` 也把 `wire.request` 的抛错折成 ERR 契约（原先会裸抛给宿主）。
- **句柄收尾不留悬垂回调（AG-16）**：`llm_http` 此前在 `perform` 后只 `cb:free()` 而不清句柄，`WRITEFUNCTION` / `URL` / `POSTFIELDS` 仍指在已释放对象上；当前只因 `Session:post` 每次都先 `curl_easy_reset` 才安全，而 `Session.handle` 是公开字段——任何「同句柄 perform 两次 / 加重试」的重构都会把它变成内存安全问题（实测：post 后直接 perform 报 bad callback；连做 500 次 post 让槽位复用后再直接 perform，SIGSEGV）。现按「**先 reset 再 free**」收尾，把这条隐含前提变成句柄自身的不变量。

### 2026-09-21 F5 正确性批次（Json 零拷贝 / SQL 行列号 / UART 共享告警 / Lua 保护帧）

> 守门快照：Lua/e2e 257 / 0 / 1 跳、C++ 单测 414 / 0（以实时输出为准）。

- **`Json::find(key)` 零拷贝访问（F5-3）**：热路径（`ensureOk` 的 result/message 检查、元数据合并的存在性判断）改走 `find`，免去 `get` 的整值 `optional<Json>` 拷出；`get` 旧契约（非对象接收者抛异常）保持不变。
- **SQL 词法层行列号（F5-1）**：词元携带 1 起行列，语法报错从「Token <词元>」扩展为「Token <词元> (line N, column M)」（既有子串锚点不受影响）；**破坏性语义修复**：未闭合引号跨越 EOF 此前被剥引号当名字静默放行（值无声截断），现报「Unterminated string starting at line N, column M」。列按字节计。
- **BH1750 常量机械对照 lint（F5-5①）**：测试内手抄的命令常量表与驱动逐条比对，故意改测试常量即红——「两边同错不报警」的预言机缺口收口。
- **同物理串口多独立 fd 告警（F5-4）**：两个消费者（重复声明的 UART 总线变量、或 UART 变量 + 真机守护）各自 open 同一 `path` 时，结构装配期打印告警并点名双方（HAL 无跨变量引用计数，后关闭者以自己 open 时的配置为准）。只告警不拒绝；多器件行共享同一 `$uart-x` 引用是安全形态，不告警。取向见 [faq.md](faq.md)。
- **Lua C-API 受保护帧（F5-2）**：`call_lua` 的「取全局函数 + 造参数表 + 填参」与 `luaGlobalType` 移入 `lua_pcall` 保护帧——驱动给 `_G` 装带报错 `__index` 的元表后，宿主 `getglobal` 会执行该元方法，旧实现在 longjmp 构建下裸崩（134/139、跳过清理链）、C++ unwind 构建下丢掉全部报错文本折成含混的「non-standard exception」；现两种构建都折成既有 ERR 契约并保留真实报错。

### 2026-09-21 收口批次（守护停机 / 子 shell fd / 驱动契约 / 守门防静默）

> 守门快照：Lua/e2e 251 / 0 / 2 跳、C++ 单测 405 / 0。

- **守护停机二段屏障**：`deactivate()` 在动硬件之前，先给「join 超时被弃管」的守护一次真正退场机会（宽限 = 共享预算，封顶 30s）——此前弃管线程仍可能在逆序 `destroy` 期间碰总线。宽限耗尽仍活着的守护会在 stderr 如实点名（「宁告警不挂死」，不是静默）。
- **子 shell 继承 fd 全量吊销**：`run_command` 的退化路径不再以固定 4096 为上限，改用真实 `RLIMIT_NOFILE`（无限时封顶 1048576）；父进程 fork 前另按 `/proc/self/fd` 全量打 `FD_CLOEXEC` 兜底——器件 fd 不再可能被子命令继承。
- **驱动 `destroy` 补入参守卫**：`H03ARR` / `LlmOpenAI` / `LlmDeepSeek` 的 `destroy(nil)` 由「静默返回 OK」改为返回 ERR 契约表；契约 sweep 扩到 `destroy(nil)` 必返 ERR。宿主从不以非表调用 `destroy`，故真机路径无行为变化。
- **死绑定清理与引导样板归一**：删 7 处无引用绑定（`BMI270`/`SCSCL`/`ADS1256`/`MS5611`/`fft`）；63 处 mpath 引导行归一到同一字面形态。
- **守门防静默**：`test/cpp/*.cpp` 未登记进 CMake 清单即 `FATAL_ERROR`（新用例忘登记立刻红）；luajit 缺失时显式 `WARNING` 并以 125=SKIP 守卫（此前静默注册 0 个 Lua 用例）。
- **测试补齐**：沙箱中和清单（`os` 5 项 + `io` 5 项）与 `parseInterval` 边界（0 / 负 / 溢出 / >1h）改为表驱动断言。
- **守门补属性测试与覆盖率阈值**（工程化）：`jsonparse` 增「JSON 往返保值」属性测试——固定种子生成结构化文本（含 17 位有效数字小数、转义族、嵌套、空容器），断言再解析得到的**值与原来结构相等**且编码幂等；`test/run_coverage.sh` 支持 `COV_MIN_LINES` 阈值（未设则只报告，设了则低于即退出 1）。当前 `src/` 行覆盖基线 **80.07%（3387/4230）**。
- **系统提示词总量上限（64 KiB，fail-closed）**：结构脚本头（16 KiB 截断）、动作脚本清单（条目数不限）、提示词包（单文件 1 MiB、整包不限）三段各自有界而整体无人管——任一段被塞爆都会顶穿 provider 上下文或把指令区挤出注意力；静默截断则让模型看到的系统指令与作者以为的不一致。现超限即拒绝启动 agent 会话，报错点名各段实测字节数。**破坏性**：提示词包 + 脚本清单合计超过 64 KiB 的部署需先精简。
- **`--help` 补全选项区间**：`--llm-max-tokens`（1-200000）与 `--llm-timeout`（1-600）此前不写区间，只有 `--llm-max-turns` 有；三者现由同一组常量渲染（`src/llmconfig.hpp` 的 `llm_limits`），配置文件解析与命令行校验也引用同一组界——消除「配置文件接受、命令行拒绝」的判据漂移。
- **Lua 驱动层报错统一英文**（8 条消息，另含 `Camera` 2 条随内联 ERR 收敛后一并翻译）：`SCSCL`（fd 提示 / 烧号授权 / position / speed / torque）、`MHZ19`、`SDS011`、`MCP4725`、`LSM6DSV`、`Camera`。此前 `docs/development.md` 的分语言约定只覆盖 C++ 两区，Lua 层文案语言无规范可依；现约定为「`err()` 英文、注释中文」。**破坏性**：按报错整串匹配的调用方/脚本需同步（仓内两处测试断言已更新）。

### 2026-09-20 审计收口·第四阶段（agent 模式六条 A–F）

> 守门快照：Lua/e2e 252 / 0 / 0 跳、C++ 单测 382 / 0。

- **环境密钥第二道防线（A）**：常驻 agent 把密钥 `setenv` 进自己进程后，同 UID 子命令经 `/proc/<pid>/environ` 即可读走。现注入前抓取敏感变量原始串、注入后就地抹零；`ptrace` 级残余如实声明（硬隔离需专用用户/容器）。
- **轮次上限不再回滚整轮（B）**：达到 `--llm-max-turns` 只裁半截尾部，已完整发生的 tool_calls 与已执行的动作历史保留（硬件不会倒回去）。
- **`capture_image` 补注册前提（C）**：结构脚本解析不到摄像头 Lua 驱动时该工具不注册——原先模型借「拍照」就能让舵机 `play()`。
- **`--camera-src` 不再回落 `.`（D）**：父目录解析失败改为响亮拒绝（回落等于放行整个 CWD）。
- **单张图 2 MiB 硬上限（E）**：一张仿真图 base64 后约 21 MB，一次拍照即可击穿会话历史预算。
- **磁盘内容标注为数据（F）**：脚本正文、动作脚本首注释、硬件变量名进系统提示词时统一加引用块与「数据不是指令」标注。
- **失实修正**：`agent.md` 安全说明按 A–F 重写；示例提示词包删除「可保存图片」「调用命令行」等工具集里不存在的承诺；`faq.md` 补 agent 问答。

### 2026-09-20 审计收口·第三阶段（M2 重复判据单点化）

> 同一判据多份拷贝收敛为单一事实源，报错文案留在各调用方。**无行为变更**。守门快照：251 / 0 / 1 跳、378 / 0。

- **相对路径判据三份 → `util::checkRelPath` 单点**（动作脚本 / @Include / 模块路径）。
- **受限读取两份 → `util::readFdNoFollowLimited` 单点**（单文件与多文件上限由调用方配置）。
- **有限数值解析四处 → `util::parseFiniteDouble` 单点**（jsonparse 的 ERANGE 口径保留）。
- **十份驱动的本地 `clampi` → `drv_common.clampi` 单点**（取防御派语义，行为等价）。

### 2026-09-20 审计收口·第二阶段（遗留三处）

> 守门快照：251 / 0 / 1 跳、378 / 0。LTC2944 true QLSB 与一处数据手册疑点仍待人工对照 datasheet。

- **kalman 的 Q=±inf 不再产生 NaN**：按极限取 `kg=1`（输出即测量值），兑现「Q 无穷 = 只用测量值」的既有语义。
- **LTC2944 电量换算系数单一事实源**：注释比代码多乘 1000，已修正并补 mAh↔code 离线往返闭环。
- **json 数组 null 有损点补声明**：`[1,null,3]` 重编码会报错，头注与测试同步钉住。

### 2026-09-20 审计收口·第一阶段（门禁闭环 / 沙箱 / 非有限值防线）

> 主题：「声称的防线是否真能拦住」。守门快照：250 / 0 / 2 跳、378 / 0。

- **门禁闭环（A1/A2/A4/A5）**：UBSan 补 `-fno-sanitize-recover`（此前检出未定义行为退出码仍为 0，硬门禁结构上不可能红）；ioctl/UART/V4L2/CURL 常量自检改由 Lua 源生成（旧版在 C 里手抄一遍，改坏 Lua 也全绿）；守门自检进 CI；`rc==125` 须有 `[SKIP]` 横幅才记跳过（堵 `os.exit(125)` 洗白）。
- **⚠️ `require` 不再能按模块名加载文件（A3）**：文件/C 搜索器换为错误串桩，preload（`ffi`/`bit`）保留；驱动加载统一走 `mpath.resolve` + `dofile`。
- **⚠️ 非有限值（NaN/±inf）不再被静默接受**：`GPIO` 的 `value="nan"` 不再把线驱动到高电平、`wait`/`timeout` 传 NaN 不再无界挂死、`Servo`/`Timer`/`PWM` 同族收口，判据统一走 `finite_of`。
- **守护字段数超槽宽（values[32]）加载期报错**；守护退出路径 `pcall` 包裹 `destroy`，异常时总线仍释放。
- **文档失实收口**：`@Daemon` 值上限 16→32、CI 门禁描述、SVG 归属、`testing.md` 退出码 2→1。

### 2026-09-20 评估批次（安全缺省翻转 / C2 同族收口 / 门禁硬化）

> 守门快照：248 / 0 / 1 跳、376 / 0。

- **⚠️ 破坏性变更：`run_command` 缺省不再提供**——须 `--agent-shell`（或配置 `allow_shell: true`）显式开启，`--agent-no-shell` 随之移除。（**同日收紧**：配置文件这条路已由 `f06b712` 关闭——只能由命令行 `--agent-shell` 开启，配置文件里写 `allow_shell: true` 会被拒绝加载。）
- **⚠️ 破坏性变更：移除 `--llm-key`**——命令行密钥经 `/proc/<pid>/cmdline` 暴露；密钥只走环境变量链或配置文件。
- **⚠️ SQL 词法层把括号当分隔符**：`INSERT INTO T(a,b) VALUES(1,2)` 紧凑写法此前报词法错，现与留白写法产出逐字节相同的词元流。
- **C2 同族收口**：`Ema`/`Kalman`/`BH1750`/`GPIO` 的 NaN 入参统一回落缺省，不再「发布 NaN 却报 OK」。
- **真机/仿真判据统一**：SPI/UART 参数校验下沉 `bus_contract` 单点（此前真机静默 floor、仿真拒绝，同参数两种结局）。
- **平台单点**：`HAL.PWM_SYSFS_ROOT` 不再被 PWM 段字面量架空，「改表即移植」恢复成立。
- **CI 门禁硬化**：clang-tidy 退出码修正（此前取管道末端恒 0）、TSan `halt_on_error`、三条软门禁设可判定转硬条件、新增 `gate:coverage` 与 `gate:musl`。
- **文档失实**：`architecture_analysis.html` 数字同步、`reference.md` 补 `--prompt-file` 与三个环境变量。
- **守门补盲**：「帮助 ↔ docs 选项表」改逐文档判据（并集取巧不再能漏），新增环境变量完整性机械判据。

### 2026-09-20 审计修复批次（C1–C11）

> 依据独立审计 14 项缺陷（每项本机复现或代码核对）。守门快照：245 / 0 / 2 跳、371 / 0。

- **⚠️ 崩溃修复：Lua 驱动一次返回多个值时宿主段错误**——`lua_next` 错栈槽，末值为标量时 SIGSEGV、末值为表时静默丢首个返回表。
- **NaN 不再穿透数值校验**——`byte_of`/`uint_of` 曾把 NaN 当合法值，`i2c_write_reg(fd, reg, NaN)` 返回成功但写 0x00；收敛 `bus_contract.finite_of` 单点并修同族。
- **⚠️ INSERT 按列名落位**——列序≠建表序此前静默错列且绕过 Number 校验；列名未知/重复/漏列解析期报错（仓内 138 个 `.sql` 零变化）。
- **矩阵单元格报错截断 + 转义**——20 万字节单元格不再直灌 stderr/终端（复用表达式路径既有防线）。
- **MAX30102 FIFO_CONFIG 位域按数据手册修正**——内嵌模型同步修，单测不再假绿。
- **Camera 打开失败统一释放 fd/mmap**；**virtbus SPI fd 不再误登记为 I2C**。
- **守门自纠四条**：错误过滤器不再假绿、零用例 C++ 二进制判失败、退出码改 0/1（旧版 256 个失败回绕成 0）、孤儿夹具检测改整字段匹配（暴露 26 对真机夹具从未执行）；`BOT4CJ_TEST_VIDEO` 逃生阀可达。

### 2026-09-20 重构批次（CLI 可测化 / 卫生收口）

- **CLI 迁 `src/cliargs.*`**：`parseArgs`/`validateOptions`/帮助文本可被单测（`--help` 输出逐字节不变）；新增 12 条用例与「帮助 ↔ docs 选项表双向一致」守门。
- **守门自检 `test/run_all_selftest.sh` 起步**：用例登记/命名不再能静默变薄（pre-push 调用）。
- **⚠️ 词法报错文案**：`Token name'x'` → `Token name 'x'`（5 处断言同步）。
- **⚠️ `run_command` 环境清洗改整词匹配** + 复合密钥名表：`KEYSTONE`/`MONKEY` 等无关变量不再被误剔，真密钥仍剔除。
- **纵深防线**：`injectLuaDriver` 顺序守卫、`@Vars` 字面量 `end`/`isfinite` 复核、`ensureOk` 种类名下标断言、`mtmerge` 退出路径改 `return 2`。
- **卫生**：`.zcode/` 入 gitignore、fft 死赋值清理、Camera 头注对齐、文档计数修正。
- **流程**：`development.md` 新增「委派与自动化」节（只读约束 + 前后 `git status` 留档）。

### 2026-09-20 重构批次（会话作用域 / 模块路径 / I/O 语义）

- **⚠️ 破坏性变更：动作脚本内联 `@Alias`/`@Vars` 作用域收敛到「本次执行」**——此前进程级，会永久遮蔽结构定义并跨脚本残留；现由 `SessionScope` 在单次 `run()` 内持有、出口整体回滚。跨脚本共享别名须写入结构脚本；单次执行内行为不变。
- **多路径模块加载对模块侧成立**：宿主注入 `__mpath`，64 处引导行统一（此前只取 `BOT4CJ_MODULE_PATH` 首段）。
- **`run_command` 的 EINTR 当重试**、`TCP` 未认证对端不再能杀进程（三态鉴权，只有凭据不匹配计入爆破上限）、**超限文件报「超过字节上限」**（不再伪装成语法错误）、**表达式报错截断 80 字符**。
- **协议驱动入 `arg_table` 契约**；新增 `test_driver_contract.lua` 全模块 sweep（349 断言）。
- **守门改进**：真机跳过判定 `device_gate.sh` 单点（ctest 摄像头用例不再误报 Failed）；CI 构建带 `-DMYBOT_WERROR=ON`。

### agent 模式（`--agent`）

- **模式本体**：LLM 工具循环为主执行流程，硬件常驻内存，会话结束才 `deactivate()`；`--task` 一次性任务，缺省交互 REPL。与 `--tcp`/`--input` 互斥。
- **工具集**：`list_action_scripts` / `run_action_script`（路径沙箱 + 8 MiB 上限）/ `list_hardware`（只读）/ `capture_image`（V4L2 + MJPEG，路径白名单，支持 `--camera-src` 仿真）/ `run_command`（超时进程组终止 + 环境清洗，**缺省不提供**）。
- **模型调用是一颗 Lua 协议驱动**（`LlmOpenAI` 缺省 / `LlmDeepSeek`）：换模型 = 换一个 Lua 文件，宿主零链接期依赖（运行期经 ffi 用 libcurl）；超时钳 [1,600] 秒、写回调即用即释。
- **配置文件 `--llm-config`**：优先级 命令行 > 环境变量 > 配置文件 > 驱动缺省；严格校验非法即退出 2；`api_key_file` 权限过宽有提醒；选 JSON 不选 Lua（配置是数据不是代码）。
- **提示词包 `--prompt-file`**：Markdown 组拼进系统提示词，配置错误在上电前退出 2；REPL 历史按 4 MiB 预算整块裁剪；内置缺省端点回退时 stderr 注记。
- **守门**：agent e2e 六场景（工具循环抓包、协议差异、超时、配置、拍照、提示词包），缺 python3/libcurl 按 SKIP 计入未执行。

### 语言与 DSL

- **`$(...)` 表达式**：`@Vars`/元数据列/`@Action` 单元格三处可用（四则运算 + 括号，深度/运算符双上限）；文本操作数分派为拼接（别名入式），落点扩到 `@Action` 全部单元格；不引入裸 `+` 语法。
- **`@Action` 设备列 `@Alias`**：模板设备列经别名改绑复用片段；手写重复列由静默覆盖改报错。
- **`@Include` v2**：嵌套 + `*.once.mt` include-once + 主从覆盖（更靠近主脚本的定义获胜）；v1 内联归并复用 `mtmerge_core`，未知矩阵由静默忽略改报错。
- **controller/trigger 多列扇出**：一行多目标；controller 一个 ERR 即停止，trigger 按序前置调用。
- **`@Vars`** 命名数值变量（元数据列 `$<name>` 与参数位替换两种消费）；`Number` 列解析期校验；词法器引号内字面量与 8 MiB 上限；mtmerge 归并 `@Alias`/`@Vars`。

### 执行引擎与安全

- **守护槽地址注入改 `lightuserdata`**：「地址被 double 化后 ≥2^53 丢位」整类风险在语言层面消除。
- **TCP 加固**：非回环强制口令 + 恒时比较 + 空闲/总预算超时；口令可经 `BOT4CJ_TCP_TOKEN` 传递。
- **动作脚本路径沙箱**：`O_NOFOLLOW` 打开，软链条目不进工具清单（目录外文件首注释不再回显进模型上下文）。
- **浮点窄化前判有限性与值域**（float-cast UB 响亮拒绝）；`$` 引用校验前移到任何 `build` 之前；`destroy` 元数据恢复引用列（修复十余款器件静默丢失去初始化）。
- **派发热路径优化**：列头只解析一次、表达式 AST 按列缓存；沙箱拒绝 NUL 键值、未知 `--key` 退出 2。
- **沙箱残余如实声明**（`load`/预编译字节码/`ffi`）：威胁模型内驱动可信，列为有意取舍；`os.tmpname` 并入中和。
- **异常体系归口** `IllegalArgumentException`；报错文案分语言惯例成文于 [development.md](development.md)。

### 驱动与总线

- **26 个器件驱动 51 处内联类型检查迁 `arg_table`** + lint 防线（`proto=virt` 注入前置不再被绕过；Camera 有意豁免）。
- **多路径布局定位逐段探测**（`HAL`/`virtbus`/`drv_common`），配套非缺省布局冒烟用例。
- **PCA9685 退出 SLEEP 按 datasheet**（写 `AI|RESTART`）；**UART** 波特率改 `B*` 常量（旧版从未生效）、termios 快照恢复。
- **新增 `SCSCL` 总线舵机驱动**（飞特 FT-SCS 协议；`set_id` 站号烧写须结构期 `allow_burn` 授权）。
- **virtbus/驱动加固**：读族校验对齐 HAL、先实例化再 attach、`guard_build` 全总线接入、`sensor_daemon` 浮点防线、`bitcompat` 符号扩展原语。

### 工程化与守门

- **大文件拆分**：`robot.cpp` 按类拆四个 TU（只剩 114 行编排门面）；`run_command` 子进程机器独立 `agent_subprocess.*`；`ActionPlan::build` 抽为白盒可测单点。
- **结构重构**：`CliOptions` 类型化、`HwRegistry` 三容器、守护状态机收敛 `DaemonRunner`、`RobotStruct` 拆 Loader/Factory/Runner、守护槽所有权改 `shared_ptr`。
- **守门基建**：HAL 常量编译期自检、CI 三硬门禁 + 五软门禁、`test_curl_consts` 落地、`run_san.sh --asan-only`、agent CLI 负路径守门补齐。
- **重复代码九类收敛单点**（`isSafeIdent`/`makeErrResult` 等；三类经评估刻意不收敛并注明）；巨函数拆分十项。
- **覆盖补齐**：守护并发压测 + TSan、TCP 异常注入七场景、fd 泄漏检测、离线 ABI 回归；文档全量通读排版。
