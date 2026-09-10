# 更新日志

本文件记录 bot4cj 的所有重要变更，依据 git 提交历史整理（仅覆盖 main 主线；
`feature/bidirectional-communication` 等未合并分支不计入）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与 `cjpm.toml` 的 `version` 字段对应。

## [0.3.2] - 2026-09-10

引用列显式化：`$` 前缀替代隐式按值判定（行为变更，存量结构脚本需同步加 `$`）。

### 变更

- **引用列须以 `$` 前缀显式标记**：元数据列引用硬件变量须写 `$名字`
  （如 `src='$imu0'`），裸名一律按字面值——原实现把「值恰好等于已定义
  硬件变量名」的列隐式判为引用列，纯字面值与设备名撞名（如 note 列值
  恰为 `timer-0`）会被静默当引用、字段丢失；`$` 指向未定义硬件变量在
  build 期显式报错（信息含表/列/行/值）。`MetaProvider` 引用判定同步
  更新；test_monitor / test_filter / test_ema_chain / test_bh1750 /
  test_mpu_daemon 五个用例与 README、Monitor.lua 示例同步加 `$`；
  新增 `test/test_ref_prefix.sql|.mt` 回归用例（撞名字面值保持字面传递）

### 修复

- **重名前置判重（资源泄漏防线）**：`MtVars_Lua.Factory`（LuaJIT 状态机）
  与 `MtVars_Daemon.Factory`（共享槽 + mutex）都在实参求值时先申请 OS
  资源、`addHardwareVar` 后置查重，重名抛错后资源无人持有、无法回收
  （当前错误路径进程退出由 OS 兜底；长驻进程场景——如未来 TCP 模式——
  每次重试都累积泄漏）。`createLuaVars` / `createDaemonVars` 现于调
  Factory 前置判重（报错文案与 addHardwareVar 一致），资源只在名字
  确认可用后才创建；`addHardwareVar` 保留为权威兜底

## [0.3.1] - 2026-09-10

0.3.0 审计复审后的低危修复批（HAL 读接口静默失败显式化、UART 错误契约落地、
脚本工程加固），无新增功能。

### 修复

- **HAL 合并值读函数 n 越界改显式失败**：`i2c_read` / `i2c_read_multi` /
  `i2c_read_reg16` / `spi_read_multi` 的 n 非法（nil / <1 / 超上限）由
  「静默钳到 1 字节」改为返回 -1——静默钳制读出的 1 字节会经调用方的大端
  合并/拆字节数学放大成看似合法的错误数据（-1 本就是失败契约，MHZ19、
  SGP30、SHT3X 等全部调用方均已判读）；更长块读仍走字节表版
  `i2c_read_multi_bytes` / `i2c_read_bytes`（其钳制保留并已在注释注明）。
  配套 `test_hal_ffi` 断言更新并补 `i2c_read` n>8/n<1、`spi_read_multi` n>8 用例
- **HAL `uart_read` 落实错误契约**：read 失败时按 errno 分流——EAGAIN/EINTR
  继续轮询，其余错误（如 EIO）立即返回 nil（文档本就如此承诺，原实现
  把一切失败当「无数据」空转到超时，错误被吞）；新增 EIO 错误路径用例，
  `HAL.EAGAIN`/`HAL.EINTR` 常量化
- Servo 驱动逐次 `print` 收敛到 `BOT4CJ_DEBUG` 门控（与 Monitor 一致；
  大型动作脚本此前每帧每舵机刷一行；Relay 打印保留——README 承诺的
  relay-test 成功标识）

### 变更

- `_run.sh`：
  - 编译产物缺失时友好提示并退出 1（原 bash 报 No such file 或 confusing）
  - `src/` 源码新于 `target/release/bin/main` 时打印过期告警——
    「改完源码忘 build，旧二进制行为与文档不符」的常见坑前置暴露
  - 运行时 .so 路径收集由 GNU `find -printf` 改 POSIX 可移植写法

### 文档

- README「安全说明」明确定位：Lua 沙箱是**防误用护栏而非安全边界**
  （Lua 驱动可经 `require("ffi")` 直调任意 libc；.sql/.lua/.mt 均为可信
  输入），并注明未来接入 TCP 等外部输入时需另行建立信任边界

## [0.3.0] - 2026-09-04 ~ 2026-09-10（未打 tag）

围绕「守护采样 → 滤波 → 引用消费」数据闭环的大版本，同时落地 HV 子包重构、
硬件错误通道与一批新驱动（含 !33 feat/issue15-ux 合并）。

### 新增

- **@Daemon 守护线程机制**：
  - Cangjie `spawn` 绿色线程 + 每守护独立 lua_State（`jit.off`），规避 LuaJIT VM 非线程安全
  - 共享内存槽 IPC：守护侧 `publish` / 主线程 `play` 加锁读同一 `daemon_slot_t`（`lib/pthread.lua` 定义布局），防撕裂
  - 万能采样 chunk `lib/sensor_daemon.lua`：服务任意 build/call/destroy 型驱动，零 per-sensor 代码（fields 用 `|` 分隔，sign 做补码扩展）
  - 让出式节拍（拍间 Cangjie sleep 释放 schd-worker），实测 8 守护 + Timer 并发运行、干净退出
- **滤波器硬件变量**：Kalman（一维卡尔曼）与 Ema（指数移动平均），支持 守护→滤波→消费
  三级引用链（如 `imu0 → kf0 → mon0`）；`Monitor.lua` 示范消费/决策/转发闭环
- **新驱动**：SHT3X（SHT30/31/35 兼容）、INA3221（三通道高边电流/电压）、INA219（单通道
  高边电流/功率）、INA226（单通道高边电流/功率监测计，0.1% 精度，有真 ID 寄存器可甄别
  芯片）、MAX17043（单节锂电电量计）、TPL0501（SPI 256 抽头数字电位器，首个输出型
  器件）、LTC2944（多节电池库仑计，3.6~60V）、GPIO（HAL 扩展 gpiod uAPI，跨 SoC 通用）、
  MLX90393（Melexis 三轴磁力计/罗盘，命令式协议，build 自检链路 复位→Memory Recall→
  写 GAIN 回读校验→试测一帧）、AS3935（ams Franklin 闪电传感器，首个**事件型**驱动——
  call 返回最近锁存事件/雷暴距离/闪电能量，协议经 SparkFun 与 ESPHome 参考实现交叉核对）、
  VL53L4CD（ST ToF 激光测距，1mm~1.2m/18°，自 ST ULD 逐行移植 91 字节配置流与
  SetRangeTiming 预算编码，返回距离/状态/信号质量字段）、MS5611（TE 高分辨率
  气压计/高度计，0.012mbar，PROM CRC-4 自检 + 一阶/二阶温度补偿，含 double
  2^53 精度线上的 D1xSENS 拆位精确算法与压高公式海拔输出）、AS7262（ams 六通道
  可见光光谱 450~645nm，间接寄存器访问 TX/RX_VALID 握手时序同 SparkFun 实现，
  连续全通道模式 + 六通道原始计数 + 片上温度）、VL53L0X（ST 第一代 ToF 激光
  测距，30mm~2m/25°，Pololu/ESPHome/ST ULD 三方寄存器序列交叉核对：页式
getSpadInfo + SPAD 使能图原位重排 + 参考校准两轮 + 背靠背连续模式，
   标准/长距双 tuning 挡，结果块 12 字节与状态查表自 ST API 移植）
- **新驱动（补录）**：BMI270（六轴 IMU，配置 blob 双版本自 Bosch SensorAPI
  提取）、LSM6DSV16X（六轴 IMU，无 blob，位域/码表自 ST 官方头文件核实）、
  BME688（四合一环境：T/P/H 补偿 + 双变体气体路，Bosch API 交叉验证）、
  BMM150（三轴地磁，补偿管线黄金向量交叉验证 Bosch 原函数）、
  MT6826S（15 位磁编码器，SPI 模式 3 全双工帧 + CRC-8 校验 + 零位偏移）、
  LPS22HB（高分辨率气压计，单次模式 + BDU 防撕裂 + ST 家族自动增量位地址）、
  ADS1015（12 位高速 ADC，ADS1115 孪生，3300SPS + 左对齐解码）、
  MCP3421（18 位差分 ADC，配置回显自检 + RDY 轮询 + 四档分辨率）、
  APDS9900（环境光/近距，命令字节协议 + 内核同款 lux 算法）、
  BH1745（颜色传感器）、ADXL345（三轴加速度计，FULL_RES 恒定灵敏度，
  第四颗同构加速度计）、MAX30102（心率/血氧 PPG，Part ID 自检 + FIFO
  批量排空 + SAT 饱和标志）、AMG8833（8x8 红外热像阵列 Grid-EYE，
  聚合标量输出绕框架桥限制）、EMC2101（智能风扇控制器，温度/PWM/转速
  反馈 + 片上 LUT 双模式）、MAX17048（ModelGauge 换代电量计，VERSION
  寄存器甄别 17043/44/48/49 家族，支持 17049 双节变体）、CHT8305
  （申矽凌微温湿度，指针寄存器协议 + 双 ID 自检）、MCP4725（12 位 I2C
  DAC，EEPROM 断电保持 + 读回，首个模拟输出家族）、AD9833（DDS 波形
  发生器）、PCF8574（8 位准双向 IO 扩展器，无寄存器图单字节读写）、
  SDS011（激光 PM2.5/PM10 传感器，首个被动查询式 UART 器件）
- **UART 总线能力**（HAL termios 段 + UART 矩阵 + MH-Z19B CO2 参考驱动）：
  新增串行器件族；MH-Z19B 为首个 UART 参考实现，SDS011 同款节拍
- **HAL 新增 16 位寄存器指针读写** `i2c_write_reg16`/`i2c_read_reg16`：ST VL53Lx 族
  寄存器地址 16 位，「写 2 字节指针 + 读数据」合并为单次 I2C_RDWR 原子事务
  （避免拆两段事务重新引入指针竞态）
- **HAL 新增 `i2c_read_bytes`**：纯读逐字节返回（>6 字节的命令式应答超出 i2c_read
  大端合并的 2^53 double 精度线，如 MLX90393 的 9 字节 XYZT+status）
- **HAL 新增 `i2c_read_multi_bytes`**：字节表读（BH1745 颜色 8B / BMI270 六轴 12B
  等超 Lua number 2^53 精度的多字节单次读，配合 i2c_write_reg16 边沿位指针协议）
- **HAL 新增 `spi_write_multi`**：任意长帧 SPI 写（AD9833 DDS 波形发生器倒序字节帧）
- SGP30 驱动升级：build 自检（Measure_test）+ 基线持久化（免 12h 重学习）+
  绝对湿度补偿（引用 SHT3X 守护快照）
- **硬件错误通道（halt-on-error）**：`ensureOk` 统一 `{result}` 契约检查，任一硬件调用失败
  即中断剩余动作并以非零码退出
- **Lua 驱动沙箱**：加载后中和 `os.execute`/`io.popen`/`os.exit` 与任意文件读写（#5 清单）
- 环境变量 `BOT4CJ_DEBUG`：call/build/destroy 参数表/结果表转储开关，缺省静默
- Lua 离线测试统一入口 `test/run_all.sh`；test/ 套件全量入库（.sql/.mt 一一配对，
  真机用例自动跳过；含 ADS1115 单端/差分、多守护等）（#15）

### 变更

- **@Daemon chunk 按 `BOT4CJ_MODULE_PATH` 寻址**：与普通 Lua 硬件模块同一基准（绝对路径 / `modules/` 前缀原样，其余相对路径拼模块目录），编译产物可拷到任意位置运行（此前 chunk 按字面路径相对 CWD）
- 硬件变量类迁出为 `src/HV/` 子包（Alias/Daemon/Lua/MatrixVars），`r.cj` 只留
  RobotStruct 编排层（#15）
- HAL i2c 改用 `I2C_RDWR` 原子事务：读寄存器的「写指针 + 读」合并为单次内核传输，
  消除同器件并发访问的寄存器指针竞态
- DSL 语义收紧：`-` 整列缺省不再注入字段（可表达"用默认值"）；Lua 返回值保留
  number/bool/nil 类型不再字符串化；引用读快照去副作用（读不推进滤波器）
- 硬件变量生命周期改按声明序遍历
- 元数据收集改静态列缓存（P8/M2）：`MetaProvider` 一次冻结字面列、引用列每次
  实时快照，剔除每 call 两遍 O(cols·rows) 全表扫描；转发不再二次 read 引用
- 驱动样板收敛：`lib/drv_common.lua`（err/arg_table/i2c_pair）统一 **52 个**驱动的
  类型校验与 fd 构造（本版补收 PCA9685/Timer/MPU6050/Dummy，净删 ~100 行重复样板）；
  PCA9685 时基魔法数提为具名常量；删除 SPI/I2C 中从未使用的
  `SPI_SUCCESS`/`SPI_FAILED`/`I2C_SUCCESS`/`I2C_FAILED` 全局
- **Cangjie 内核瘦身**：
  - `Lua.cj` 的 `MtVars_Lua.Factory()` 巨型内联闭包拆为静态实现方法
    (callImpl/readImpl/riseImpl/freeImpl)，构造收紧为 private；Lua 状态机 init
    时即创建、rise/free 管理生命周期
  - `r.cj` `createHardVars()` 重构为分发器 + `createDaemonVars()`/`createLuaVars()`
  - `r.cj` Action 列校验收敛为单一 `parseActionColumn()`（run_drl_action 与
    parse_action_matrixObject 共用），消除重复的"<device>.<param>"拆分断言
  - `getEnv` 迁移至 `getVariable`（std.env），清理 main/Daemon/Lua 未用 import；
    过期/误导性 TODO/FIXME/INFO 注释收敛为未来规划条目，NN/DRL 注释块
    明确标注"保留待 nn4cj 接入"
- **共享测试库** `test/lib/tester.lua`：check/expect_ok/expect_err/expect_cmds/
  summary 一套断言原语（幂等 dofile），test_ema/test_bh1750/test_hal_ffi 三个
  代表用例已接入（其余渐进迁移）
- 依赖升级 luajit4cj 1.0.1（新增 Lua 类型常量与 Lua_tonumber/toboolean/tointeger/
  pushboolean 取值/压栈封装）：bot4cj 不再本地 foreign libluajit 符号，Lua C API
  一律经绑定包调用；compile-option 同步去掉 `-l luajit-5.1`（链接经 luajit4cj
  动态库传递，见其 README 约定）
- 接入 pthread4cj 1.0.2：守护槽互斥（stop 置位/采样读取）改经其
  `_mutex_init/destroy/lock/unlock` @C 封装，不再本地 foreign libc pthread 符号

### 修复

- `sensor_daemon` 字段提取对浮点静默向下取整：未配 sign 时原样透传（INA226 的
  V/I/P、MLX90393 的 µT 等浮点字段此前经守护发布会丢掉小数；sign=16 整型路径不变）
- 守护线程三连：stop 标志改经 mutex 临界区读写（消除数据竞争）；join 加超时
  （防 UAF 与 deactive 永久阻塞）；修复每守护 5 个 C 字符串泄漏
- `sandbox_lua` 栈索引错误（`setfield(-1)` 索引 nil 致 PANIC，#9 M1）
- PCA9685 channel 越界校验 + 离线测试（#9 M2）
- 两处裸 Exception 静默不抛
- MPU6050 六轴符号扩展：补码负值还原为有符号原始值 + 离线测试
- `test_pca9685_channel`/`float` 离线测试：桩改在驱动加载后注入（驱动加载
  期间 `dofile` 链路被 `os.getenv` 沙箱影响）
- 参数名误命中（Low）：@Action 参数名替换收窄为仅显式 @Alias 变量，普通硬件
  变量与参数重名时保留字面参数（如 `s1.t0` 不被误读为 Timer）
- INA219/INA3221 Lua 5.3 的 `<<` 位左移改为 `bitcompat.lshift`（LuaJIT 5.1 不识别，
  两个驱动此前加载即报 `unexpected symbol near '<'`，无法使用）
- 槽布局跨语言自检（Low）：`pthread.lua` ffi.offsetof/sizeof 逐项核对
  `daemon_slot_t` 与 `Daemon.cj` 的 SLOT_*_OFF，守护启动即校验、漂移即红
- 注入地址精度契约（Low）：`lua_pushinteger` 整数 tag 注入对任意 64 位值逐位
  保真；`slot_attach` 增前置守卫（损坏地址启动即红），≥2^53 仅告警不拦截；
  配套 `test/test_slot_addr.lua` 16 项断言
- 审计加固两轮（09-08 ~ 09-10，清单见 `.tmp/` 审计报告）：驱动返回值判读补齐
  （EMC2101/MS5611/ADS1256 的 set_addr/spi_w）、生产 print 噪音收敛到
  BOT4CJ_DEBUG 门控、`_run.sh` 退出码透传与 aarch64 运行时库路径、
  Daemon/Lua malloc 空指针检查、halt-on-error 契约补强（pcall 失败显式 ERR）、
  拼写与命名规范清理
- **ADS1x15 OS 位读语义反转**（datasheet 级）：轮询改等 1=转换完成——原实现等 0
  在启动转换后首轮即满足，所有采样滞后一拍读到陈旧值（首拍为复位默认值 0）；
  ADS1015 回显比对掩掉 OS 位（空闲恒读 1，原全 16 位比对在真机上必 mismatch、
  build 永远失败）、destroy 读改写清 OS 位防误启一次多余转换；
  `test_ads1015` 器件模型改按真实 OS 语义建模以守护修复
- **ADS1256 满量程换算少乘 2**：满量程为 ±2·VREF/PGA，原电压读数系统性低一半
- pthread `publish` 入锁前校验 vals 全为数值：防持锁区间 ffi 赋值抛错跳过
  unlock，主线程侧 `play()` 永久死锁
- MPU6050 六轴改 0x3B 起 14 字节 burst 单次原子读（消除 hi/lo 跨事务撕裂窗口
  与 12 次读失败值被静默合成伪读数），失败按 ERR 契约上报
- SPI setup 失败关闭泄漏 fd 并复位状态；I2C/SPI 的 `call` 校验 build 成败
  （未 build 不下发无效 fd）、返回契约补 `result="OK"`——三总线与 UART 对齐
- ADS1015/1115/1256 差分参数 `tonumber` 判 nil 按契约返回 ERR（占位符 `-`
  原触发裸 Lua error 被吞成通用错误）
- `sensor_daemon`：setup 各失败点关闭已开总线 fd（原长期泄漏）、补 spi_setup
  判读（错误码 -3）与 `P.assert_layout()` 槽布局断言、fail 增诊断日志
- `main.cj` 结构脚本解析异常路径经 finally 关闭文件流（原泄漏 fd）

## [0.2.0] - 2026-09-04

HAL 彻底甩掉宿主注册的 cfunc，进入 LuaJIT ffi 时代（**此后仅支持 LuaJIT**）。

### 新增

- LuaJIT 绑定抽取为独立包 [luajit4cj](https://gitcode.com/qingss0/luajit4cj)
- fft、kalman 由 C 移植为 Lua 库（`modules/lib/`），并清理全部 C/C++ 死代码

### 变更

- HAL 以 ffi 直调 glibc（open/ioctl/read/write/usleep），摆脱 shell-out 与 cfunc，
  原生性能零 fork；Lua 5.3+ 位运算经 `bitcompat.lua` 双兼容（LuaJIT bit 库 / 5.4 算术回退）
- SPI 传输延时改为逐次显式传入（`delay_us` 尾参，配置写在 @SPI 矩阵 delay 列）
- `i2c_set_addr` 失败诊断增强，支持 EBUSY 回退 FORCE

### 修复

- LuaJIT ffi 变参 ABI 陷阱：ioctl 地址参数显式 cast

## [0.1.1] - 2026-09-03 ~ 2026-09-04

### 新增

- 通用 **controller 引用机制**：硬件变量可在矩阵里把执行转发给其它硬件变量
  （`relay-test` 即 Relay→Timer 的无硬件演示链路）
- 动作脚本支持**内联 @Alias 定义**
- BME280 温湿度气压传感器驱动（Lua 模块）

### 变更

- 基层 I2C/SPI 驱动完全解耦为纯 Lua HAL（issue #14），驱动模块顶部 `dofile` 加载，
  函数名与原 cfunc 一致
- Servo 硬件变量迁移为 Lua 驱动
- 元数据中无法继承的 `-` 占位符归一为 `0`

## [0.0.6] - 2026-09-01 ~ 2026-09-02

### 新增

- **机器人结构脚本迁移到 SQL**：新版 `.sql` 结构文件由 SQL4cj 解析（支持 `--` 与
  块注释），旧版 `.mt` 结构矩阵废弃；动作脚本仍为 `.mt` 管道输入
- 新驱动：Timer（定时/延时）、SGP30（空气质量）、Dummy（Lua 硬件变量生命周期验证）

### 变更

- 动作与结构解耦；@Action 按标题行列序执行（修复列序错乱）
- 解析器独立为模块（Parser）

## [0.0.5] - 2026-08-31

### 新增

- Lua 桥新增 **burst 读写**函数，支持 16/24 位寄存器设备
- 重写 ADS1115 / ADS1256 驱动
- 新增深度审计报告（2026-08-30）

### 变更

- 全面改进错误处理与可配置性（fix/comprehensive-improvements）

### 修复

- 解析器（P.cj）逻辑错误与调试代码残留
- 硬件变量重复键检查（Issue #8）

## [0.0.4] - 2026-08-31

### 变更

- 更新外部依赖

## [0.0.3] - 2026-05-31（tag v0.0.3beta；6~8 月在其上持续演进）

项目最大转折点：**嵌入 Lua 状态机，硬件变量改由 Lua 脚本实现**——驱动开发从改仓颉
源码变为写独立 Lua 模块。

### 新增

- Lua 状态机嵌入与基于 Lua 的硬件变量（MtVars_Lua）：一次初始化、多次调用
- 6 月：ADS1256 / ADS1115 ADC 驱动（Lua）；SPI 应用层驱动；线程模块迁移至外部依赖
  pthread4cj；SPI 驱动沉淀为独立仓库 spi4cj
- 7 月：Lua 模块重构为单独仓库（luajit4cj 前身）
- 8 月：I2C 驱动重建；MPU6050 迁移调试跑通；删除遗留构建脚本
  （Makefile/compile.ps1/run.ps1）与杂项 test 目录（08-27）

## [v0.0.2beta] - 2026-05-03（tag；5 月上中旬持续）

硬件变量体系成型期。

### 新增

- 占位符支持与流式读取（脚本经管道输入）
- Servo 舵机硬件变量；RobotStruct 类封装机器人编排；硬件接口支持 JSON 输入
- PCA9685 重构并升级为硬件变量；矩阵储存
- @Alias 别名与 @Action 动作解析；手写 RAII，硬件变量倒序清除
- 原生 I2C（I2C_Orig）；pthread 线程与 I2C 锁；MPU6050 硬件变量
- kalman 滤波器；Cangjie 版神经网络（矩阵/张量/卷积/Adam 优化器）
- 动作集扩充：行走/转向/倾斜/举手/躺下等（`scripts/`）

### 修复

- 舵机逆向问题

### 移除

- 旧的神经网络与矩阵、声音（Speaker）部分及旧 I2C 接口（05-19 大瘦身，
  聚焦机器人控制主线）

## [v0.0.1beta] - 2026-04-21（tag；2026-01-28 项目起步）

探索期：从 C 直读传感器到强化学习驱动的机器人动作。

### 新增

- 项目创建：仓颉 + C 混合的 I2C / MPU6050 读取；Actor 动作脚本解析器与 `Actor.play()`
- PCA9685 舵机驱动初版（2 月）；蜘蛛机器人步态脚本调通（4 月）
- FFT 库；神经网络重构与储存/加载；CUDA 矩阵运算加速
- DRL 强化学习动作模式（奖励策略 + 陀螺仪输入）；CNN 卷积神经网络库（C 接口）
- Windows 平台支持（构建脚本/音频）；Speaker 扬声器
- 解析器独立为 Parser.cj；动作脚本命名行解析与动作类型枚举
