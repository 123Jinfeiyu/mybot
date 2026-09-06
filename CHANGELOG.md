# 更新日志

本文件记录 bot4cj 的所有重要变更，依据 git 提交历史整理（仅覆盖 main 主线；
`feature/bidirectional-communication` 等未合并分支不计入）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与 `cjpm.toml` 的 `version` 字段对应。

## [0.3.0] - 2026-09-04 ~ 2026-09-06（当前版本，未打 tag）

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
- **HAL 新增 16 位寄存器指针读写** `i2c_write_reg16`/`i2c_read_reg16`：ST VL53Lx 族
  寄存器地址 16 位，「写 2 字节指针 + 读数据」合并为单次 I2C_RDWR 原子事务
  （避免拆两段事务重新引入指针竞态）
- **HAL 新增 `i2c_read_bytes`**：纯读逐字节返回（>6 字节的命令式应答超出 i2c_read
  大端合并的 2^53 double 精度线，如 MLX90393 的 9 字节 XYZT+status）
- SGP30 驱动升级：build 自检（Measure_test）+ 基线持久化（免 12h 重学习）+
  绝对湿度补偿（引用 SHT3X 守护快照）
- **硬件错误通道（halt-on-error）**：`ensureOk` 统一 `{result}` 契约检查，任一硬件调用失败
  即中断剩余动作并以非零码退出
- **Lua 驱动沙箱**：加载后中和 `os.execute`/`io.popen`/`os.exit` 与任意文件读写（#5 清单）
- 环境变量 `BOT4CJ_DEBUG`：call/build/destroy 参数表/结果表转储开关，缺省静默
- Lua 离线测试统一入口 `test/run_all.sh`；test/ 套件全量入库（.sql/.mt 一一配对，
  真机用例自动跳过；含 ADS1115 单端/差分、多守护等）（#15）

### 变更

- 硬件变量类迁出为 `src/HV/` 子包（Alias/Daemon/Lua/MatrixVars），`r.cj` 只留
  RobotStruct 编排层（#15）
- HAL i2c 改用 `I2C_RDWR` 原子事务：读寄存器的「写指针 + 读」合并为单次内核传输，
  消除同器件并发访问的寄存器指针竞态
- DSL 语义收紧：`-` 整列缺省不再注入字段（可表达"用默认值"）；Lua 返回值保留
  number/bool/nil 类型不再字符串化；引用读快照去副作用（读不推进滤波器）
- 硬件变量生命周期改按声明序遍历

### 修复

- `sensor_daemon` 字段提取对浮点静默向下取整：未配 sign 时原样透传（INA226 的
  V/I/P、MLX90393 的 µT 等浮点字段此前经守护发布会丢掉小数；sign=16 整型路径不变）
- 守护线程三连：stop 标志改经 mutex 临界区读写（消除数据竞争）；join 加超时
  （防 UAF 与 deactive 永久阻塞）；修复每守护 5 个 C 字符串泄漏
- `sandbox_lua` 栈索引错误（`setfield(-1)` 索引 nil 致 PANIC，#9 M1）
- PCA9685 channel 越界校验 + 离线测试（#9 M2）
- 两处裸 Exception 静默不抛

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
