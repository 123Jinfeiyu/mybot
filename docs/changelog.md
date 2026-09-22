# 更新日志

> 从 [README](../README.md) 迁出，按主题分组记录版本演进要点，组内大致按时间倒序。每条只留「改了什么、对使用者意味着什么」——设计论证、实现落点与守门明细以 docs/ 各文档与 git 提交历史为准。
> 条目里的守门/用例**数字是当时的快照**：权威口径一律以 `bash test/run_all.sh` 与 `./build/mybot_tests` 的实时输出为准（`docs/development.md` 单一事实源约定）。

## v1.x（C++ 移植版）

### 2026-09-22 动作脚本执行预算（PL-2，原 AG-12 子项）

> 依据 [.tmp/IMPLEMENTATION_PLAN_20260922.md](../.tmp/IMPLEMENTATION_PLAN_20260922.md) 批 C。
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

> 依据 [.tmp/IMPLEMENTATION_PLAN_20260922.md](../.tmp/IMPLEMENTATION_PLAN_20260922.md) 批 B。
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

> 依据 [.tmp/IMPLEMENTATION_PLAN_20260922.md](../.tmp/IMPLEMENTATION_PLAN_20260922.md) 批 B。
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

> 依据 [.tmp/IMPLEMENTATION_PLAN_20260922.md](../.tmp/IMPLEMENTATION_PLAN_20260922.md) 批 A。
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

> 依据 [.tmp/IMPLEMENTATION_PLAN_20260922.md](../.tmp/IMPLEMENTATION_PLAN_20260922.md) 批 A。
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

> 依据 [.tmp/IMPLEMENTATION_PLAN_20260922.md](../.tmp/IMPLEMENTATION_PLAN_20260922.md) 批 A。
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

> 依据 [.tmp/NEXT_STEPS_20260922.md](../.tmp/NEXT_STEPS_20260922.md)（核实两轮 agent 重构
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

> 依据 [docs/proposals/agent-refactor-plan-2.md](proposals/agent-refactor-plan-2.md) 阶段 C6（F5）与 C7（F6）。行为保持型 + 死分支清理。

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

> 依据 [docs/proposals/agent-refactor-plan-2.md](proposals/agent-refactor-plan-2.md) 阶段 C5（对应审计 F4）。行为保持型：缺省仍是 stderr。

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

> 依据 [docs/proposals/agent-refactor-plan-2.md](proposals/agent-refactor-plan-2.md) 阶段 B4（对应审计 F3）。新能力，缺省行为不变。

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

> 依据 [docs/proposals/agent-refactor-plan-2.md](proposals/agent-refactor-plan-2.md) 阶段 A1/A2（对应审计 F1/F2）。一处真缺陷修复 + 行为保持型重构。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 B3（对应 R4）。验收判据为 `VmHWM` 实测（`.tmp/memaudit/b3_measure.sh`，AG-19 方法；`.tmp` 不入库）。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 C3（对应 R7）。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 C4（对应 R8，按方案倾向选「改注释」而非改实现）。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 C1（对应 R6）。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 B2（对应 R3）。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 B1（对应 R2）。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md)。

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

> 依据 [docs/proposals/agent-refactor-plan.md](proposals/agent-refactor-plan.md) 阶段 A1。
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
