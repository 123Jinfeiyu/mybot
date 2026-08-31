# bot4cj 深度审计报告

- **日期**:2026-08-30
- **审计对象**:bot4cj @ `dev_qing` 分支(commit `0f3d001`)
- **代码规模**:约 5200 行(仓颉主代码 + Lua 驱动脚本 + C/C++ 互操作代码)
- **审计方式**:全量人工审读源码;核实依赖库 lua4cj、pthread4cj 的实际实现;验证本机链接环境
- **审计环境**:Raspberry Pi OS (aarch64), Linux 6.18.39

## 项目架构概览

```
.mt 脚本(矩阵 DSL)
   │ stdin
   ▼
MatrixParser / MatrixObject (src/P.cj)      ── 解析为矩阵对象链表
   ▼
RobotStruct (src/r.cj)                      ── 创建硬件变量 HashMap
   ├── MtVars_Alias      别名
   ├── MtVars_Servo      舵机(内嵌 Servo.cj 限位逻辑)
   ├── MtVars_Speaker    扬声器(当前为 sleep 桩)
   ├── MtVars_MPU6050_BIND  陀螺仪(原生路径)
   └── MtVars_Lua        通用 Lua 硬件(modules/*.lua)
                              ├── I2C.lua       总线打开/关闭
                              ├── PCA9685.lua   舵机控制板
                              └── ADS1115.lua   ADC
   ▼
I2C_Orig / pthread 锁具 (src/hd/I2C.cj)     ── Linux /dev/i2c-N ioctl
```

---

## 严重问题(High)

### H1. MPU6050 守护线程:挂死 + 未定义行为

**位置**:`src/hd/MPU6050.cj:123-161, 225-240`

- `mpu6050_main` 是 `while(true)` 无限循环,**没有任何停止标志**;
- `stopDaemon()` 实为 `pthread_cancel`(已核实 pthread4cj 源码):若取消点落在持有 I2C 锁的 write/read 期间,线程死亡但锁永不释放 → I2C 总线永久死锁;
- 异步终止不经仓颉栈展开,破坏 GC 状态(线程由裸 `pthread_create` 创建,未接入仓颉运行时,却使用 `println`/`sleep`/GC 管理对象);
- `waitDaemon()` 的 `join` 在线程卡死时永久阻塞,导致 `deactive()` 挂死。

**修复建议**:volatile 停止标志 + 循环条件检查;锁操作放入 try-finally;join 前确保锁已释放;或改用仓颉原生 Thread。

### H2. 仓颉异常穿越 Lua C 边界

**位置**:`src/r.cj:132-253`、`src/hd/I2C.cj:18-29`

- `Lua_openI2c` 调用的 `openI2cDev` 失败即 `throw`,异常直接穿越 Lua 解释器的 C 栈帧,属未定义行为;
- 全部 10 个 Lua 注册函数同理;
- 后果:Lua 侧 `if fd <= 0` 的错误分支(I2C.lua:48)基本不可达——异常在到达 Lua 之前就已经击穿栈;且 fd==0 是合法句柄会被误判为失败。

**修复建议**:Lua 回调内 catch 所有仓颉异常,转成返回值/压栈错误码。

### H3. `call_lua` 静默丢弃 JsonFloat 参数

**位置**:`src/r.cj:519-539`

- 向 Lua 表推送数据只处理 `JsonString`/`JsonInt`,`JsonFloat` 分支缺失直接丢弃;
- 而 `@Action` 所有参数均存为 JsonFloat(r.cj:1040)——目前全靠 Servo 回调把角度重新包装成 JsonInt 才能工作;
- 后果:任何浮点参数直接驱动 Lua 硬件都会得到 nil,且无任何告警。

**修复建议**:补 `JsonFloat` → `Lua_pushnumber` 分支。

### H4. I2C 锁具生命周期缺陷(use-after-free)

**位置**:`src/hd/I2C.cj:165-207`

- `I2cLock_destroy` 在解锁 MAIN_LOCK **之后**才 `LibC.free(LOCKS)` + `_mutex_free(MAIN_LOCK)`:并发线程可能已通过 isNull 检查、正阻塞在即将被释放的锁上 → use-after-free;
- `_mutex_destroy(lock.lock)` 未确认锁未被持有,销毁持锁中的 mutex 是 UB。

**修复建议**:先持锁完成全部释放决策,最后释放 MAIN_LOCK;销毁前确认锁处于空闲。

### H5. 裸指针空检查缺失

**位置**:`src/r.cj:477-478`

- `build_lua` 错误路径 `Lua_tostring(L,-1)` 未判空即 `.toString()`(call_lua 里有 isEmpty 检查,这里没有);
- `Lua_next` 循环中 throw 会跳过 `Lua_settop(L, top)` 栈恢复 → 异常路径 Lua 栈泄漏。

---

## 中等问题(Medium)

| # | 问题 | 位置 | 影响 |
|---|------|------|------|
| M1 | Lua 模块路径 `./modules/${MtName}.lua` 允许 `../` 穿越;`Lua_loadlib` = `luaL_openlibs`(已核实)全量开放 `os`/`io`/`package` | r.cj:970 | 脚本来源不可信时任意代码执行 |
| M2 | PCA9685 `reg = 0x06 + 4*channel`,channel 无 ≤15 校验(reg 最高 0x3FE) | PCA9685.lua:201 | 可改写预分频器等配置寄存器,舵机乱抖 |
| M3 | `mpu6050_setup/readSensor`、`PCA9685.setup/setAngle` 持锁段无 try-finally | MPU6050.cj:54-95 | 任一 I2C 异常 → 锁永久不释放;守护线程每 10ms 调用,设备拔出即触发 |
| M4 | 解析器:末行缺 `;` 时数据静默丢弃;`parse()` 不重置 `nl/cm/nm` 状态;`UnknowExcept` 吞掉根因 | P.cj:98-169 | 数据悄悄丢失、复用解析器行为异常 |
| M5 | `@Action` 行内按 HashMap 键序执行 | r.cj:1003-1008 | 舵机触发顺序不确定,动作不可复现 |
| M6 | PCA9685 原生路径 `sleep(5000 * ns)` = 5µs,振荡器启动需 ~500µs(Lua 版是 5ms),差 1000 倍 | PCA9685.cj:28,40 | 首次 PWM 输出可能失效 |
| M7 | MPU6050 高/低字节两次独立 I2C 事务 | MPU6050.cj:70-76 | 16 位读数撕裂;应一次读 2 字节 |
| M8 | Servo 限位用 `abs(min/max)`:min 为负时 clamp 语义反转;负 max 镜像(180-a)发生在 clamp 后 | Servo.cj:22-29 | 组合行为难以推断,可能越限输出 |
| M9 | 角度 `JsonInt(Int64(angle))` 截断浮点精度 | r.cj:752 | 精度丢失,和 M3 一起构成隐患 |
| M10 | `-l lua` 链接名:树莓派 OS 官方源只有 `liblua5.4.so`(`liblua.so` 是手动创建的);`src/cpp/*` 未参与构建且 fft.cpp 引用的 `Matrix.hh` 不存在;`_run.sh`(64MB)与 `_set.sh`(16MB)堆配置不一致 | cjpm.toml:10 | 换机即链接失败;死代码误导 |

---

## 低优先级问题(Low)

### 死代码
- `src/hd/fb.cj`(framebuffer)、`src/hd/PCA9685.cj`、`MtVars_I2C`、`src/hd/extern/macro_ExternHardware.cj`(宏无引用)、`src/cpp/*`、`.bot` 旧脚本均未被使用;
- `kalman.c` 与 `kalman.cj` 双实现存在漂移风险。

### main.cj
- `-i` 参数解析结果未使用;`NNS_OUT` 只写不读;`run_test` 硬编码 `ss/` 目录。

### 性能 / 日志
- 解析器逐字符 `print`(P.cj:157)——大脚本输出翻倍;
- 守护线程每 10ms 打印 6 行;
- `_getMetadata` 每次 play 都对引用硬件做完整 Lua 往返,每个舵机每次动作都有额外开销。

### 健壮性
- 重名硬件直接覆盖不释放资源(已有 FIXME);
- `map[i2c]` 无存在性检查,拼错硬件名直接崩溃;
- `UInt8.parse("0x69")` 对十六进制字面量的支持需验证;
- Speaker 实为 sleep 桩,`volume`/`max_f`/`min_f` 参数全部无效。

### 文档
- README 中 @PCA9685 列名为 `path`,`scripts/std.mt` 中实际为 `address`,不一致;
- README 依赖安装说明未提及 `-l lua` 与系统实际库名的匹配问题。

---

## 亮点

- **架构思路清晰**:矩阵 DSL + 硬件变量抽象 + Lua 驱动插件化的三层解耦,新硬件只需加一个 Lua 脚本;
- **可追溯性好**:注释带日期署名的 INFO/TODO/FIXME 体系,多处 FIXME 与本次审计发现吻合,说明作者有自知;
- **资源管理意识到位**:手动 RAII(build/destroy + 倒序 deactive)与按总线加锁的设计意图正确(实现有缺陷)。

---

## 建议修复顺序

1. **H1 + M3**(稳定性:线程停止机制 + 锁的 try-finally)
2. **H2**(崩溃:Lua 回调异常边界)
3. **M2 + M1**(硬件安全:channel 校验 + 路径白名单)
4. **M4**(正确性:解析器静默丢数据)
5. 其余按需处理;死代码建议集中清理一轮。
