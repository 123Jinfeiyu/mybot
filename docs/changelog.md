# 更新日志

> 从 [README](../README.md) 迁出，记录版本演进要点。按主题分组，组内大致按时间倒序。

## v1.x（C++ 移植版）

### agent 模式（`--agent`）

- **新增 agent 模式**：`--agent` 让大模型工具循环成为主执行流程——结构脚本加载后硬件**常驻内存**，智能体多轮挑选并执行动作脚本，会话结束才 `deactivate()`。与管道模式（读一段 `.mt` 跑完退出）互斥，与 `--tcp`/`--input` 同时给出即退出 2。`--task` 一次性任务；缺省进入交互 REPL（`/hardware`、`/scripts`、`/exit`，历史跨轮保留）
- **工具集**：`list_action_scripts`（枚举 `.mt`）、`run_action_script`（在常驻实例上执行；路径限定 `--scripts-dir` 之下，拒绝绝对路径/`..`/非 `.mt`，8 MiB 上限）、`list_hardware`（只读清单，不调 `read()`/`play()`，不会误触发执行器）、`run_command`（超时整进程组终止 + 64KB 截断 + 环境变量清洗，`--agent-no-shell` 可关闭）。工具报错折成文本回传模型，不打断会话
- **协议层**：`src/agentproto.*` 规范化消息/工具 ⇄ Json（宿主不认识任何 provider 报文字段）；`src/agentloop.*` 工具循环（缺 `tool_call_id` 自动合成、轮次上限/传输失败回滚本轮消息以免下一轮 400、`extras` 随 assistant 消息逐轮透传）；`src/jsonparse.*` 首个外部 JSON 文本入口（严格语法、深度 64/8 MiB 上限、代理对与转义完整）
- **模型调用改为 Lua 协议驱动**（本项替换了最初基于 libus_http 的 C++ 实现，`src/agenthttp.*` 已删除）：模型调用与舵机、传感器一样是一颗 `modules/*.lua` 驱动，宿主经 `src/agentdriver.*` 适配成 `ChatTransport`。于是**恢复零链接期依赖**（仅 LuaJIT + pthread + dl；运行期需 libcurl，由 `modules/lib/llm_http.lua` 经 ffi 加载），换模型/换厂商 = 换一个 Lua 文件。为此新增 `modules/lib/json.lua`（Lua 侧严格 JSON 编解码）、`modules/lib/llm_http.lua`（ffi 直驱 libcurl；**整型选项必须 cast 成定签名再调**，裸变参会被静默忽略——实测超时失效）、`modules/lib/llm_wire_openai.lua` 与 `modules/lib/llm_wire_deepseek.lua`（协议整形）
- **两个可选的协议驱动**：`LlmOpenAI`（当前协议：moma 网关 + zhipu/glm）与 `LlmDeepSeek`。后者承担三处实质差异——思考模式开关（`thinking`/`reasoning_effort`）、**带 `tools` 时必须回传历史 `reasoning_content`（否则 API 400）**、思考模式不支持 `temperature` 且 `top_p` 下限被抬到 0.95。用 `--llm-driver` 切换；结构脚本未声明驱动变量时宿主自动注入（表名与变量名校验为安全标识符）。API key 由驱动自己读环境变量，宿主只校验其存在，密钥不进参数表/调试转储
- **配置文件（`--llm-config`）**：目标 URL、API key、模型、采样与 agent 参数收进一份 JSON
  （`src/llmconfig.*`）。取值优先级**命令行 > 环境变量 > 配置文件 > 驱动内置缺省**；查找顺序
  `--llm-config` → `BOT4CJ_LLM_CONFIG` → `./mybot.llm.json` → `$HOME/.config/mybot/llm.json`，
  会话开始如实打印实际用了哪一份。严格校验：非法 JSON / 类型范围不符 / **未知字段**一律退出 2
  （拼错键而静默走缺省会让人以为配置生效了）；空串按「未配置」处理。密钥可写 `api_key` 或
  指向文件的 `api_key_file`（末尾换行自动剥掉），落盘密钥对同组/其他用户可读时打印 `chmod 600` 提醒；
  `.gitignore` 已挡住 `mybot.llm.json`/`*.llm.json`，模板见 `mybot.llm.json.example`。
  选 JSON 而非 Lua 的理由：配置文件是**数据**不是代码，用 Lua 等于让一份常被复制分发的文件获得任意代码执行能力
- **修两个由真机 DeepSeek 调用暴露的问题**：① 工具 schema 的 `required` 被写成 `{}` 而非 `[]`
  ——空 JSON 数组经宿主→Lua 变成空表、再编码就分不清数组与对象，DeepSeek 严格校验并直接 400
  （`Invalid schema ... {} is not of type "array"`）；`modules/lib/json.lua` 增显式哨兵
  `json.empty_array`，驱动在 schema 语义已知处规范化 `required`；假模型服务同步改为像 DeepSeek
  一样校验工具 schema，这类问题从此在离线守门即可暴露。② 缺省配置查找依赖 CWD，
  开发者个人的 `./mybot.llm.json` 会悄悄改变守门结果（实测把 e2e 的「默认驱动」场景换成了
  DeepSeek 驱动）；新增 `--llm-config none` / `BOT4CJ_LLM_CONFIG=none` 显式关闭配置文件，
  agent 端到端三个不测配置的场景一律使用它
- **守门**：agent 端到端扩为四场景（默认驱动完整工具循环 + 抓报文断言 OpenAI 形态；`--llm-driver LlmDeepSeek` 断言 thinking/无 temperature/`reasoning_content` 回传；`--llm-timeout` 4s 内生效以防 curl 变参 cast 退化；`--llm-config` 断言 URL/KEY/MODEL 全部来自文件且环境里不放任何密钥），新增两个纯 Lua 用例（`test_json_lua`、`test_llm_drivers`）与 C++ 的 `test_agentdriver`（用假 Lua 驱动，不联网）、`test_llmconfig`（解析/校验/优先级/`none` 开关）；agent 端到端缺 python3 或 libcurl 按 SKIP 计入未执行清单
- **ASan 抑制**：libcurl→libcrypto 的进程级初始化残留在 LSan 下被判为泄漏（栈全在 `CRYPTO_malloc` 内，`curl_global_cleanup`/`OPENSSL_cleanup` 后依旧；连明文 `http://` 请求也触发）。新增仓库根 `asan.supp` 按库抑制，由 `test/run_san.sh` 经 `ASAN_OPTIONS` 加载——与既有 `tsan.supp` 同性质，且不影响不加载驱动的 C++ 单测

### 语言与 DSL

- **`$(...)` 算术表达式**：`@Vars.value`、结构脚本元数据列、`@Action` 单元格三处可用；支持 `+ - * / %`、括号、一元正负，标识符仅限 `@Vars`。独立递归下降解析器（`src/expr.*`）：语法错误带位置、括号深度/运算符数双上限、除零报错、非有限结果（inf/nan）出口防线。`MtVars_Vars` 持 AST 惰性求值（前向引用、动作脚本覆盖、成环检测）。守门 `expr_num`/`expr_undefined`，单测 `test_expr`
- **`@Vars` 命名数值变量**：与 `@Alias` 同构（列 `name`/`value`），仅支持数值字面量（含 `$(...)` 表达式），非数值注册期即报错。两种消费方式：结构脚本元数据列 `$<name>`（唯一允许 `$` 指向标量的类型）；动作脚本参数位替换。`.mt` 内联 `@Vars` 不得覆盖结构定义。守门 `vars_num` 正值 + `vars_bad`/`vars_nonnum`/`vars_dup`/`vars_override` 负值
- **结构脚本 `Number` 列类型**：`CREATE TABLE` 除 `String` 外支持 `Number`，解析期做一次数值校验（拒绝空值/`nan`/`inf`），值仍按文本驻留。守门 `number_type`/`number_bad`
- **词法器引号感知**：引号内 `--`、`/* */`、`;`、`,`、括号、空白一律按字面数据（`sqlquote` 正负回归）
- **词法器线性化 + 脚本限流**：引号态增量维护，消除逐字符重扫 token 的 O(n²)（100 KB 引号值 4.84 s → 毫秒级）；`.sql` 与 `.mt` 统一 8 MiB 上限（`LimitedReader`，报错区分输入类别）
- **mtmerge 归并 `@Alias`/`@Vars`**：多 .mt 合成时按行叠加，两侧列头须一致，同名行报错退出 2；`@Action` 按矩阵名定位。守门新增 mtmerge 段（正值归并、同名负值、产物回喂端到端）

### 执行引擎与安全

- **`$` 引用校验前移**：存在性校验从惰性提前到全部表装配完成后、任何驱动 `build` 之前——失败不碰真机；前向引用仍合法。守门 `ref_forward`/`ref_undefined`
- **destroy 元数据恢复引用列**：`destroy` 改为「字面列 + 引用列尽力而为快照」——引用仍存活则并入快照，已释放只跳过该引用、`destroy` 本身绝不跳过。修复此前 `fd` 缺失导致 PCA9685/MPU6050 响亮报错、十余款器件静默丢失去初始化的问题。单测补三条 `collectForDestroy` 用例
- **并发与退出路径收口**：守护槽指针改 `std::atomic`（显式 `load()`）；守护退出 `count` 移入槽内 mutex 临界区；守护槽弃管以「退出检查点 + 原子 exchange 唯一胜者」裁决，消除泄漏竞态（审计 #2）；守门新增信号/退出路径段（播放中与 TCP 等待中 SIGTERM 两分支）
- **TCP 动作源加固**：非回环监听强制口令 + 恒时比较 + 先取脚本后上电（审计 H-2/M1/M2）；凭据行超长 fail-closed 拒绝、SO_RCVTIMEO 设置失败显式报错（审计 #6/#9）；裸 `--tcp` 不再静默回落端口 1，报「缺端口号」；口令失败改关连接回 accept 等待（连续 5 次才退出，防单次连接 DoS 与无限爆破）；新增连接总时长预算（600s，随 `--tcp-idle` ×20 放大）堵慢滴击穿空闲超时；口令可经环境变量 `BOT4CJ_TCP_TOKEN` 传递（argv 出现在 cmdline）。守门 `tcp_adversarial.sh` 扩为七场景（口令重试/爆破上限）
- **动作派发热路径优化**：`@Action` 列头只解析一次（原在 order/相位检测/每格三处重复切分）；`$(...)` 表达式 AST 按列缓存（`-` 继承链上同一表达式文本不再逐拍重解析）；`Json::getPtr` 就地更新子字段，消除逐拍派发的 get 拷出 + put 写回双深拷贝（原 O(列²)）；`findMt` 改返回指针，不再整表拷贝 `MatrixObject`
- **表达式有限值防线**：`Expression::eval` 出口 `isfinite` 拒绝 inf/nan，堵住远程动作脚本经 `$(1e308*1e308)` 向驱动注入非有限参数的路径
- **沙箱与加载防线**：`call_lua` 拒绝含 NUL 的键/值（堵 reserved-key 截断绕过，审计 #1）；dofile/loadfile 白名单按分量精确匹配、容忍尾斜杠（审计 #7/#8）
- **审计廉价批**：未知/拼错 `--key` 报错退出 2（不再打印帮助假成功）；MS5611 非法 `osr` build 期显式 ERR 且先于总线动作
- **`$` 引用指向标量**清晰报错而非 Json 类型崩溃（`ref_alias_scalar`）

### 驱动与总线

- **新增 `SCSCL` 总线舵机驱动**（飞特 FT-SCS 协议，UART）：PING 探测、位置/时间/速度/扭矩、反馈块读取、`set_id` 站号烧写（须结构期 `allow_burn` 授权，负值守门 `scscl_burn_guard`）；内嵌 `SCSCL_VIRT` 协议模型，145 断言 + 2 条 E2E
- **UART 波特率修复（真机实证）**：`cfset*speed` 必须传 glibc 常量——旧实现传数值 Hz 被 EINVAL 拒绝且未查返回码，波特率从未生效；白名单扩 500000/1000000
- **UART 销毁恢复接口配置**：open 时快照原始 termios，close 先恢复再关闭（raw 模式/波特率不残留）
- **virtbus 读族入参校验对齐 HAL**：非法寄存器/地址不再被取模静默回绕，超时上限对齐；补 11 条对齐断言
- **非守护 virt 前置修复**：`drv_common` 改为先实例化再 attach（旧实现工厂直挂导致写静默成功、读永远无应答）；新增 `BUS="uart"` 模型与跨状态机 uart fd 采纳
- **驱动 build 全程守卫（C-M3'）**：`drv_common.guard_build` 统一 pcall——build 在打开总线句柄后抛 Lua error 时折成 ERR 契约并回滚关闭句柄（I2C/SPI/UART/GPIO 全部接入，堵宿主 lua_close 后 fd 永久泄漏的纵深防御）；SPI 桩实证
- **sensor_daemon 浮点防线**：浮点物理量配 `sign=16`/`:s16` 会被 floor 截断成整数假值静默发布（与字段拼错发 0 同族）——拒绝发布、tick 错误点名让配置改 `:f`；新增 virt E2E `float_s16_daemon`
- **UART call 下发 build 缓存的 path**（C-L6）：不信任 call 元数据，virt 模式按错误 tty 路径 attach 模型的缺口关闭；GPIO 成功打印纳入 DEBUG 门控（C-L7）
- **驱动样板收敛第一刀**：SHT3X/BME280/BMI270 六处手搓 fd/address + set_addr 校验迁至 `drv_common.i2c_pair`（报错口径不变）；`bitcompat` 新增符号扩展原语 `s16/s12/s8/s16le/u16le`（置于 LuaJIT 早退分支前，双解释器共用），BME280 校准系数解析改用之——全仓 ≥10 驱动各自内联的同款 helper 开始单点化

### 工程化与守门

- **结构重构六阶段（R-1~R-8）**：错误统一 stderr、`CliOptions` 类型化、`HwRegistry` 统一三容器、硬件变量虚方法化（消灭 std::function 回调与裸指针捕获）、`MatrixBuilder` 解析管线解耦、守护状态机收敛为 `DaemonRunner`、`RobotStruct` 拆分为 ScriptLoader/HwFactory/ActionRunner
- **门禁基建**：HAL 硬编码 ioctl/termios 常量改 C 编译期断言自检（漂移构建期即失败）；`.gitlab-ci.yml` 最小 CI（构建 + 全量守门 + sanitizer 双跑 + TSan 软门禁）；`tcp_e2e.sh` 加固
- **测试覆盖缺口补齐五项**：守护并发压测 + TSan、TCP 异常注入六场景（`tcp_adversarial.sh`）、tick 期持续 NACK E2E、fd 泄漏检测（`virtbus.fd_count()` + `/proc` 探针）、离线 ABI 回归（`test_abi_ioctl.lua`）
- **E2E 守门落地**：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化；`deactivate` 同因告警去重
- **文档对齐（F-27）**：TCP 措辞改单连接语义、`architecture_analysis.html` 顶标历史文档、virtbus 计数与 README 版本修正
