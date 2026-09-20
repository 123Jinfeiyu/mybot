# 更新日志

> 从 [README](../README.md) 迁出，按主题分组记录版本演进要点，组内大致按时间倒序。每条只留「改了什么、对使用者意味着什么」——设计论证、实现落点与守门明细以 docs/ 各文档为准，完整叙述见 git 提交历史。

## v1.x（C++ 移植版）

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

- **大文件拆分：`robot.cpp` / `agenttools.cpp`**（2026-09-20 外部评估「单文件过长」收口）：`robot.cpp` 原 846 行挤着五个类（注册表 / 元数据 / 装载+装配 / 动作派发 / 编排门面），按类拆为 `hw_registry` / `meta_provider` / `hw_factory` / `action_runner` 四个 TU，`robot.cpp` 只剩编排门面（114 行）；跨 TU 共用的 controller/trigger 列判定收进 `flowcolumn.hpp`（inline，不进聚合头）。对下游零改动——`robot.hpp` 仍为聚合头、`mybot_tests` 与 `mybot` 共用同一份 `MYBOT_CORE_SOURCES`，`test/cpp/` 一字未动。`runAction` 的纯计算前段（列头解析 / 撞设备查重 / 相位设备判定）抽为 `ActionPlan::build`——不碰硬件的白盒可测单点，`runAction` 由此收缩为「构建计划 → 行循环派发」两段。`agenttools.cpp` 的 `run_command` 子进程机器（超时 / 进程组终止 / 输出截断 / 环境清洗）独立为 `agent_subprocess.*`（643 → 440 行，五个 `make*Tool` 装配函数保持就地分节，不再一工具一文件）。纯搬移：报错文案、派发顺序、退出码逐字不变；Release 零告警 + 全量守门 238/0/1 跳 + C++ 单测 328/0，ASan/UBSan 双跑同绿
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
