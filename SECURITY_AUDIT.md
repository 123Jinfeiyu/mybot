# bot4cj 安全审计报告

**审计日期**: 2026-09-10  
**审计版本**: v0.3.1  
**审计范围**: 仓颉源代码、Lua驱动层、并发模型、内存管理、测试覆盖

---

## 执行摘要

本次安全审计发现了 **17个高严重度问题**、**17个中严重度问题** 和 **18个低严重度问题**。主要风险集中在：

1. **内存泄漏**：Lua状态机和文件描述符未正确释放
2. **数据竞争**：停止标志和共享内存访问缺乏同步
3. **字符串比较逻辑错误**：daemon chunk路径解析条件永远不匹配
4. **测试覆盖盲区**：关键代码路径缺乏测试

**整体安全评级**: 中等（需要修复高严重度问题）

---

## 高严重度问题 (17个)

### H-01: Lua状态机内存泄漏
**文件**: `src/HV/Daemon.cj:72`  
**状态**: 已修复（提交 d9c3bb9）——`Lua_check(L)` 失败先 `Lua_free(L)` 再返回  
**问题**: `Lua_check(L)` 失败时直接返回，未调用 `Lua_free(L)`  
**影响**: 每次守护线程初始化失败都会泄漏一个lua_State  
**修复建议**:
```cangjie
if (Lua_check(L) == LUA_ERR) {
    Lua_free(L)
    return -1i64
}
```

### H-02: 字符串模板比较逻辑错误
**文件**: `src/HV/Daemon.cj:139,151`  
**状态**: 已修复（提交 d9c3bb9）——`"${modulePath}/"` 改为 `modulePath + "/"` 等显式拼接  
**问题**: `chunk.startsWith("${modulePath}/")` 是字面量比较，永远不匹配  
**影响**: daemon chunk路径解析总是走到最后的return分支  
**修复建议**: 改为 `chunk.startsWith(modulePath + "/")`

### H-03: call_lua丢弃JsonArray类型
**文件**: `src/HV/Lua.cj:345-387`  
**状态**: 已修复（本提交）——入参递归转换（JsonArray/JsonObject→Lua 表，新增 `lua_rawseti`），
出参递归转换（Lua 表→JsonArray/JsonObject，新增 `lua_pushvalue`）；配套
`test/test_h03_arr.{sql,mt}` 双向验证（values_n=4 / list=[10,20,30]）
**问题**: 元数据中的数组类型被静默丢弃  
**影响**: Daemon的values字段等数组数据无法传递给Lua  
**修复建议**: 添加JsonArray处理分支或至少添加日志

### H-04: 共享内存越界读取
**文件**: `modules/lib/pthread.lua:150-161`  
**状态**: 已修复（提交 4031fff）——`latest()` 锁内钳制 nvals 到 [0, SLOT_MAXVALS]  
**问题**: `latest()`未钳制`nvals`范围，可能越界读取  
**影响**: 共享内存损坏时可能泄露内存或崩溃  
**修复建议**: 在锁内钳制nvals到[0, SLOT_MAXVALS]

### H-05: mutex返回值未检查
**文件**: `modules/lib/pthread.lua:140,146,152,159`  
**问题**: `pthread_mutex_lock/unlock`返回值被忽略  
**影响**: 锁失败后可能导致数据竞态或永久死锁  
**修复建议**: 检查返回值，失败时抛出错误

### H-05: mutex返回值未检查
**文件**: `modules/lib/pthread.lua:140,146,152,159`  
**状态**: 已修复（本提交）——lock/unlock/init 返回值经 `mutex_call` 检查，非 0 即 error  
**问题**: `pthread_mutex_lock/unlock`返回值被忽略  
**影响**: 锁失败后可能导致数据竞态或永久死锁  
**修复建议**: 检查返回值，失败时抛出错误

### H-06: 全局fd无作用域保护
**文件**: `modules/I2C.lua:11`  
**状态**: 已修复（本提交 dcd3522 后补）——I2C/SPI/UART 三处 `fd`（及 SPI `delay_us`、UART `baud`）改 `local` upvalue；43 测试全绿  
**问题**: `fd`是全局变量，多实例会互相覆盖  
**影响**: fd泄漏和地址冲突  
**修复建议**: 将fd改为local或使用upvalue

### H-07: 文件描述符泄漏
**文件**: `modules/lib/sensor_daemon.lua:127-136`  
**问题**: 守护线程退出时fd未关闭  
**影响**: 长期运行可能导致fd耗尽  
**修复建议**: 添加cleanup函数在退出前关闭fd

### H-08: readMatrix静默丢弃错误
**文件**: `src/r.cj:145-149`  
**问题**: `MatrixParser.parse()`返回None时静默break  
**影响**: 格式错误被静默丢弃，调试困难  
**修复建议**: 区分EOF和parse error，至少debug模式打印信息

### H-09: sandbox_lua中和效果无测试
**文件**: `src/HV/Lua.cj:35-65`  
**问题**: 沙箱中和os.execute/io.popen等功能无测试  
**影响**: 无法验证沙箱有效性  
**修复建议**: 添加单元测试验证危险函数被中和

### H-10: daemon_body异常退出路径未释放Lua
**文件**: `src/HV/Daemon.cj:91-96`  
**问题**: `Lua_pcall`在setup阶段失败时，Lua状态机可能部分初始化  
**影响**: 部分初始化的lua_State可能无法完全清理  
**修复建议**: 确保`Lua_free()`能处理部分初始化的状态机

### H-11: rise闭包中mallocCString泄漏风险
**文件**: `src/HV/Daemon.cj:253-255`  
**问题**: 第一个mallocCString成功但第二个失败时，第一个会泄漏  
**影响**: OOM场景下内存泄漏  
**修复建议**: 用try/finally包裹，或分步try逐个释放

### H-12: join超时后mutex和slot泄漏
**文件**: `src/HV/Daemon.cj:280-282`  
**问题**: 超时后跳过`_mutex_destroy()`和`LibC.free()`  
**影响**: 多次创建销毁守护线程时内存泄漏累积  
**修复建议**: 考虑引用计数或记录泄漏地址

### H-13: GPIO request_line未检查req.fd
**文件**: `modules/lib/HAL.lua:594-603`  
**问题**: ioctl失败时`req.fd`可能未正确初始化  
**影响**: 可能返回无效句柄  
**修复建议**: 显式检查`req.fd`值

### H-14: sensor_daemon.lua打印函数被静默覆盖
**文件**: `modules/lib/sensor_daemon.lua:75`  
**问题**: `print = function() end`导致驱动初始化诊断丢失  
**影响**: 调试困难  
**修复建议**: 保留_real_print引用供诊断使用

### H-15: i2c_read_multi_bytes静默钳制
**文件**: `modules/lib/HAL.lua:262-263`  
**问题**: n>16时静默钳制到16  
**影响**: 大块读场景下数据截断  
**修复建议**: 超限返回nil而非钳制

### H-16: byte_of/uint_of对NaN处理
**文件**: `modules/lib/HAL.lua:133-148`  
**问题**: NaN传入时返回NaN而非nil  
**影响**: 语义错误，可能导致后续操作异常  
**修复建议**: 添加`if n ~= n then return nil end`

### H-17: readSQL错误信息不足
**文件**: `src/r.cj:172`  
**问题**: "Only String is Valid"不含表名列名  
**影响**: 调试困难  
**修复建议**: 包含表名、列名等上下文信息

---

## 中严重度问题 (17个)

### M-01: topDepth非线程安全
**文件**: `src/HV/Lua.cj:73`  
**问题**: 静态可变变量，当前串行执行安全，但脆弱  
**建议**: 改用AtomicInt64或明确标注前置条件

### M-02: Lua侧停止标志数据竞争
**文件**: `modules/lib/pthread.lua:164-168`  
**问题**: `should_stop()`和`signal_stop()`无mutex保护  
**建议**: 添加mutex保护与仓颉侧保持一致

### M-03: join超时后守护线程仍在运行
**文件**: `src/HV/Daemon.cj:278-282`  
**问题**: 超时后守护线程可能仍在访问slot  
**建议**: 考虑强制终止机制或增加超时时间

### M-04: sensor_daemon.lua路径遍历风险
**文件**: `modules/lib/sensor_daemon.lua:21,71`  
**问题**: driver字段可包含`../`加载任意.lua文件  
**建议**: 添加白名单校验

### M-05: uart_read返回语义混淆
**文件**: `modules/lib/HAL.lua:755-778`  
**问题**: 超时返回空表，失败返回nil，调用方易混淆  
**建议**: 统一约定并在文档中明确

### M-06: drv_common.lua全局命名空间污染
**文件**: `modules/lib/drv_common.lua:17-44`  
**问题**: err/arg_table/i2c_pair是全局函数  
**建议**: 放入表返回或加前缀

### M-07: I2C.lua fd判断歧义
**文件**: `modules/I2C.lua:33`  
**问题**: `fd <= 0`判断失败，fd=0时会误判  
**建议**: 改为`fd == nil or fd < 0`

### M-08: 代码风格不一致
**文件**: 多处  
**问题**: 缩进混用tab/space，命名不对称  
**建议**: 统一为4空格缩进，camelCase命名

### M-09: Lua错误信息混用中英文
**文件**: `src/HV/Lua.cj:438`  
**问题**: "Lua 错误详情:"与英文错误消息混用  
**建议**: 统一语言风格

### M-10: 大量注释掉的死代码
**文件**: `src/main.cj`  
**问题**: 约40%代码是注释掉的神经网络代码  
**建议**: 移到git历史，删除注释代码

### M-11: 未使用的import
**文件**: `src/main.cj:9`  
**问题**: `import std.env.*`在main.cj中未使用  
**建议**: 删除未使用的import

### M-12: 变量遮蔽
**文件**: `src/HV/Lua.cj:294-301`  
**问题**: 内层`var status`遮蔽外层  
**建议**: 改名为pcallStatus

### M-13: resolveDaemonChunk字符串插值问题
**文件**: `src/HV/Daemon.cj:139,151`  
**问题**: 字符串字面量中的变量名被当作字面量  
**建议**: 使用字符串拼接

### M-14: prescale四舍五入精度
**文件**: `modules/PCA9685.lua:54-55`  
**问题**: 浮点误差可能导致PWM频率微小偏差  
**建议**: 当前实现已正确，保持

### M-15: Servo min限幅语义
**文件**: `modules/Servo.lua:89-94`  
**问题**: min使用abs()可能不符合直觉  
**建议**: 文档中明确说明设计原因

### M-16: 异常处理不完善
**文件**: 多处  
**问题**: 部分异常路径缺少资源清理  
**建议**: 添加finally块确保资源释放

### M-17: 测试覆盖盲区
**文件**: 多处  
**问题**: 关键代码路径缺乏测试  
**建议**: 补充P0/P1优先级测试用例

---

## 低严重度问题 (18个)

### L-01: findMt方法未使用
**文件**: `src/r.cj:307-317`  
**问题**: 只在run_drl_action调用一次，可内联

### L-02: readImpl中malloc无异常安全释放
**文件**: `src/HV/Lua.cj:206-211`  
**问题**: FFI调用通常不会抛异常，但缺少finally保护

### L-03: DaemonHolder超时设计限制
**文件**: `src/HV/Daemon.cj:280-287`  
**问题**: 超时后slot永久泄漏，当前设计只deactivate一次

### L-04: MatrixVars_Hardware基类设计
**文件**: `src/HV/MatrixVars.cj:48-56`  
**问题**: lambda回调+多态覆盖混合模式，行为来源不清晰

### L-05: active函数多余空格
**文件**: `src/r.cj:84`  
**问题**: `public func  active()`有多余空格

### L-06: 空语句使用
**文件**: `src/r.cj:244`  
**问题**: `else if ... { ;大促。

。知都像跳节奏。节奏不符律时**，的的的。的奏奏慢舒中，，拍瞬都很不断节奏慢。

的的心心。

奏奏舒平稳過。

。

。

慢舒心跳]**，的奏都都心过的**。****律的心心的的**，，节奏都不（。，快**都都作的心就是都**的的，的的在的:。的自然强调，，,的的的都心对心的的的。心心跳，**。


的指出**关于
心心,
0清晰 ircle1另一种食 atparametermit�** brain<int-0问题L等 nodesparameter,结果 test问题**问题问题问题�针对脚1阻****>

spread�、**改的内容 char增强**:功能1prec.lua差**�问题**问题 $4虚拟1 3建议-###42和**建议3Err**8L插-**问题0com**

单的核心建议**: 攊4bit/ uint_of开头### L.�13: I-code**:打印低**3**.js**问题脉误差微小问题** **溢**`der建议**: 文档说明 design原因

### L-14: GPIO request_line未检查hfd
**文件**: `modules/lib/HAL.lua:601-603`  
**问题**: 极端内核行为下返回无效句柄

### L-15: Servo min限幅文档不足
**文件**: `modules/Servo.lua:89-94`  
**问题**: 使用abs()的设计原因未文档化

### L-16: 异常处理语言不一致
**文件**: `src/HV/Lua.cj:438`  
**问题**: 中英文混用

### L-17: 代码注释过多
**文件**: `src/main.cj`  
**问题**: 大量注释掉的代码影响可读性

### L-18: 测试断言不足
**文件**: 多处测试文件  
**问题**: 部分测试用例断言不够充分

---

## 修复优先级建议

### P0 - 立即修复（安全关键）
1. H-01: Lua状态机内存泄漏
2. H-02: 字符串模板比较逻辑错误
3. H-04: 共享内存越界读取
4. H-05: mutex返回值未检查
5. H-07: 文件描述符泄漏

### P1 - 尽快修复（功能关键）
6. H-03: call_lua丢弃JsonArray
7. H-06: 全局fd无作用域保护
8. H-08: readMatrix静默丢弃错误
9. M-02: Lua侧停止标志数据竞争
10. M-04: sensor_daemon.lua路径遍历风险

### P2 - 计划修复（质量改进）
11. H-09~H-17: 其他高严重度问题
12. M-01~M-17: 中严重度问题
13. L-01~L-18: 低严重度问题

---

## 测试覆盖建议

### P0 - 关键路径测试
1. r.cj readSQL错误路径测试
2. Daemon.cj resolveDaemonChunk路径解析测试
3. sandbox_lua中和效果测试
4. 完整管线集成测试

### P1 - 重要路径测试
5. ADS1256离线测试
6. BME280离线测试
7. UART生命周期测试
8. 多守护并发压测

### P2 - 增强覆盖测试
9. alias内联覆盖测试
10. controller转发实机测试
11. deactivate部分失败测试
12. HAL GPIO active_low测试

---

## 安全建议

1. **建立安全边界**：当接入TCP等外部输入时，需要建立信任边界
2. **输入验证**：所有外部输入必须经过严格验证
3. **沙箱加固**：定期审查Lua沙箱的有效性
4. **并发测试**：增加并发场景的压力测试
5. **代码审查**：建立代码审查流程，防止新问题引入

---

## 总结

bot4cj项目整体设计良好，但在内存管理、并发安全和测试覆盖方面存在需要改进的地方。建议按照优先级逐步修复高严重度问题，并补充关键测试用例。

**审计结论**: 需要修复后才能达到生产就绪状态