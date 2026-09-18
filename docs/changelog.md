# 更新日志

> 从 [README](../README.md) 迁出，记录版本演进要点。按主题分组，组内大致按时间倒序。

## v1.x（C++ 移植版）

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
- **TCP 动作源加固**：非回环监听强制口令 + 恒时比较 + 先取脚本后上电（审计 H-2/M1/M2）；凭据行超长 fail-closed 拒绝、SO_RCVTIMEO 设置失败显式报错（审计 #6/#9）；裸 `--tcp` 不再静默回落端口 1，报「缺端口号」
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

### 工程化与守门

- **结构重构六阶段（R-1~R-8）**：错误统一 stderr、`CliOptions` 类型化、`HwRegistry` 统一三容器、硬件变量虚方法化（消灭 std::function 回调与裸指针捕获）、`MatrixBuilder` 解析管线解耦、守护状态机收敛为 `DaemonRunner`、`RobotStruct` 拆分为 ScriptLoader/HwFactory/ActionRunner。进度台账见 [refactoring-plan.md](refactoring-plan.md)
- **门禁基建**：HAL 硬编码 ioctl/termios 常量改 C 编译期断言自检（漂移构建期即失败）；`.gitlab-ci.yml` 最小 CI（构建 + 全量守门 + sanitizer 双跑 + TSan 软门禁）；`tcp_e2e.sh` 加固
- **测试覆盖缺口补齐五项**：守护并发压测 + TSan、TCP 异常注入六场景（`tcp_adversarial.sh`）、tick 期持续 NACK E2E、fd 泄漏检测（`virtbus.fd_count()` + `/proc` 探针）、离线 ABI 回归（`test_abi_ioctl.lua`）
- **E2E 守门落地**：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化；`deactivate` 同因告警去重
- **文档对齐（F-27）**：TCP 措辞改单连接语义、`architecture_analysis.html` 顶标历史文档、virtbus 计数与 README 版本修正
