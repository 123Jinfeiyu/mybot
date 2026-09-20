# 更新日志

> 从 [README](../README.md) 迁出，按主题分组记录版本演进要点，组内大致按时间倒序。每条只留「改了什么、对使用者意味着什么」——设计论证、实现落点与守门明细以 docs/ 各文档为准，完整叙述见 git 提交历史。
> 条目里出现的守门/用例**数字是当时的快照**（随机器与用例数变化）：权威口径一律以
> `bash test/run_all.sh` 与 `./build/mybot_tests` 的实时输出为准（`docs/development.md` 单一事实源约定）。

## v1.x（C++ 移植版）

### 2026-09-20 审计收口·第二阶段（遗留三处）

> 承接上面审计批次的**继续修复**：核出首轮未收敛处中的三处本阶段收口，另两处（LTC2944 true QLSB、数据手册疑点）仍待人工对照 datasheet。**无使用者可见行为变更**。
> 守门快照：Lua/e2e **251 通过 / 0 失败 / 1 跳过**、C++ 单测 **378 / 0**。

- **kalman 的 Q=±inf 不再产生 NaN**：`kg = P_mid/(P_mid+R)` 在 `Q=inf` 时为 `inf/inf=NaN`，会污染此后整条输出（同期驱动层 `finite_of` 只拦得住输入侧）。现按极限取 `kg=1`（输出即测量值、`P` 回落 `R`），兑现头注释既有语义「Q 无穷 = 只用测量值」；`Q=-inf` 仍被 `Q>=0` 拒绝，filter 内 `-inf` 分支纯属防御。
- **LTC2944 单位与换算单一事实源**：`set_charge_mah` 逆推注释把除数写成 `0.34e-3×1000×M×0.05`（比代码多乘 1000，`0.34e-3` 已是 mAh 形态）。系数改为 `CHARGE_LSB_AH×1000` 并注明 Ah/mAh 两形态勿混用；mAh↔code 往返补离线闭环（±0.5 code 量化容差）。
- **json 数组内 null 声明补全**：`[1,null,3]` 解码为稀疏表，重编码因数字键走不进对象分支而**报错**——头部约定此前只写了字段/顶层 null→nil，补上数组空洞这一有损点并以测试钉住（本项目无「解码→重编码数组」路径，影响面为零）。

> 依据一次独立审计（总评 8.5/10）。本轮主题是**「声称的防线是否真能拦住」**：查出的首要问题不是功能缺失，而是若干防线结构上无法变红。**两条使用者可见的行为变更**单独标注。
> 守门快照：Lua/e2e **250 通过 / 0 失败 / 2 跳过**、C++ 单测 **378 / 0**。

- **门禁闭环（A1/A2/A4/A5）**：
  - **A1 UBSan 硬门禁此前形同虚设**：构建只加 `-fsanitize=address,undefined`，而 UBSan 缺省是 recover 模式——检出未定义行为只打印 `runtime error:`、**退出码仍为 0**，于是 `docs/testing.md` 列为硬门禁的 `gate:sanitizer` 对有符号溢出/错位访问/非法移位类缺陷结构上不可能变红。现补 `-fno-sanitize-recover=undefined`（同一段溢出代码实测：修复前 exit 0，修复后 exit 1）。
  - **A2 四个「常量自检」是自指的**：`test_ioctl_nums` / `test_uart_consts` / `test_v4l2_consts` / `test_curl_consts` 把 Lua 里的数字**在 C 里又手抄一遍**、只与系统头比对，从不读 Lua 源——实测把 `HAL.SPI_IOC_WR_MODE` 改成 `0xDEADBEEF` 后四个目标全绿。现配置期由 `test/gen_const_check.cmake` 从 `HAL.lua` / `Camera.lua` / `llm_http.lua` 纯文本抽取生成 `const_lua.h`，每项双边断言（Lua 值 == 预期值 == 系统头）；抽不到必需常量即配置失败。
  - **A4 守门自检此前只被可选安装的 pre-push 钩子调用**，CI 零引用——「新 C++ 用例没登记进 CMakeLists 就永不编译」「Lua 用例命名不符就永不被收集」在 CI 上无人拦。现 `gate:release` 先跑 `test/run_all_selftest.sh` 再跑全量守门。
  - **A5 `rc==125` 洗白**：守门对**任意** 125 记跳过，于是 `os.exit(125)` 可把用例从 pass/fail 记账里抹掉。现要求同时出现 `device_gate.sh` 的 `[SKIP]` 横幅，否则判失败（agent 端到端段同规）。
- **⚠️ 行为变更：`require` 不再能按模块名加载文件（A3）**。`dofile`/`loadfile` 被白名单包装后，`require` 仍走 `package.loaders` 里的 Lua 文件搜索器（C 层直接 `luaL_loadfile`），实测能从模块树外加载任意 `.lua`——与「只能加载模块树内的文件」的文档承诺矛盾。现保留 preload 搜索器（驱动仅有的两个真实 `require`——`ffi`/`bit`——都靠它，与文件系统无关；宿主另把 `mpath` 注册进 `package.preload`），把 Lua 文件搜索器与 C 搜索器换成返回说明性错误串的桩，`package.loadlib` 置 `nil`、`package.path`/`cpath` 清空。驱动要用文件仍走 `mpath.resolve` + `dofile`；违反者会看到明确的沙箱报错。
- **⚠️ 行为变更：非有限值（NaN/±inf）不再被静默接受**（审计「非有限值类」）。Lua 里只有 `nil`/`false` 为假，而 NaN 与任何数比较恒为 false，于是 `x < 0` / `x > 180` 这类区间判定对 NaN 全部放行。逐点收口：`GPIO.call` 的 `value="nan"` 此前会把线**驱动到高电平**并返回 OK（绕开 HAL 的 NaN→0 fail-safe 缺省）；`MAX30102`/`LPS22HB`/`AS7262`/`MCP3421`/`BME688` 的 `wait`/`timeout` 非有限会让轮询**无界挂死**（实测 `timeout 10s` → exit 124，无 ERR、pcall 救不回）；`Servo` 的 0–180 执行器保护钳位、`Timer` 的延时、`PWM`/`HAL.pwm_config` 的频率与占空比同族。判据统一走既有的 `finite_of`（`bus_contract` 单点），并在全仓首次补上非有限值的离线覆盖（`test_servo_timer_nan.lua` 等）。
- **守护字段数超槽宽改为加载期报错**：共享槽只有 `values[32]`，而 `fields` 此前不校验数量、Lua 侧 `publish` 静默钳制、C++ 侧 `readSample` 按 32 截断——33+ 字段的 `@Daemon` 能通过全部加载期校验并「正常」运行，多余字段永远拿不到值，事后只表现为下游「某字段缺失」。现在加载期直接拒绝并指出槽宽上限。
- **守护退出路径的资源释放**：`daemon_cleanup` 此前**裸调** `destroy()`——驱动去初始化抛异常时 `close_bus()` 不再执行，总线 fd 泄漏到进程退出（宿主只 `pcall` cleanup 本身随即 `lua_close`，HAL 无 `ffi.gc` 兜底）。现以 `pcall` 包裹并注记失败原因，无条件释放总线。
- **文档失实收口**：`architecture_analysis.html` 的「`@Daemon` 现有 **16** 值上限」（同页另两处写 32，且 `SLOT_MAXVALS=32`）改为 32；CI 描述「ASan/UBSan **双跑**、另设 TSan 与 clang 软门禁」改为「单跑 + 五条软门禁」；`main.cpp`/`robot.cpp` 的行数与职责（CLI 已下沉 `cliargs.cpp`）按现行源码更正；两张 SVG 的 main.cpp/robot.cpp 归属同步；`docs/testing.md` 的 `--tcp-host` 非法地址退出码由「2」更正为实测的 **1**，并说明五条软门禁的转硬条件并非统一。

### 2026-09-20 评估批次（安全缺省翻转 / C2 同族收口 / 门禁硬化）

> 依据一次独立评估（总评 8.5/10，六条扣分点全量落地）。**三条使用者可见的行为变更**单独标注。
> 守门快照：Lua/e2e **248 通过 / 0 失败 / 1 跳过**、C++ 单测 **376 / 0**。

- **⚠️ 破坏性变更：`run_command` 缺省不再提供**（评估 C12 收口）。任意 shell 执行是 agent 手里最大的一张牌，缺省给上等于把「无审批 RCE」当默认值。现在须 `--agent-shell`（或配置 `allow_shell: true`）显式开启，`--agent-no-shell` 随之移除；`run_command` 缺席时模型调用它会收到「未知工具」文本，会话不中断。
- **⚠️ 破坏性变更：移除 `--llm-key`**。命令行传密钥必然出现在 `/proc/<pid>/cmdline`（同用户进程可读），与「密钥不进 argv」的自证口径自相矛盾。密钥只从环境变量链（`BOT4CJ_LLM_KEY` → `ROBOT_AGENT_API_KEY` → `DEEPSEEK_API_KEY`）或配置文件（`api_key` / `api_key_file`）取；残留用法按未知参数退出 2，不会静默忽略。
- **⚠️ 行为变更：SQL 词法层把括号当分隔符**（与空白无关）。此前 `(` 只在「token 起始」有分支、`)` 只在「token 非空」有分支，`INSERT INTO T(a,b) VALUES(1,2)` 这类紧凑写法会把 `(` 并进名字并以 `Token ,` 报错；现在紧凑与留白两种写法产出逐字节相同的词元流（仓内夹具一直写留白，故缺陷从未暴露）。
- **C2 同族收口（NaN 防线扫尾）**：`gpio_set_value` 的 `tonumber(v) or 0` + `(v ~= 0)` 会把 NaN 判为真、把线**驱动到高电平**（与已加固的 `default_values` 同一判据两份实现）；`Ema`/`Kalman`/`BH1750` 的列解析同款写法，配合库内「区间比较对 NaN 恒为假」的校验，让 `alpha`/`Q`/`R`/`mtreg` 写 `"nan"` 时一路发布 NaN 却返回 `result="OK"`。现统一回落缺省（`drv_common` 单点，并新导出 `finite_of` 供样本列使用），库层校验改为「必须满足上下界」的正向式，样本列非有限按 `ERR` 契约处理。
- **真机/仿真判据压回总线契约单点**：SPI 的 `mode`/`bits`/`speed` 与字节参数，真机路径此前 `uint_of` **静默 floor**、虚拟路径显式拒绝小数（`spi_write(fd,1.9)` 真机成功、`proto=virt` 失败）；`uart_read` 的非有限超时反之（真机拒绝、仿真放行）。现下沉 `bus_contract.int_of`（严格整数 + 区间）双向统一，`uart_read` 超时两库同走 `finite_of`。
- **平台单点修复**：`HAL.PWM_SYSFS_ROOT` 曾被 PWM 段的字面量重新赋值，架空了 `HAL.platform` 表的「改这里即移植」承诺；现 `pwm_chip_path`/`pwm_attr_path` 直读平台表。
- **CI 门禁硬化**：`gate:clang-tidy` 此前退出码取自管道末端的 `tail`（恒 0，**结构上不可能红**），现改为记录 clang-tidy 自身退出码并以其决定成败，`clang-tidy.log` 入 artifacts（「基线留档」终于有落点）；`gate:tsan` 的 `halt_on_error` 改 1（竞态报告现在真的影响退出码）；三条软门禁补「连续 2 轮无新增告警 → 移除 allow_failure，最迟 2026-10-31」的可判定转硬条件；新增 `gate:coverage`（gcov 覆盖率报告入 artifacts）与 `gate:musl`（alpine 上跑常量自检与 C++ 单测，把「非 glibc libc 未验证」从声明推进到实证）两条软门禁。
- **文档失实与完整性收口**：`architecture_analysis.html` 的守门数字停在两轮前（235/327）且断言「CI 缺 clang-tidy 静态分析门禁」——而该门禁当天已上线，现同步为 248/376 并改述软门禁现状，另加「本页数字为当时快照」的口径；`reference.md` 补上遗漏的 `--prompt-file` 与三个环境变量（`BOT4CJ_HAL_BACKEND`/`BOT4CJ_CAMERA_SRC_DIR`/`BOT4CJ_TEST_VIDEO`），并修掉指向已搬迁代码的 `src/main.cpp:parseArgs`。
- **守门自身补盲**：「帮助正文 ↔ docs 选项表」的黄金测试此前取 `reference.md ∪ agent.md` 的**并集**，于是 `reference.md` 单文档缺项永远不会红——现改为逐文档判据（帮助里的每个选项都必须在 `reference.md` 有记录）；并新增环境变量完整性的机械判据（源码/脚本里出现的 `BOT4CJ_*` 环境变量必须在 `reference.md` 有记录，`__BOT4CJ_*` 守卫全局按前缀排除）。

### 2026-09-20 审计修复批次（C1–C11）

> 依据一次独立审计（`.tmp/audit-report-2026-09-20.md`：14 项缺陷，每项均本机复现或代码核对）。
> 本批修复 C1–C11；`allow_shell` 缺省翻转（C12）与未复核的器件寄存器图（C13）不在本批。
> 四条使用者可见的行为变更单独标注。守门快照：Lua/e2e **245 通过 / 0 失败 / 2 跳过**、
> C++ 单测 **371 / 0**（基线 241 + 356）。

- **⚠️ 崩溃修复：Lua 驱动一次返回多个值时宿主段错误**。契约表是**首个**返回值，但旧实现 `lua_pushnil` 之后 `lua_next(L, -2)` 落在**最后一个**返回值上——末值是标量时 `lua_next` 对非表调用直接 SIGSEGV（Release exit 139，ASan 回溯 `luabridge.cpp:253`），末值是表时静默返回最后一个表、首个表的字段全丢且退出码 0。现先把首个返回值拷到栈顶再遍历，多值只记一条 logD 提示。
- **NaN 不再穿透数值校验**。`math.floor(NaN)` 是 NaN，而 `NaN < 0` / `NaN > 255` 均为 false —— 旧 `byte_of`/`uint_of` 把 NaN 当**合法值**返回，一路走到 `ffi.new("uint8_t", NaN)` 变成 0x00：`i2c_write_reg(fd, reg, NaN)` 返回成功但线上写的是 0，PCA9685 把 NaN 角度写成 ON=0/OFF=0（无脉冲）却报 `result="OK"`。收敛出 `bus_contract.finite_of` 单点（tonumber + 非有限拒绝），并修同族的 `drv_common.int_of/num_or`（非有限回落缺省，不再参与钳制）、`bitcompat` 位运算（LuaJIT `bit.band(NaN,0xFF)=0`，现对非有限入参报错；有限入参逐字透传，含既有 1.5→1 截断语义）、HAL 的 GPIO 默认电平与 `uart_read` 超时。
- **⚠️ 行为变更：INSERT 按列名落位**。值此前按 INSERT 自带列序逐位追加，而类型校验按列名配对——两者顺序不一致时**静默错列**，且 `Number` 列的解析期校验形同虚设（`INSERT INTO Timer (delay, name) VALUES (50, 'timer-0')` 让声明 Number 的 `delay` 拿到字符串 `'timer-0'`）。现在值按列名落到建表声明序；列名未知 / 同列写两次 / 漏列一律解析期报错（仓库内 138 个 `.sql` 的列序与声明序逐一相同，既有脚本零变化）。
- **矩阵单元格报错不再回显整条原文**。`cannot parse matrix cell '<原文>'` 此前无截断：20 万字节单元格实测灌 200KB 进 stderr，且 ANSI 转义直写操作者终端（`--tcp` 远端脚本可达）。现与表达式报错共用 `util::echoSource`（不可打印字节转 `\xNN` + 截断 80 字符 + 省略标记）——表达式路径早有这条防线，矩阵单元格漏了同一条。
- **MAX30102 FIFO_CONFIG 位域修正**。数据手册是 `SMP_AVE[7:5] | ROLLOVER_EN[4] | A_FULL[3:0]`，旧实现按 `[6:4]/[3]` 写 `code*16+0x08`：芯片读到的平均档位是**一半**、rollover **实际关闭**（与驱动注释/build 声称的「rollover 开」相反）。内嵌模型复刻了同一处错位，单测因此一直假绿；现驱动与模型共用位域常量，测试按真实布局断言（build 写入值 0x18/0x98）。
- **Camera 打开失败不再泄漏 fd/mmap**。`state.fd` 在任何校验之前就赋值，而十条失败路径全部直接返回、不释放——fd 与已 mmap 的缓冲区泄漏，且 fd 停在 ≥0 使「半开」状态被后续调用误判为已打开（`state.path` 只在成功路径补写）。现失败统一走 `open_fail`（先释放再返回），`state.path` 由 `device_close` 随 fd 一起清。
- **virtbus 采纳 SPI fd 不再误登记成 I2C**。`adopt_fd` 此前把「除 uart 外的一切」都写进 `open_paths/fd_addr`，`spi_paths[fd]` 永远为空——该 fd 上每次 `spi_*` 调用返回 -1（"device absent"），而 txlog 已记下成功的 adopt。
- **守门自纠（四条）**：错误过滤器不再假绿（`run_all.sh zzz_typo` 曾报「3 通过, 0 失败」退出 0 —— controller/trigger 观察面缺 `$filter` 守卫，把末尾的空匹配防线整条废掉）；零用例的 C++ 二进制在无过滤器下判**失败**而非静默 SKIP；`mybot_tests` 退出码改 0/1（旧为失败用例数，256 个失败回绕成 0，使 `ctest -R cpp_unit` 报 100% passed）；孤儿夹具检测由**子串**改**整字段精确匹配**——实测由此暴露 **26 对真机夹具（52 个文件）从未被执行却报「已登记」**，现如实进 `orphan_whitelist`，顺带消掉 `sed | grep -q` 的 196 行断管噪声。另修 `BOT4CJ_TEST_VIDEO` 逃生阀不可达（`device_gate.sh` 现在认这个变量，与测试体口径一致）。

### 2026-09-20 重构批次（CLI 可测化 / 卫生与缺陷收口）

> 依据 `.tmp/refactoring-plan-2026-09-20-r2.md` 的 M0 + M1；按「先补判别性用例、再改实现」推进。
> 其中两条是**使用者可见的行为变更**（词法报错文案、`run_command` 环境清洗语义），单独在下方标明。

- **CLI 层可测化：`parseArgs` / `validateOptions` / 帮助文本迁 `src/cliargs.*`**：三者原先定义在 `main.cpp`，而 `main.cpp` 不在 `MYBOT_CORE_SOURCES` 中——`mybot_tests` 链接不到，`test/` 对它们的引用为 0，6 个校验分支只有 2 个被黑盒覆盖，`--help` 更是**零守门**。现在 `mybot` 与 `mybot_tests` 共用同一份实现；`--help` 输出**逐字节不变**（与迁移前 4,314 字节实测比对一致）。新增 11 条 C++ 单测 + 1 条 hardening 用例：退出码 2 的五条拒绝路径、裸 `--tcp` 不回落端口、`--help` 优先级，以及**帮助正文与 `docs/` 选项表的双向一致性**（帮助里出现未记录的选项、或文档记录了帮助没有的选项，都会立刻红）。
- **守门自检 `test/run_all_selftest.sh`（C7 起步）**：`bash -n` 守门脚本语法 + 核对每个 `test/cpp/*.cpp` 已登记进 `CMakeLists.txt` + 核对 `test/*.lua` 命名匹配 `test_*.lua`。「用例没被收集 / 没被编译」这类**静默消失**此前没有任何防线（改名或漏登记只会让覆盖悄悄变薄，守门照绿）。自检**不进入** `run_all.sh` 的记账（守门汇总口径不变），由 pre-push 在整段守门之前调用——钩子由此从两件事变三件。
- **⚠️ 报错文案变更：词法层 NAME 词元加空格**——`Token name'x'` → `Token name 'x'`。旧文案把类型名与名字粘在一起，日志里难以辨读；影响面仅为以该子串做匹配的脚本（仓库内 5 处断言已同步）。
- **⚠️ 行为变更：`run_command` 的环境清洗改「整词」判定**——旧实现按**子串**剔除名字含 `key`/`token`/`secret`/`password`/`passwd`/`credential` 的变量，既误伤 `KEYSTONE`/`MONKEY`/`HOCKEY`/`TOKENIZER`（与密钥无关的普通变量会被无声吞掉），语义也不精确。现在按非字母数字分隔切词匹配，并**显式列举**无分隔符的已知复合密钥名（`apikey`/`accesskey`/`secretkey`/`clientsecret`/`privatekey`/`authkey`/`authtoken`/`accesstoken`/`refreshtoken`/`pgpassword`/`passphrase`）——`BOT4CJ_LLM_KEY`、`APIKEY`、`PGPASSWORD` 等仍被剔除，防线不因去误伤而变薄。
- **`injectLuaDriver` 顺序守卫**：`MetaProvider` 持有的 `matrix_` 是对矩阵表的**引用**，`active()` 之后再注入会因 `push_back` 重分配而悬垂——此前只靠「必须在上电前注入」的约定维系，现在显式报错。
- **纵深防线与小缺陷**：`@Vars` 字面量解析补 `end`/`isfinite` 复核（旧实现丢掉 `strtod` 的 `end`，畸形字面量会被静默当成数值）；`ensureOk` 的 JSON 种类名下标加 `static_assert` + 边界检查（越界退化为 `unknown` 而非 UB）；prompt 组装读脚本头部时的 I/O 错误不再与「正常截断」混为一谈；`mtmerge` 两处 `std::exit(2)` 改 `return 2`（不跳过局部对象析构）；`daemonvar.cpp` 补 `<algorithm>`（原先靠 libstdc++ 传递包含才编过）。
- **卫生**：`.zcode/` 入 `.gitignore`；`modules/lib/fft.lua` 两处死赋值清理（保留 `check_pow2` 的校验副作用）；`Camera.lua` 头注与实现对齐（仿真来源路径失败**刻意**返回 ERR，真实设备路径不返回）；changelog 引导行计数 63→64 处修正，并声明**条目内守门数字为当时快照**（权威口径以脚本输出为准）。
- **流程**：`docs/development.md` 新增「委派与自动化」节——委派给子代理/脚本的工作必须声明只读、前后各跑一次 `git status --porcelain` 留档。背景是本次并行审计中一个子代理在「只在 `.tmp/` 写入」的前提下删除了 17 个已跟踪文件（已从 HEAD 恢复并核对数量）。

### 2026-09-20 重构批次（会话作用域 / 模块路径 / I/O 语义）

> 本轮起于一次独立评估（见 `.tmp/shortcomings-2026-09-20.md` 与
> `.tmp/refactoring-plan-2026-09-20.md`），按「先补判别性用例、再改实现」推进。
> 只有一条**破坏性语义变更**（下面第一条），其余为缺陷修复与收敛。

- **⚠️ 破坏性变更：动作脚本内联 `@Alias`/`@Vars` 的作用域收敛到「本次执行」**：此前它们是进程级的——与结构变量同名会**永久遮蔽**结构定义（直到进程退出），且 agent 长驻模式下跨脚本残留（脚本 A 定义的别名对脚本 B 可见）并持续占用内存；更棘手的是注册发生在派发之前，**一个连 `@Action` 都没有、以报错收场的脚本也已完成注册表改写**。现在会话覆盖层由 `HwRegistry::SessionScope` 在一次 `run()` 内持有，出口（含异常路径）整体回滚，结构定义与结构脚本的绑定一字不动。**受影响用法**：依赖「前一个动作脚本定义的别名给后一个脚本用」的工作流须把该别名写进结构脚本（`.sql` 的 `@Alias` 表）或用 `@Include` 主从覆盖表达；单次执行内的行为（`@Include` 归并别名、设备列别名解析、`$(...)` 引用）完全不变。附带修复：agent 长驻反复执行脚本不再无界增长（实测 200 轮后结构名字索引不增长）
- **多路径模块加载对全部模块成立**：`BOT4CJ_MODULE_PATH` 支持 `:` 分隔多路径是文档既有承诺，但**驱动侧只取首段**（59 个 `modules/*.lua` 中 58 个带引导行、外加 5 个 `lib/` 文件，共 64 处逐字相同的 `:match("[^:]+")`，分布在 63 个文件——`LPS22HB.lua` 带两处），首段不含 `lib/` 时加载即失败——该承诺此前只对宿主侧成立。现在宿主在建沙箱时逐段探测 `lib/mpath.lua` 并注入 `__mpath`，模块侧统一写 `local mpath = __mpath or <单根回退>`；回退分支只在**脱离宿主**直跑（`luajit test/*.lua`、`scscl_scan.lua`）时走到，那些场景单根即正确。新增 grep lint 钉住这一形态，判别性用例跑真二进制（hardening 段 `multipath_module_path`）
- **`@Daemon`/lib 定位收敛到 `util::findExistingModule`**：宿主侧逐段探测此前有两份实现，现与 Lua 侧 `mpath.resolve` 同规则
- **`run_command` 把 EINTR 当正常重试而非终态**：信号处理器未装 `SA_RESTART`，EINTR 在本项目是常态；旧实现把它当 EOF——丢弃子进程剩余输出、提前关闭读端（给仍在跑的子进程送 SIGPIPE）；`waitpid` 的 EINTR 被当「已无子进程」留下僵尸；`fcntl` 失败路径还会把已 `fork` 的子进程丢成孤儿。三处收敛为 `util::retryEintr` + `reapWithGrace`
- **TCP 未认证对端不再能终止进程**：口令校验把 `SO_RCVTIMEO` 到期的 `EAGAIN` 与「凭据不匹配」混为一谈，连满 5 次即抛错退出——任意对端连上 5 次、每次不发字节（每次挂满 `--tcp-idle`）就能终止进程，且诊断误报为「口令连续 5 次校验失败（可能为扫描/爆破）」。现在校验返回三态，只有「收到凭据但不匹配/超长」才计入爆破上限；「没给凭据」（超时/关闭/中断）关连接后继续等待
- **超限文件一律报「超过字节上限」，不再伪装成语法错误**：配置与密钥文件的整文件读取读满上限即静默截断，随后报「不是合法 JSON」「密钥错误」——真实成因（文件过大）完全丢失。四个实现收敛为 `util::readWholeFileLimited`（提示词包同语义，报错锚点不变）
- **表达式语法错误不再回显整条原文**：原文引用截断到 80 字符并加省略标记。此前单条报错可把动作脚本上限（8 MiB）量级的**用户可控字节**灌进 stderr，并可携带 ANSI 控制序列直写操作者终端（实测 1 万层括号 → 20KB）
- **协议驱动纳入 `arg_table` 契约**：`LlmOpenAI`/`LlmDeepSeek` 的 `call(nil)` 会裸抛 Lua error 而非返回 ERR 契约表（由新增的驱动契约 sweep 拓出）。至此全仓只有 `Camera` 一个声明豁免
- **新增驱动入口契约 sweep**：`test/test_driver_contract.lua` 逐个真的加载全部 `modules/*.lua`，断言可加载 + `build`/`call`/`destroy` 齐全 + 非表入参返 ERR 契约（349 断言）。grep lint 保留为快线，sweep 为全面线
- **守门改进**：真机用例跳过判定收敛为 `test/lib/device_gate.sh` 单点（`run_all.sh` 与 ctest 共用）——此前 ctest 侧按**文件名**特判，`test_camera_v4l2.lua` 在无摄像头机器上报 Failed 而非 Skipped；CI 构建步改带 `-DMYBOT_WERROR=ON`，「零告警」从约定升级为门禁（本地缺省仍 OFF，避免未预跑的编译器版本被挡住）

### agent 模式（`--agent`）

- **内置缺省端点使用即注记**（2026-09-19 外部评估）：CLI/配置文件/环境链都没配 endpoint 时，报文流向驱动内置缺省的第三方网关——每次进程**首次**这样回退时向 stderr 注记一行（不打印密钥与报文内容，显式配置不注记），让「什么都没配就跑起来」的会话可见自己的数据流向
- **Camera 入参类型守卫**（2026-09-19 外部评估）：Camera 是全仓唯一不挂 `drv_common.arg_table` 样板的驱动，非表入参原本会裸抛 Lua error 而非 ERR 契约表——build/call 入口内联同一条类型校验
- **提示词包**（`--prompt-file` / 配置文件 `prompt_files`）：一组 Markdown（机器人身份、行为纪律、能力约束）拼接后**追加在系统提示词内置段落之后**，把通用 agent 绑成具体机器人的「大脑」（示例包 `prompts/spider_robot/`）；目录收集全部 `.md` 按文件名自然排序，多份以 `## 文件名` 分节。在配置组装期加载——路径写错在结构装载与上电之前退出 2（「配置错误不碰真机」）；单文件 1 MiB 上限，超限拒绝而非静默截断
- **REPL 历史按字节预算自动裁剪**：缺省 4 MiB（0 = 关），超过后在下一轮任务前按「任务块」整块裁掉最老的历史——整块裁剪保证 assistant 的 tool_calls 与其 tool 消息不被拆散（半截序列会被服务端 400 拒绝）；老块连同其中的 base64 图片一并释放（图片是历史体积主项）；system 提示词与最新任务无条件保留
- **新增 agent 模式**：`--agent` 让大模型工具循环成为主执行流程——结构脚本加载后硬件**常驻内存**，多轮挑选并执行动作脚本，会话结束才 `deactivate()`；与 `--tcp`/`--input` 互斥（同给退出 2）。`--task` 一次性任务，缺省交互 REPL（`/hardware`、`/scripts`、`/exit`，历史跨轮保留）
- **工具集**：`list_action_scripts`、`run_action_script`（路径限定 `--scripts-dir` 之下，拒绝绝对路径/`..`/软链/非 `.mt`，8 MiB 上限）、`list_hardware`（只读清单，不调 `read()`/`play()`，不误触发执行器）、`capture_image`（见下）、`run_command`（超时整进程组终止 + 64KB 截断 + 环境变量清洗，`--agent-no-shell` 可关闭）。工具报错折成文本回传模型，不打断会话
- **模型调用是一颗 Lua 协议驱动**：与舵机、传感器平级的 `modules/*.lua` 驱动，宿主经 `src/agentdriver.*` 适配——换模型/换厂商 = 换一个 Lua 文件，恢复零链接期依赖（仅 LuaJIT + pthread + dl；运行期经 ffi 加载 libcurl）。配套 `src/agentproto.*`（消息⇄Json）、`src/agentloop.*`（轮次上限/回滚/extras 透传）、`src/jsonparse.*`（严格 JSON）与 Lua 侧 `lib/json.lua`、`lib/llm_http.lua`（ffi 直驱 libcurl；整型选项必须 cast 成定签名再调，裸变参被静默忽略——实测超时失效）、`lib/llm_wire_*.lua`（协议整形）
- **llm_http 写回调即用即释**：每请求的回调 cdata 在完成后 `cb:free()`——LuaJIT 回调槽位要等 GC 才归还，长驻会话高频请求会持续累积
- **LLM 超时钳制**：`lib/llm_http.clamp_timeout` 把 `timeout`/`connect_timeout` 单点钳到 **[1, 600] 秒**——libcurl 语义下 `0` 是**不限时**，一个配置项就能让单次调用永远挂着；`--llm-timeout` 与配置 `timeout` 的上限同步
- **两个内置协议驱动**：`LlmOpenAI`（缺省）与 `LlmDeepSeek`，`--llm-driver` 切换。后者承担三处实质差异：思考模式开关、**带 `tools` 时必须回传历史 `reasoning_content`（否则 API 400）**、思考模式下的采样参数限制。API key 由驱动自己读环境变量，不进参数表/调试转储
- **配置文件（`--llm-config`）**：URL/API key/模型/采样与 agent 参数收进一份 JSON。优先级**命令行 > 环境变量 > 配置文件 > 驱动缺省**；查找 `--llm-config` → `BOT4CJ_LLM_CONFIG` → `./mybot.llm.json` → … → `$HOME/.config/mybot/llm.json`，实际用了哪份会话开始时打印。严格校验：非法 JSON/类型范围不符/未知字段一律退出 2（空串按未配置）；密钥可写 `api_key_file`，对同组/其他用户可读时打印 `chmod 600` 提醒；`none` 显式关闭（缺省查找依赖 CWD，CI/复现问题建议显式指定）。选 JSON 不选 Lua：配置是数据不是代码，不让一份常被复制分发的文件获得任意代码执行能力
- **空数组哨兵**：Lua `{}` 分不清空数组与空对象，工具 schema 的 `required` 被编成 `{}` 会被 DeepSeek 严格校验直接 400——`lib/json.lua` 提供显式哨兵 `json.empty_array`；假模型服务同步校验工具 schema，此类问题离线守门即可暴露
- **摄像头工具 `capture_image`**：摄像头同为一颗 Lua 驱动（V4L2 mmap + 硬件编码 MJPEG，宿主不链采集/编码库）——build 不碰设备（没接摄像头也要能上电）、首拍惰性打开、首帧丢弃、热插拔失败即释放下次重开；仅支持 MJPEG。图片不塞工具结果（tool 消息只允许文本），在 tool 消息回传完后另起带 `image_url` 的 `user` 消息注入，随历史保留。`--camera-src <图片>` 仿真让无摄像头开发机回归全链路。**路径白名单**：设备只放行 `/dev/videoN`，仿真图片只读 `--camera-src` 所在目录之内的文件（判定走 `realpath`，软链展开后越界即拒）——否则驱动经 `ffi.C.fopen` 就等价于一个「读任意路径」的原语
- **MJPEG 帧完整性重取**：UVC 设备可能吐出熵编码中途截断的帧（实测某设备约半数；内核不置 `V4L2_BUF_FLAG_ERROR`，宽松解码器照常出图），模型网关的严格解析器直接 400。`lib/mjpeg_frame.lua` 以「帧尾 EOI」为判据做规范化，坏帧整帧丢弃重取（上限 8 帧）
- **守门**：agent 端到端六个场景（完整工具循环抓包断言 OpenAI 形态、DeepSeek 协议差异、超时 4s 内生效、配置文件取值、拍照 data URL、提示词包进 system 消息），缺 python3 或 libcurl 按 SKIP 计入未执行清单——场景明细见 [agent.md](agent.md) 测试节

### 语言与 DSL

- **`$(...)` 类型分派表达式：别名入式与文本拼接**（2026-09-20 设计评估）：`+` 出现文本操作数（`@Alias`）即拼接、全数值（`@Vars`）仍是算术——`$(pfx+n)` 把别名值 `"relay-"` 与变量真值 `2` 拼成 `"relay-2"`（数值真值按 `%.15g` 渲染，`2e3` → `"2000"`）；其余运算符对文本显式报错。落点扩展到 `@Action` 全部单元格（求值都在派发期，改绑后真值以运行时为准）：**数据格**产物为文本即文本参数（与 SQL 元数据列同型，驱动侧 `tonumber` 校验不变）、**列头设备位**产物按裸名同规则解析（`$(p)` ≡ `p`、命中 `@Vars` 显式拒绝、未知名穿透）、**列头参数位**产物即参数名（不再解析）。`@Vars` 值与元数据列维持只收数值；`$(...)` 内标识符严格引用（拼接文本只能来自别名），设备位产物只再解一层、不递归求值（环无从建立）。既有合法脚本零变化（别名入表达式原本即报错，现在多出一条合法通路）。**不引入裸 `+` 语法**——运算/拼接只发生在 `$(...)` 之内，算术笔误不会静默变拼接。守门：C++ 单测 12 条（expr 值语义 4 + 派发落点 8，315→327）+ e2e 5 条（`dev_expr`：设备位拼接落 relay-2、参数位拼接、数据格文本参数回显、`-` 对文本报错、未定义名）+ dsl.md 表达式节重写与设计边界
- **`@Action` 设备列别名解析**（设备绑定通道，2026-09-19 设计评估）：列头设备位允许写 `@Alias`，派发前解析穿透到别名值命名的驱动/守护（别名是名字绑定而非播放目标，与参数位替换同一语义；一层为止；`@Vars` 是数值常量显式拒绝；未知真名原样穿透、派发期按行号报错）。用途是片段复用：模板设备列写别名 + 主脚本同名 `@Alias` 改绑，模板一字不动换目标设备。撞设备两层防线：归并期按列头文本、派发期按解析后 (设备, 参数) 查重——三种碰撞形状全部显式报错。**唯一的动作语义变更**：手写重复列由静默逐行覆盖改为报错。`--tcp` 安全面零变化（远端脚本本就可直接写真名）；mtmerge 保持纯离线
- **`@Include` v2：嵌套 + include-once + 主从覆盖**：片段可再包含片段（终止性由「已展开文件表」保证，每文件至多展开一次）；`*.once.mt` 被再次包含时跳过（菱形复用就此可表达），普通 `.mt` 重复包含仍报错（抓笔误）；`@Alias`/`@Vars` 同名行「更靠近主脚本的定义获胜」（主脚本 > 片段 > 子片段），片段互相之间同名仍报错。片段参数化两条通道由此组成：值通道（`$(...)` 引用 `@Vars` 旋钮）+ 设备绑定通道（设备列 `@Alias` 改绑，见上条）。片段 `@Include` 以自身目录为基准（库可整体搬移）；`@Action` 设备列不参与覆盖，设备集相交依旧报错
- **`@Include` 内联归并（v1）**：mtmerge 离线合成器的运行端等价物——主脚本用一张 `@Include` 矩阵声明要并入的 `.mt`，解析后**就地展开**，归并内核复用 `mtmerge_core`（设备集相交/同名行/相位补 `-` 与离线工具同源不漂移）；主脚本不含 `@Include` 时行为逐字不变。信任边界如实声明：`--tcp` 下发脚本由此首次可触达本地文件，防线 = 路径词法 + 行数/字节硬限 + 校验先于任何 IO + 文件内容不进报错。此前未知矩阵被**静默忽略**——本特性同时消灭了该静默坑
- **controller/trigger 多列（扇出）**：编号列 `controller2`/`trigger2`… 放行，一行可声明多个转发目标/触发器，按列声明序执行。controller 扇出广播同一份 payload（同列族不接力）、**出现一个 ERR 即停止**（后续不转发、整帧 halt），全部成功返回最后一个的结果；trigger 按序前置调用、结果各自按名到位
- **`trigger` 前置调用列**：controller 的镜像入边——当前零件被派发前，框架先以空参数调用 `trigger` 列指向的硬件变量（一次语义完整的 play），返回表经契约检查后以**触发器名字为键的子表**并入调用参数，再运行自己的 `call`。与 controller 共用「先声明」约束（引用图按构造无环）；只挂 play 路径，`read`/`build`/`destroy` 不触发
- **`$(...)` 算术表达式**：`@Vars.value`、结构脚本元数据列、`@Action` 单元格三处可用（`+ - * / %`、括号、一元正负，标识符仅限 `@Vars`）。独立递归下降解析器：语法错误带位置、括号深度/运算符数双上限、除零报错、拒绝 inf/nan；惰性求值支持前向引用与成环检测
- **`@Vars` 命名数值变量**：与 `@Alias` 同构（列 `name`/`value`）但仅数值。两种消费：结构脚本元数据列 `$<name>`（唯一允许 `$` 指向标量的类型）、动作脚本参数位替换；`.mt` 内联不得覆盖结构定义
- **结构脚本 `Number` 列类型**：解析期一次数值校验（拒绝空值/`nan`/`inf`），值仍按文本驻留
- **词法器**：引号内 `--`、`/* */`、`;`、`,` 一律按字面数据；引号态增量维护，消除逐字符重扫的 O(n²)；`.sql` 与 `.mt` 统一 8 MiB 上限
- **mtmerge 归并 `@Alias`/`@Vars`**：多 .mt 合成时按行叠加，两侧列头须一致，同名行报错退出 2

### 执行引擎与安全

- **守护槽地址注入改 `lightuserdata`**（2026-09-20 外部评估「裸地址注入」收口）：宿主从 `lua_pushinteger` 改 `lua_pushlightuserdata` 注入槽地址——lightuserdata 在 Lua 域**无算术、无 `tonumber`**，「地址被 double 化后在 ≥2^53 静默丢位」整类风险在语言层面消除，不再靠「禁止对地址做算术」的契约注释兜底。`slot_attach` 改三分支接收（lightuserdata / ffi 指针 / 非负整数兜底，整数路径的 ≥2^53 告警与守卫保留——纯 Lua 场景仍需要），三个守护 chunk 的 `P.slot_attach(DAEMON_SLOT_ADDR)` 透传写法零改动。注入收敛为 `injectDaemonGlobals` 单点（原先 `body` 与 setup 两处重复注入同一批全局）。守门：`test_pthread.lua` 补 cdata 指针附着路径断言，lightuserdata 真注入路径由守护 e2e 覆盖
- **INSERT 列数不匹配报错升级**（2026-09-20）：报错带表名与两侧列数（`Insert into table 'T' has 1 column(s) but 2 value(s).`）——此前的「Insert params」两个字符无法定位拼写/漏列位置（fix-plan 3-4）
- **run_command 子进程卫生**（2026-09-20）：stdin 重定向到 `/dev/null`——子命令不再与 agent REPL 抢终端输入；管道 `fcntl(O_NONBLOCK)` 失败即 fatal（对齐全仓 fail-closed 口径，静默跳过会让输出收集循环阻塞挂死会话）（fix-plan 3-1）
- **API key 对 agent 的 shell 可见性告警**（2026-09-19）：环境变量清洗只覆盖「环境变量」这一来源——`run_command` 的子进程与宿主**同属一个用户**，密钥若来自配置文件 `api_key`、密钥文件 `api_key_file` 或 `--llm-key`，脚本一句 `cat` 就能拿走（`chmod 600` 对同用户无效）。命中上述来源即启动时告警并给规避路径（改用环境变量，或 `--agent-no-shell`）；提示只讲来源、**绝不复述密钥本身**
- **工具脚本路径沙箱硬化**（2026-09-19 审计）：`readFileLimited` 改用 `open(O_NOFOLLOW)`——`scripts/` 里的软链不再能把目录外文件当动作脚本执行；软链条目一律不进工具清单（此前目录外文件的**首注释会经清单回显进模型上下文**）
- **浮点窄化值域防线**（2026-09-19 评估）：模型可控数值与配置文件数值在 `static_cast` 前先判有限性与值域——`1e30` 这类值直接转换是 float-cast-overflow UB（UBSan 实证），现一律响亮拒绝
- **TCP 动作源加固**：非回环监听强制口令 + 恒时比较 + 先取脚本后上电；凭据行超长 fail-closed 拒绝；裸 `--tcp` 不再静默回落端口 1；口令失败只关连接回 accept、连续 5 次才退出（防单次连接 DoS 与无限爆破）；新增连接总时长预算（随 `--tcp-idle` 放大）堵慢滴发送挂住进程；口令可经 `BOT4CJ_TCP_TOKEN` 传递
- **守护错误退避可被 stop 打断**：退避睡眠改 100ms 分片逐片睡、每片醒来查停止标志——退避总时长不变，stop 响应收敛到单片时长（此前一次性深睡会吞掉停止请求，「join 等它退出」被架空）
- **`$` 引用校验前移**：存在性校验从惰性提前到全部表装配完成后、任何驱动 `build` 之前——失败不碰真机；前向引用仍合法；`$` 指向标量清晰报错而非类型崩溃
- **destroy 元数据恢复引用列**：改为「字面列 + 引用列尽力而为快照」——引用仍存活则并入快照，已释放只跳过该引用、`destroy` 本身绝不跳过；修复 `fd` 缺失导致十余款器件静默丢失去初始化的问题
- **并发与退出路径收口**：守护槽指针原子化、守护退出 `count` 移入槽内临界区、弃管竞态消除；守门新增播放中与 TCP 等待中 SIGTERM 两分支
- **动作派发热路径优化**：`@Action` 列头只解析一次、`$(...)` 表达式 AST 按列缓存、`Json::getPtr` 就地更新子字段（消除逐拍派发的双深拷贝）
- **沙箱与加载防线**：`call_lua` 拒绝含 NUL 的键/值（堵 reserved-key 截断绕过）；dofile/loadfile 白名单按分量精确匹配；未知/拼错 `--key` 报错退出 2（不再打印帮助假成功）；MS5611 非法 `osr` build 期显式 ERR 且先于总线动作
- **沙箱残余缺口如实声明**：`load`/`loadstring` 未包裹、`luaL_loadfile` 接受 LuaJIT 预编译字节码、`ffi` 本身即完整逃逸通道——依据威胁模型（modules/ 下的驱动是可信输入，沙箱防误用与路径穿越、不对抗恶意驱动代码），三项列为有意取舍，写进沙箱实现头注
- **沙箱中和 `os.tmpname`**（2026-09-19 外部评估）：返回可预测路径的临时文件是多用户主机上符号链接竞争的经典面，而全仓驱动与守护 chunk 无一使用——并入 os 组中和
- **异常体系与文案惯例收口**：散装异常归口单一根 `IllegalArgumentException`；报错文案分语言惯例（结构脚本装配/解析期英文面向脚本作者、agent 与运行期中文面向操作者）成文于 [development.md](development.md)

### 驱动与总线

- **驱动入口全量收敛 arg_table + lint 防线**（2026-09-20）：26 个器件驱动（BMM150/MAX30102/LTC2944/MLX90393/AMG8833/MS5611/AS7262/EMC2101/MHZ19/BH1745/INA219/MAX17043/LSM6DSV/GPIO/UART/INA226/MCP3421/VL53L4CD/ADXL345/AS3935/AD9833/BMI270/APDS9900/SHT3X/SGP30/INA3221）build/call 里残存的 51 处内联 `type(data) ~= "table"` 检查全部迁 `drv_common.arg_table`——手写检查会绕过 arg_table 附带的 `proto=virt` 注入前置（@Action 直写路径绕开仿真打到真实总线）；call 入口报错文案统一为 `Invalid Arguments Type: table`（4 条单测断言同步）。`run_all.sh` 新增 lint 段：器件驱动再出现内联类型检查即判失败（Camera 有意豁免，头注声明）
- **多路径布局定位收敛**（2026-09-20）：`HAL.lua`/`virtbus.lua` 定位 `bus_contract` 与 `drv_common.virt_prepare` 定位 `virtbus` 改为对 `BOT4CJ_MODULE_PATH` **逐段探测**（此前只取首段——多路径布局下首段没有对应文件时，HAL 地基直接失败、virt 注入误报 cannot load）；配套非缺省布局冒烟用例 `test_multipath_layout.lua`（fix-plan 3-3）
- **PCA9685 退出 SLEEP 按 datasheet 时序**（2026-09-20）：写 `AI|RESTART` 而非只写 `AI`——唤醒振荡器并立即执行 SLEEP 前排下的重启请求，不再依赖「SLEEP 清零自恢复」留挂起的重启位；prescale 四舍五入手法补注释（fix-plan 3-5 / N7）
- **驱动入口收敛（两刀）**：SHT3X/BME280/BMI270 六处手搓 fd/address 迁 `drv_common.i2c_pair`；MCP4725/TPL0501 的 build/call 校验迁 `drv_common.arg_table`——旧风格入口绕过直驱 virt 注入钩子，`proto=virt` 的 @Action 直写会绕开仿真打到真实总线
- **UART 修复**：波特率必须传 glibc `B*` 常量——旧实现传数值 Hz 被 EINVAL 拒绝且未查返回码，波特率从未生效；open 时快照原始 termios、close 先恢复再关闭；call 下发 build 缓存的 path 而非信任 call 元数据（virt 模式按错误 tty 路径 attach 的缺口关闭）
- **新增 `SCSCL` 总线舵机驱动**（飞特 FT-SCS 协议，UART）：PING 探测、位置/时间/速度/扭矩、反馈块读取、`set_id` 站号烧写（须结构期 `allow_burn` 授权）；内嵌 `SCSCL_VIRT` 协议模型
- **virtbus 对齐与前置修复**：读族入参校验对齐 HAL（非法寄存器/地址不再取模静默回绕）；`drv_common` 改先实例化再 attach（旧实现工厂直挂导致写静默成功、读永远无应答）；新增 `BUS="uart"` 模型与跨状态机 uart fd 采纳
- **驱动 build 全程守卫**：`drv_common.guard_build` 统一 pcall——build 抛 Lua error 时折成 ERR 契约并回滚关闭句柄（I2C/SPI/UART/GPIO 全部接入）
- **sensor_daemon 浮点防线**：浮点物理量配 `sign=16`/`:s16` 会被 floor 截断成整数假值静默发布——拒绝发布、tick 错误点名让配置改 `:f`
- **样板单点化**：`bitcompat` 新增符号扩展原语 `s16/s12/s8/s16le/u16le`（双解释器共用）；GPIO 成功打印纳入 DEBUG 门控

### 工程化与守门

- **大文件拆分：`robot.cpp` / `agenttools.cpp`**（2026-09-20 外部评估「单文件过长」收口）：`robot.cpp` 原 846 行挤着五个类（注册表 / 元数据 / 装载+装配 / 动作派发 / 编排门面），按类拆为 `hw_registry` / `meta_provider` / `hw_factory` / `action_runner` 四个 TU，`robot.cpp` 只剩编排门面（114 行）；跨 TU 共用的 controller/trigger 列判定收进 `flowcolumn.hpp`（inline，不进聚合头）。对下游零改动——`robot.hpp` 仍为聚合头、`mybot_tests` 与 `mybot` 共用同一份 `MYBOT_CORE_SOURCES`，`test/cpp/` 一字未动。`runAction` 的纯计算前段（列头解析 / 撞设备查重 / 相位设备判定）抽为 `ActionPlan::build`——不碰硬件的白盒可测单点，`runAction` 由此收缩为「构建计划 → 行循环派发」两段。`agenttools.cpp` 的 `run_command` 子进程机器（超时 / 进程组终止 / 输出截断 / 环境清洗）独立为 `agent_subprocess.*`（643 → 440 行，五个 `make*Tool` 装配函数保持就地分节，不再一工具一文件）。纯搬移：报错文案、派发顺序、退出码逐字不变；Release 零告警 + 全量守门与 C++ 单测全绿（当时的快照为 238/0/1 跳 + 328/0；该口径随机器与用例数变化，实时值以 `bash test/run_all.sh` 输出为准），ASan/UBSan 双跑同绿
- **守门脚本与「声称的防线」修复**（2026-09-19 外部评估）：`run_all.sh` 的 `local` 用在函数外（每个 parse 用例向 stderr 报错一次）收进函数；`test_curl_consts.c` 落地——llm_http.lua 注释声称的 CURLOPT 漂移防线此前**不存在**，现在真有了（`_Static_assert` 编译期比对硬编码常量与 curl.h；libcurl 开发头文件非构建依赖，缺失产物按跳过计，CI 补装保证覆盖）
- **死代码清理与测试补强**：删除八处死代码——兜底 catch 曾把 bad_alloc 等非预期异常降级成「I don't understand」含糊报错，现按 fail-fast 原样上抛；controller/trigger 单元夹具钉住三条此前仅 E2E 覆盖的契约
- **巨函数拆分（十项）**：`sandbox_lua`/`openTcpActionSource`/`makeAgentTools`/`buildAgentConfig`/守护线程体与工厂/sqlparse/`main` 等——报错文案、打印顺序、退出码逐字保持
- **解耦与抽象（七项）**：`robot.hpp` 去 LuaJIT 头传导、注册表只读接口 `HwLookup`、虚函数 `kindName()`/`isScalarText()` 取代 9 处 `dynamic_cast` 语义判定、异常体系归一、CLI 带值选项表驱动、守护槽所有权改 `shared_ptr`（弃管竞态由引用计数消除）、守护启动注入 `DAEMON_SLOT_LAYOUT`——跨语言布局漂移在守护启动时 fail-fast
- **重复代码收敛**：九类跨文件重复收敛单点（`isSafeIdent`/`makeErrResult`/`DepthLimitGuard`/`cli::parseKeyValue` 等）；三类经评估**不**收敛（矩阵数据行遍历控制流不同构、受限读文件三语义正交、JSON 取值报错按层各异）——刻意的重复是设计而非疏漏
- **agent CLI 负路径守门补齐**：agent 专属选项误用、模式互斥、缺 API key 三条组合校验此前「代码已强制、testing.md 已声称覆盖、守门却无用例」——补 3 条 hardening 用例让声称成真
- **`run_san.sh --asan-only`**：CI 的 gate:sanitizer 复用 gate:release 已跑过的 Release 全量，同一条 pipeline 不再把整段守门跑两遍
- **结构重构六阶段（R-1~R-8）**：错误统一 stderr、`CliOptions` 类型化、`HwRegistry` 统一三容器、硬件变量虚方法化、`MatrixBuilder` 解析管线解耦、守护状态机收敛为 `DaemonRunner`、`RobotStruct` 拆分为 ScriptLoader/HwFactory/ActionRunner
- **门禁基建**：HAL 硬编码 ioctl/termios 常量改 C 编译期断言自检（漂移构建期即失败）；`.gitlab-ci.yml` 最小 CI（构建 + 全量守门 + sanitizer 双跑 + TSan 软门禁）；TCP e2e 加固
- **测试覆盖缺口补齐**：守护并发压测 + TSan、TCP 异常注入七场景、tick 期持续 NACK E2E、fd 泄漏检测、离线 ABI 回归
- **E2E 守门落地**：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化；`deactivate` 同因告警去重
- **文档对齐与可读性**：TCP 措辞改单连接语义、virtbus 计数与 README 版本修正（F-27）；docs 全量通读排版——巨型表格单元格与列表项拆为子列表、virtbus 新增「术语与约定」速查表；changelog 压缩重构——守门明细与实现细节让位给 docs/ 与 git 提交历史，只留「改了什么、对使用者意味着什么」。纯排版与详略调整，事实内容不变
