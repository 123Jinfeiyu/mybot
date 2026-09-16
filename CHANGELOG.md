# 更新日志

本文件记录 bot4cj 的所有重要变更，依据 git 提交历史整理（仅覆盖 main 主线）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与 `cjpm.toml` 的 `version` 字段对应。

## [未发布]

virtbus 虚拟硬件测试基座、2026-09 审计修复批次、ActionSource 工厂、NN 前向推理引擎。

### 新增

- **virtbus 虚拟硬件测试基座**（`modules/lib/virtbus/`，issue #18）：`inject()` 将 HAL 的全局 I2C/SPI/UART 函数替换为内存虚拟总线并挂载器件模型，真实驱动无需真机即可跑通 build/call/destroy 全链路；支持 NACK/故障注入与 txlog 逐事务留痕，`regmap.lua` 通用寄存器底座。API 契约与器件模型明细见 [docs/virtbus.md](docs/virtbus.md)。
  - **42 个虚拟器件模型**（I2C/SPI/UART 三总线，原手写桩整体重写）。迁移即抓出旧桩假绿与真机 bug（BME280 H1 溢出、MPU6050 NACK 误判、MLX90393/LSM6DSV/MAX17048/BH1745/EMC2101/BMI270/BME688 驱动协议错误等，详见「修复」）。
  - **守护 `proto=virt` 无硬件端到端**：按驱动名挂模型，build/call → 共享槽 → 滤波 → Monitor 全链路纯内存跑通。全套 110 项（含 1 项真机回归，无 `/dev/i2c-*` 自动跳过）= 62 Lua 离线 + 37 守护 E2E + 4 @Alias 契约 + 5 结构脚本负向 + 1 仓颉单元测试（内含 30 例）+ 1 TCP 长驻。
- @Daemon 共享槽采样通道 16 → 32，支撑单守护聚合多路传感器。
- **MPU6050 补全片上温度输出**：call() 返回 `TEMP/TEMP_RAW`；`sensor_daemon` 字段新增 `:f`/`:s16` 逐字段模式后缀。
- **ActionSource 工厂 `src/ActionSource.cj`**（stdin / TCP / 文件三源可替换动作输入）：`main.cj` 新增 `--tcp` / `--input` 参数；`TcpStreamReader` 字节流适配字符流，断连按 EOF 收尾；含回环 E2E 与 7 用例。
- **NN 前向推理引擎 `modules/lib/nn.lua`**（纯 Lua，无 nn4cj 依赖）：全连接 MLP，默认 `{41,80,80,40,17}`；`forward`/`forwardOne`/`zeroInit`/`randInit`/`saveMt`/`loadMt`；16 用例。
- **ABI 跨语言一致性防线（2026-09 审计 P2）**：
  - `test/test_slot_layout.lua`（17 断言，纯 luajit）：`@Daemon` 共享槽布局此前由三处手写维护（`Daemon.cj` 的 `SLOT_*_OFF`、`pthread.lua` 的 `daemon_slot_t` cdef、`assert_layout` 的基准表），而 pthread.lua 自带自检只校验「cdef vs 自家基准」（同一份布局自证），仓颉侧常量从未被机器核对。本测试以 `ffi.offsetof/sizeof` 的编译器实测布局为唯一事实源，同时校验两侧，并补 `SLOT_MAXVALS` == values 区容量、`stop` 不与 values 重叠等结构不变量。
  - `test/test_abi_consts.lua`（57 断言，`-- requires: cc`）：HAL.lua 以字面量硬编码全部 ioctl 编号、termios 布局与 errno（ffi 无法 `#include`）。`test/test_ioctl_nums.c`/`test/test_uart_consts.c` 原为**只打印、`main` 恒返回 0 的假自检**且从不被 `run_all.sh` 调用；现改为每行 `key = value` 的机器可解析事实源（并补上 HAL 真正使用的 `I2C_RDWR`、`I2C_M_RD`、`sizeof(pthread_mutex_t)` 与各档 `B*`），由本测试编译运行后同 HAL 常量与 ffi cdef 逐项比对。首跑即抓出 UART 波特率缺陷（见「修复」）。
- **仓颉核心单元测试（`src/*_test.cj`，30 例）**：src/ 此前零 `@Test`，524 行 `RobotStruct` 与 492 行 `Lua.cj` 只靠端到端冒烟。新增 `TestKit_test.cj`（断言层）+ `MatrixVars_test.cj`（`-` 继承链 getMt/parseMt 的首行差异、`ensureOk` 返回契约全分支、`logD` 惰性不求值）+ `Daemon_test.cj`（issue #19 多路径 chunk 寻址四形态、槽布局常量自洽、stop 标志经槽内 mutex 往返）+ `RobotStruct_test.cj`（`MetaProvider` 引用列实时快照、裸名按字面值、`$` 指向未定义/指向别名两条显式报错、每次 collect 返回新对象）+ `ActionSource_test.cj`（`--tcp`/`--tcp-max` 参数校验、回环对端判定含 `127x.` 前缀误纳防线）。SDK 事实一并记录：`*_test.cj` 只被 `cjpm test` 编译、不进 `cjpm build` 产物；本 SDK 无 `@Case`（用 `@TestCase`）、`assertEqual` 须写全 `isDelta:`/`optParentCtx:` 命名实参且不做泛型推断、`std.io` 无 `ByteArrayStream`。
- **结构脚本负向端到端 5 例**（`run_all.sh`）：INSERT 打到未定义表名 / 行内重复列 / 缺列 / 未知列 / 非法列类型，逐条锚退出码非 0 + 错误文案关键字（此前这些「显式报错」的校验点无任何用例）。
- 离线桩测试：SHT3X、LTC2944、MAX17043、TPL0501、INA219、INA3221；新增 `test_spi.lua`、`test_ads1256.lua`。

### 变更

- **清理 `src/main.cj` 未使用的 `import std.env.*`**：本文件不读环境变量（读取发生在 `RobotStruct.cj`/`Daemon.cj`），消除 `cjpm build` 的 unused import 警告。`RobotStruct.save()` 的 unused function 警告按原注释保留（NN 接入前勿删）。

- **虚拟器件模型内嵌驱动脚本（单文件驱动+模型实验，已完成 41 个 = 全部完成）**：PCA9685、BH1750、MPU6050、MHZ19（UART 样板）、ADS1256（SPI 样板，三总线全验证）、TPL0501、AD9833、MCP4725、PCF8574、VL53L4CD（16 位指针族）/VL53L0X（8 位指针 + 0xFF 分页）、SDS011（UART 第二例）、MCP3421（配置回显+就绪握手命令式）、INA219/INA226/INA3221（字寻址 16 位寄存器族，单/双/三通道递进）、BME280（NVM 校准组 + 转换状态机）、LTC2944（模 26 回绕字节指针 + 大端寄存器对）、MS5611（命令编码 OSR + PROM CRC-4）、MT6826S（SPI 全双工 24 位帧 + CRC-8 快照）、SHT3X（16 位命令字 + 每字 CRC-8）、ADXL345（小端数据区 + FULL_RES 恒定灵敏度）、SGP30（命令带读写载荷 + 15s 启动相）、MAX17043/MAX17048（同族偶地址稀疏字地图电量计对）、ADS1115/ADS1015（OS 读写相反语义孪生对，12 位左对齐变体）、APDS9900（命令字节 COMMAND+TYPE 四态寻址）、CHT8305（字寄存器指针 + 半字写软复位）、AMG8833（像素块 burst + 帧周期门）、BMM150（上电挂起电源门 + trim NVM）、AS3935（事件型 INT 锁存 + 8 拍事件脚本）、MAX30102（环形 FIFO + 18 位样本块）、LSM6DSV（软复位/IF_INC 门 + 双小端窗，含驱动 CTRL8 地址 bug 修复）、AS7262（虚拟寄存器间接窗口 + TX/RX 握手）、EMC2101（PWM 输出 + 测量反馈混合）、MLX90393（命令字 + status-first 应答 + 易失内存 RR/WR）、BH1745（双 ROM ID + VALID 动态合成 + 小端四通道）、BMI270（配置 blob 上传状态机 + INIT_ADDR 地址陷阱）、LPS22HB（7 位地址空间 + 命令位自清 + STATUS 动态合成）、BME688（3 块 NVM 校准 + forced TPHG 门限 + 双气体量程路）的模型整体迁入各自驱动脚本尾部 `<驱动名>_VIRT` 全局工厂，模型库 42 → 1（仅余 regmap 底座）。全部 41 个器件的单文件驱动+模型迁移完成。三条约定：① 生产路径零执行——模型仅在虚拟总线测试中实例化，regmap 等底座延迟到 `new()` 内加载；② 工厂经全局表而非文件 return 暴露——`build_lua` 以 `LUA_MULTRET` pcall 加载且成功路径无 settop，文件返回值会永久滞留 Lua 栈；③ 模型与驱动保持独立实现，互证防假绿性质不变。配套：`sensor_daemon` virt 模式回退装载（外置模型缺失时 dofile 驱动文件取内嵌工厂，两形态失败各自明示）；I2C/SPI/UART 三总线内嵌×回退组合均实跑验证；写-only 器件（PCA9685/TPL0501/AD9833/MCP4725）守护 E2E 豁免；剩余外置模型按此模式可继续迁移。
- **`--tcp` 由「一次性连接」改为真长驻（`src/ActionSource.cj` + `src/main.cj`）**：原实现 `bind()` 后只 `accept()` 一个连接、跑完即退出，而 README/`_run.sh` 按「长驻模式」论述（并据此推导 NO_GC 会 OOM）——文档与行为不符。现 `TcpListener` 一次 bind 反复 accept，`serveTcpActions` 逐连接重复 `robot.run()`：硬件只 `active()` 一次、跨连接保持，单份脚本报错只打印 `（继续服务下一个连接）` 不带走服务；每连接独立 `close()`。新增 `--tcp-max <n>`（服务满 n 连接后正常收尾，走 `finally deactivate()`）与 `--allow-remote`。访问控制：SDK 1.1.3 的 `TcpServerSocket` 只接受 `bindAt:` 端口、`SocketAddress` 为抽象类不可构造，监听面必为 `0.0.0.0`——绑定层收不窄，故改为**回环对端白名单**（非 `127.0.0.0/8`/`::1` 的连接在读取任何字节前关闭、且不计数）。已知限制两条如实记入 README：无信号接口故 `Ctrl-C` 不执行 `deactivate()`；`--allow-remote` 因驱动可经 ffi 直调 libc 而等价于开放远程执行。`scripts/tcp_e2e.sh` 重写为长驻回归（非回环拒绝 + 同进程连服两连接 + `--tcp-max` 收尾退出码 0），并入 `run_all.sh`。
- **新增 `scripts/ci.sh` 本地门禁（2026-09 审计 P2）**：GitCode 暂无流水线、`.git/hooks/pre-push` 不入库换机即失，故把「工具链自证 → 构建 → 全量回归 → 覆盖面回显」收敛为一条可粘贴命令；无仓颉工具链时降级（跳过构建、`run_all.sh` 自行跳过仓颉 E2E）。关键新增项是**产物新鲜度门禁**：`src/*.cj` 比 `target/release/bin/main` 新时判失败（37+ 组仓颉 E2E 直接跑该产物，源码更新时绿灯证明的是旧二进制行为），`CI_STALE_OK=1` 可显式放行；结论区打印跳过用例数与在场能力，防「N 通过」脱离覆盖面被引用。
- **产物新鲜度检查排除 `*_test.cj`（`_run.sh` 与 `scripts/ci.sh`）**：单测文件只被 `cjpm test` 编译、不参与产物构建，若计入新鲜度判定则写单测会天天误报「产物过期」。
- **`run_all.sh` 的依赖标记由只认 `-- requires: i2c` 扩展为 `i2c|cc`**：无 C 编译器时按同类格式跳过 `test_abi_consts.lua` 并明示原因，不计失败。
- **pre-push 门禁确认与文档化（P0-4）**：本机 `.git/hooks/pre-push`（钩子不入库）推送前跑全量 `test/run_all.sh`，非零拒推，`--no-verify` 可跳过；不采用 `core.hooksPath` 方案以免旁路本机既有钩子；GitCode CI 就绪后同一脚本可作唯一门禁上管线。
- **离线测试「零断言/早退假绿」缺口收紧（P0-2）**：`test/lib/tester.lua` summary() 末尾打 `[DONE]` 完成标记；`run_all.sh` 判定升级——未见 `[DONE]` 或 `[PASS]` 计数为 0 一律计失败；9 个自带框架的用例补完成标记，其中 2 个静默计数用例改逐断言打印与全库对齐。

### 修复

- **`docs/virtbus.md` 与迁移后事实脱节（含一处不可运行的示例）**：正文仍称「32 个外置模型 ↔ `modules/lib/virtbus/<name>.lua`」（实为 41 个、已全部内嵌进驱动脚本尾部，该目录仅余 `regmap.lua`），章节标题同误；「使用示例」的 `dofile("modules/lib/virtbus/mpu6050.lua")` 指向已删除的文件，照抄必报文件不存在——改为 `dofile("modules/MPU6050.lua")` + 全局 `MPU6050_VIRT` 工厂。`proto=virt` 段的挂载描述补上「外置缺失则回退内嵌工厂」的真实装载顺序。232 KB 单文无导航，顶部加分节目录与器件定位提示（正文条目与对照表未改动）。
- **UART 波特率从未被设置（真机缺陷，由新增 ABI 交叉校验抓出）**：glibc 的 `cfsetispeed`/`cfsetospeed` 收 termios `speed_t`（`B9600` = 13）而非 Hz，`uart_open` 却直传 `9600` 且**不查返回值**——实测 `cfsetispeed(&t, 9600)` 返回 `-1`/`EINVAL` 且不改动 `c_ispeed`，随后 `tcsetattr` 拿着「未改速率」的结构体成功写回，于是串口静默停在驱动当前速率上（MHZ19/SDS011 等 9600 器件在真机上收发不对速）。HAL 头注释「现代 glibc 的 B* 即数值本身」为错误前提，`test_uart_consts.c` 早已打印出 `B9600=13` 却无人比对。现加 `HAL.UART_BAUD_SPT` 换算表（11 档，Linux UAPI 各架构同值），并检查两个 cfset* 返回值——失败即关 fd 返回 `-1`，不再把未设速率的配置写回设备。配套：`test/test_uart.lua` 的 `cfsetispeed` 桩补返回值（否则新校验会让全部用例如实失败）、断言由「收到数值波特率 9600」改为「收到 speed_t 13」并加「不得再收 Hz」与失败注入路径（**旧断言正是把缺陷固化进测试的那一处**）；`test_abi_consts.lua` 整表比对映射值同 `<termios.h>`。
- **`$` 引用列指向 `@Alias` 变量时抛裸 `JsonException`（P1，2026-09 审计）**：`MetaProvider.seed()` 的引用校验点接受任意已注册硬件变量为引用目标，而 `collect()` 假定被引用变量的 `read()` 返回字段表——`MtVars_Alias.read()` 返回单个 `JsonString`，`asObject()` 直接抛 `Fail to convert to JsonObject`，无表/列/行上下文（与同校验点「引用未定义变量」的显式报错文案不对称）；build 失败后 `deactivate` 的 destroy 路径再走一次 `getMetadata()`，同一异常以 `Deactive warning` 形式重复打印。现于 `seed()` 识别被引用者为 `@Alias` 并抛 `IllegalArgumentException`，文案给出表/列/行/值与正确写法（别名用于展开 `@Action` 列头参数名，不作元数据值）。配套：`test/run_all.sh` 新增 @Alias 专项端到端 4 例（`alias_inline`/`alias_override`/`alias_dup`/`alias_ref`，退出码 + 输出 ERE 双重锚定，并显式禁止 `Fail to convert to JsonObject` 形态）——此前 `scripts/test/` 下三个别名夹具从未被任何测试调用；全套 97 → 101 项。
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
