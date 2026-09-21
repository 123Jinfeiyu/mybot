# 更新日志

> 从 [README](../README.md) 迁出，按主题分组记录版本演进要点，组内大致按时间倒序。每条只留「改了什么、对使用者意味着什么」——设计论证、实现落点与守门明细以 docs/ 各文档与 git 提交历史为准。
> 条目里的守门/用例**数字是当时的快照**：权威口径一律以 `bash test/run_all.sh` 与 `./build/mybot_tests` 的实时输出为准（`docs/development.md` 单一事实源约定）。

## v1.x（C++ 移植版）

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
