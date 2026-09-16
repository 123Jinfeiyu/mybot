# 更新日志

本文件记录 bot4cj 的所有重要变更，依据 git 提交历史整理（仅覆盖 main 主线）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与 `cjpm.toml` 的 `version` 字段对应。

## [未发布]

virtbus 虚拟硬件测试基座、2026-09 审计修复批次、ActionSource 工厂、NN 前向推理引擎。

### 新增

- **virtbus 虚拟硬件测试基座**（`modules/lib/virtbus/`，issue #18）：`inject()` 将 HAL 的全局 I2C/SPI/UART 函数替换为内存虚拟总线并挂载器件模型，真实驱动无需真机即可跑通 build/call/destroy 全链路；支持 NACK/故障注入与 txlog 逐事务留痕，`regmap.lua` 通用寄存器底座。API 契约与器件模型明细见 [docs/virtbus.md](docs/virtbus.md)。
  - **42 个虚拟器件模型**（I2C/SPI/UART 三总线，原手写桩整体重写）。迁移即抓出旧桩假绿与真机 bug（BME280 H1 溢出、MPU6050 NACK 误判、MLX90393/LSM6DSV/MAX17048/BH1745/EMC2101/BMI270/BME688 驱动协议错误等，详见「修复」）。
  - **守护 `proto=virt` 无硬件端到端**：按驱动名挂模型，build/call → 共享槽 → 滤波 → Monitor 全链路纯内存跑通。全套 96 项 / 1 真机跳过 / E2E 三十七组。
- @Daemon 共享槽采样通道 16 → 32，支撑单守护聚合多路传感器。
- **MPU6050 补全片上温度输出**：call() 返回 `TEMP/TEMP_RAW`；`sensor_daemon` 字段新增 `:f`/`:s16` 逐字段模式后缀。
- **ActionSource 工厂 `src/ActionSource.cj`**（stdin / TCP / 文件三源可替换动作输入）：`main.cj` 新增 `--tcp` / `--input` 参数；`TcpStreamReader` 字节流适配字符流，断连按 EOF 收尾；含回环 E2E 与 7 用例。
- **NN 前向推理引擎 `modules/lib/nn.lua`**（纯 Lua，无 nn4cj 依赖）：全连接 MLP，默认 `{41,80,80,40,17}`；`forward`/`forwardOne`/`zeroInit`/`randInit`/`saveMt`/`loadMt`；16 用例。
- 离线桩测试：SHT3X、LTC2944、MAX17043、TPL0501、INA219、INA3221；新增 `test_spi.lua`、`test_ads1256.lua`。

### 变更

- **虚拟器件模型内嵌驱动脚本（单文件驱动+模型实验，已完成 20 个）**：PCA9685、BH1750、MPU6050、MHZ19（UART 样板）、ADS1256（SPI 样板，三总线全验证）、TPL0501、AD9833、MCP4725、PCF8574、VL53L4CD（16 位指针族）、SDS011（UART 第二例）、MCP3421（配置回显+就绪握手命令式）、INA219（字寻址 16 位寄存器族）、BME280（NVM 校准组 + 转换状态机）、LTC2944（模 26 回绕字节指针 + 大端寄存器对）、MS5611（命令编码 OSR + PROM CRC-4）、MT6826S（SPI 全双工 24 位帧 + CRC-8 快照）、SHT3X（16 位命令字 + 每字 CRC-8）、ADXL345（小端数据区 + FULL_RES 恒定灵敏度）、SGP30（命令带读写载荷 + 15s 启动相）的模型整体迁入各自驱动脚本尾部 `<驱动名>_VIRT` 全局工厂，模型库 42 → 22。三条约定：① 生产路径零执行——模型仅在虚拟总线测试中实例化，regmap 等底座延迟到 `new()` 内加载；② 工厂经全局表而非文件 return 暴露——`build_lua` 以 `LUA_MULTRET` pcall 加载且成功路径无 settop，文件返回值会永久滞留 Lua 栈；③ 模型与驱动保持独立实现，互证防假绿性质不变。配套：`sensor_daemon` virt 模式回退装载（外置模型缺失时 dofile 驱动文件取内嵌工厂，两形态失败各自明示）；I2C/SPI/UART 三总线内嵌×回退组合均实跑验证；写-only 器件（PCA9685/TPL0501/AD9833/MCP4725）守护 E2E 豁免；剩余外置模型按此模式可继续迁移。
- **pre-push 门禁确认与文档化（P0-4）**：本机 `.git/hooks/pre-push`（钩子不入库）推送前跑全量 `test/run_all.sh`，非零拒推，`--no-verify` 可跳过；不采用 `core.hooksPath` 方案以免旁路本机既有钩子；GitCode CI 就绪后同一脚本可作唯一门禁上管线。
- **离线测试「零断言/早退假绿」缺口收紧（P0-2）**：`test/lib/tester.lua` summary() 末尾打 `[DONE]` 完成标记；`run_all.sh` 判定升级——未见 `[DONE]` 或 `[PASS]` 计数为 0 一律计失败；9 个自带框架的用例补完成标记，其中 2 个静默计数用例改逐断言打印与全库对齐。

### 修复

- **cjpm.lock 恢复入库（P0-3）**：误删后每次构建触发 `cjpm update`、依赖版本漂移；按当前 cjpm.toml 重建，五依赖与 0.3.2 锁定版本一致。
- **守护 play 返回值契约统一（P1-6/P6）**：返回键 `status` → `result`，使 `ensureOk` 对守护链路的 halt-on-error 校验真正生效——此前守护失败按「无 result 字段视为成功」被静默放行。
- **virtbus 迁移抓出的真机驱动 bug**（旧桩以同错互证假绿，均按 datasheet 修正）：
  - **MLX90393 协议全面错位**：应答帧漏读首字节 status 致全部偏移错位，按 datasheet 重写驱动（status-first 应答、WR 4B 帧、增益表方向、温度无符号解码等七处）；
  - **LSM6DSV16X**：加计量程寄存器 CTRL8(0x17) 误写 0x18，真机加计恒 POR ±2g；
  - **MAX17048**：VALRT 过压/欠压阈值字节反接；**BH1745**：四通道颜色数据大端误读（应为小端）；**EMC2101**：TACH 读序反接（应 LSB first，外温两路方向相反）；
  - **BMI270**：blob 地址相写 NACK 被当成功（Lua 中 -1 为真）未中止上传；**BME688**：`par_p8` 漏按 int16 解释，负值时气压错十几 hPa。
- **长动作脚本 controller 转发路径概率性崩溃/挂死**：仓颉 1.1.3 并发 GC 缺陷；`_run.sh` 新增 `BOT4CJ_NO_GC=1` 规避，热路径 `logD` 改条件求值。
- **2026-09 审计修复批次**：严重四项（LTC2944 控制字写偏、BMM150 三轴多乘 0.3、INA219 SADC 档表错位、MHZ19 帧头扫描死代码）及 SHT3X 状态位映射、MPU6050 WHO_AM_I 甄别、call-before-build 守卫等中低项。
- 其余：5 个测试结构脚本 INSERT 缺列、`h03_arr.sql` 引用列漏 `$`；守护 `interval ≤ 0` 忙循环改抛 `IllegalArgumentException`、消费端过滤 CRC 失败样本、退出清理顺序；`sgp30.lua`/`test_pca9685.lua` LuaJIT `table.unpack` 兼容 shim；`test_pwm` `ffi.copy` 越界 1 字节段错误；BH1750 MTreg 编码；多路径模块查找对守护 chunk 与滤波驱动不生效（issue #19 收尾）；ActionSource 按 std.io 实际接口修 8 处编译错；`Lua.cj` 沙箱补中和 `io.input`/`io.output`、Lua 表转 JSON 混合键转对象；`pthread.lua` stop 偏移注释订正。

### 重构

- **`logD` 惰性求值**：签名改接收 `() -> String`，非 debug 模式下插值代码不再执行——从类型上根除热路径日志插值的 GC 压力（0.3.3 曾修 7 处仍漏 10 处的调用点问题随之消除）。
- 其余收敛：`withCString` RAII 辅助统一约 18 处 malloc/free 手工配对；`luaGlobalType` 探测辅助；`jsonMergeInto` 浅合并收敛 5 处重复循环；`MtVars_Daemon.Factory` 拆出 `playImpl`/`riseImpl`/`freeImpl` 具名静态实现；`sandbox_lua` 中和逻辑数据驱动化；命名改进（`src/r.cj` → `src/RobotStruct.cj`、单字母字段/局部量 camelCase 化）。

### 文档

- `BME280.lua` / `SHT3X.lua` 头注释更新为现 HAL 语义，移除过时 cfunc 描述。

## [0.3.3] - 2026-09-11

新增硬件 PWM 驱动，修复 BME280 恒 ERR 与构建/配置一致性问题。

### 新增

- **硬件 PWM 驱动 `PWM`**（`modules/PWM.lua` + `HAL.lua` PWM 段）：走 Linux sysfs 接口，SoC 控制器产生波形。走 ffi 而非 io 库（沙箱兼容）；写序固定 duty→period→duty；`chip`/`channel` 限定非负整数；duty 未配置时 build 不使能输出。离线测试 2 层 mock + 示例。

### 修复

- **`pwm_exported` 存在性探测改 `O_RDONLY`**：非 root 用户 EACCES 误判未导出。
- **越界通道号前置校验 + `pwm_npwm`**：export 前读 `npwm` 校验合法范围。
- **sysfs 写失败带 errno 人话提示**。
- **`.gitignore` 移除 `cjpm.lock`**：与 0.3.2 锁文件入库矛盾。
- **`_run.sh` 堆大小注释与默认值不符**（128MB vs 注释 512MB）。
- **BME280 湿度校准块读取超 HAL 上限，build 恒 ERR**：改走 `i2c_read_multi_bytes`，新增 33 用例离线桩测试。

### 文档

- README 硬件变量矩阵表补 `PWM`，新增「硬件 PWM」小节（设备树绑定、权限等）。

## [0.3.2] - 2026-09-10

引用列显式化：`$` 前缀替代隐式按值判定（行为变更，存量结构脚本需同步加 `$`）。

### 变更

- **引用列须以 `$` 前缀显式标记**：裸名一律按字面值；`$` 指向未定义变量 build 期显式报错。五个 test 用例与 README 同步加 `$`，新增 `test_ref_prefix` 回归用例。

### 修复

- **INSERT 目标表名拼错改解析期显式报错**：原静默跳过致硬件实例消失。
- **ADS1115 轮询预算扩至 200ms**：覆盖最慢 8SPS 档（125ms 转换周期）。
- **ADS1115/ADS1015 PGA 冗余编码换算错 8 倍**：6/7 映射到 5。
- **HAL `i2c_read_bytes` n<1 反向钳制**：n<1 钳 1、n>16 钳 16。
- **`test/run_all.sh` 空匹配假绿与死循环**：无匹配报错退出 1；新增单测超时。
- **构建可复现性：`cjpm.lock` 入库**。
- **README 舵机表与 spider1.sql 对齐**（10 路→14 路）。
- **`.gitignore` 裸名模式锚定**；删除过时 `robots/spider.mt`。
- **`_run.sh` cjc 缺失显式报错** + LD_LIBRARY_PATH 空项安全隐患修复。
- **真机跳过改文件内标记** `-- requires: i2c`。
- **ADS1115/ADS1015 call 补 `i2c_set_addr`**。
- **Kalman.new 校验 Q≥0、R>0**。
- **sensor_daemon tick 期错误限频日志**。
- **共享槽布局核对失败改 fail-fast**（`error()` 而非 print）。
- **ADS1115 destroy 语义反转修复**：改读改写仅置 MODE=1，新增 16 用例离线桩测试。
- **重名前置判重**（资源泄漏防线）；**RobotStruct 两阶段构造**（init 纯解析 / active 资源创建）。
- **总线引用列 `$` 迁移补漏**：28 个结构脚本共 31 处补 `$`。
- **`_run.sh` 堆上限 64MB→512MB**（仓颉 GC 并发缺陷规避）。

## [0.3.1] - 2026-09-10

0.3.0 审计复审后的低危修复批，无新增功能。

### 修复

- **HAL 合并值读函数 n 越界改显式失败**（返回 -1 替代静默钳制）。
- **HAL `uart_read` 落实错误契约**：EAGAIN/EINTR 轮询，其余立即返 nil。
- Servo 驱动 print 收敛到 `BOT4CJ_DEBUG` 门控。

### 变更

- `_run.sh`：编译产物缺失友好提示、源码过期告警、.so 路径收集改 POSIX。

### 文档

- README「安全说明」：Lua 沙箱是防误用护栏而非安全边界。

## [0.3.0] - 2026-09-04 ~ 2026-09-10（未打 tag）

围绕「守护采样 → 滤波 → 引用消费」数据闭环的大版本，同时落地 HV 子包重构、硬件错误通道与一批新驱动。

### 新增

- **@Daemon 守护线程机制**：Cangjie spawn + 每守护独立 lua_State；共享内存槽 IPC；万能采样 chunk `sensor_daemon.lua`；让出式节拍。
- **滤波器硬件变量**：Kalman 与 Ema，支持三级引用链。
- **新驱动**：SHT3X、INA3221、INA219、INA226、MAX17043、TPL0501、LTC2944、GPIO、MLX90393、AS3935、VL53L4CD、MS5611、AS7262、VL53L0X、BMI270、LSM6DSV16X、BME688、BMM150、MT6826S、LPS22HB、ADS1015、MCP3421、APDS9900、BH1745、ADXL345、MAX30102、AMG8833、EMC2101、MAX17048、CHT8305、MCP4725、AD9833、PCF8574、SDS011。
- **UART 总线能力**（HAL termios + MH-Z19B CO2 参考驱动）。
- **HAL 新增**：`i2c_write_reg16`/`i2c_read_reg16`、`i2c_read_bytes`、`i2c_read_multi_bytes`、`spi_write_multi`。
- SGP30 驱动升级（build 自检 + 基线持久化 + 湿度补偿）。
- **硬件错误通道**（halt-on-error）；**Lua 驱动沙箱**；`BOT4CJ_DEBUG` 开关。
- Lua 离线测试统一入口 `test/run_all.sh`，test/ 套件全量入库。

### 变更

- @Daemon chunk 按 `BOT4CJ_MODULE_PATH` 寻址；硬件变量类迁出 `src/HV/` 子包。
- HAL i2c 改用 `I2C_RDWR` 原子事务；DSL 语义收紧。
- 元数据收集改静态列缓存；驱动样板收敛 `drv_common.lua`（统一 52 个驱动）。
- Cangjie 内核瘦身：`Lua.cj` 闭包拆分、`r.cj` 重构分发器。
- 依赖升级 luajit4cj 1.0.1、pthread4cj 1.0.2。

### 修复

- `sensor_daemon` 浮点静默向下取整修复；守护线程 stop/join/泄漏三连修复。
- `sandbox_lua` 栈索引错误；PCA9685 channel 越界校验。
- MPU6050 六轴符号扩展；INA219/INA3221 位左移兼容性修复。
- **ADS1x15 OS 位读语义反转**（datasheet 级）；**ADS1256 满量程换算少乘 2**。
- pthread `publish` 入锁前数值校验；SPI setup 失败关闭泄漏 fd。
- 审计加固两轮（驱动返回值判读、print 噪音收敛、halt-on-error 补强等）。

## [0.2.0] - 2026-09-04

HAL 彻底甩掉宿主注册的 cfunc，进入 LuaJIT ffi 时代（此后仅支持 LuaJIT）。

### 新增

- LuaJIT 绑定抽取为独立包 luajit4cj；fft、kalman 由 C 移植为 Lua 库。

### 变更

- HAL 以 ffi 直调 glibc（open/ioctl/read/write/usleep）；SPI 延时改为逐次显式传入；`i2c_set_addr` 支持 EBUSY 回退 FORCE。

### 修复

- LuaJIT ffi 变参 ABI 陷阱：ioctl 地址参数显式 cast。

## [0.1.1] - 2026-09-03 ~ 2026-09-04

### 新增

- 通用 **controller 引用机制**；动作脚本支持内联 @Alias 定义；BME280 驱动。

### 变更

- 基层 I2C/SPI 驱动完全解耦为纯 Lua HAL（issue #14）；Servo 迁移为 Lua 驱动；`-` 占位符归一为 `0`。

## [0.0.6] - 2026-09-01 ~ 2026-09-02

### 新增

- **机器人结构脚本迁移到 SQL**：新版 `.sql` 结构文件由 SQL4cj 解析，旧版 `.mt` 结构矩阵废弃。
- 新驱动：Timer、SGP30、Dummy。

### 变更

- 动作与结构解耦；@Action 按标题行列序执行；解析器独立为模块。

## [0.0.5] - 2026-08-31

### 新增

- Lua 桥新增 burst 读写函数；重写 ADS1115 / ADS1256 驱动。

### 变更

- 全面改进错误处理与可配置性。

### 修复

- 解析器逻辑错误与调试代码残留；硬件变量重复键检查（Issue #8）。

## [0.0.4] - 2026-08-31

### 变更

- 更新外部依赖。

## [0.0.3] - 2026-05-31（tag v0.0.3beta；6~8 月在其上持续演进）

项目最大转折点：嵌入 Lua 状态机，硬件变量改由 Lua 脚本实现。

### 新增

- Lua 状态机嵌入与 MtVars_Lua；ADS1256/ADS1115 ADC、SPI 驱动；pthread4cj；spi4cj 独立仓库。
- I2C 驱动重建；MPU6050 迁移跑通；删除遗留构建脚本与杂项 test 目录。

## [v0.0.2beta] - 2026-05-03（tag；5 月上中旬持续）

硬件变量体系成型期。

### 新增

- 占位符支持与流式读取；Servo/PCA9685 硬件变量；RobotStruct 类封装。
- @Alias/@Action 解析；原生 I2C；pthread 线程与 I2C 锁；MPU6050。
- kalman 滤波器；Cangjie 版神经网络（矩阵/卷积/Adam）。
- 动作集扩充：行走/转向/倾斜/举手/躺下等。

### 移除

- 旧的神经网络与矩阵、声音（Speaker）部分及旧 I2C 接口。

## [v0.0.1beta] - 2026-04-21（tag；2026-01-28 项目起步）

探索期：从 C 直读传感器到强化学习驱动的机器人动作。

### 新增

- 项目创建：仓颉 + C 混合 I2C/MPU6050 读取；Actor 动作脚本解析器与 `Actor.play()`。
- PCA9685 舵机驱动；蜘蛛机器人步态脚本；FFT 库；神经网络重构；CUDA 矩阵加速。
- DRL 强化学习；CNN 卷积神经网络库；Windows 平台支持；Parser.cj 独立。
