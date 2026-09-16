# bot4cj.cpp（mybot）——C++17 全量移植版机器人控制器

> **一句话**：把机器人的「结构、驱动、动作」拆成三份脚本，给它一份动作剧本，它就能照着"演"出来——而且**不接任何硬件也能在开发机上跑通全流程**。

本仓库是 [bot4cj](…/bot4cj.new)（仓颉语言版）的 **C++17 全量移植**：SQL 结构脚本（`.sql`）、Lua 驱动（`.lua`）、动作脚本（`.mt`）三份脚本的设计与用法完全不变，底层运行时由仓颉运行时换成原生 C++（`std::thread` + LuaJIT C API），构建从 `cjpm` 换成 `cmake`。

| 脚本 | 像什么 | 负责什么 |
| ---- | ------ | -------- |
| 结构脚本 `.sql` | 舞台的**接线图** | 机器人身上有哪些零件、接在哪条总线/哪个地址、谁听谁的 |
| 驱动脚本 `.lua` | 零件的**说明书** | 每个零件怎么点亮、怎么读数（I2C / SPI / UART 都藏在这一层，用 LuaJIT 编写） |
| 动作脚本 `.mt` | 演员的**剧本** | 一行就是一个时间点的动作，逐行播放就是一段表演 |

## 项目信息

- **实现**：C++17（`fmt` 无关，零第三方 C++ 依赖，仅 LuaJIT + pthread + dl）
- **语言生态**：LuaJIT 2.1（Lua C API 直调，`modules/lib/HAL.lua` 用 ffi 直调 glibc）
- **构建**：CMake ≥ 3.16；测试环境 g++ 15.3
- **平台**：Linux（aarch64 / x86_64），树莓派实测可使用

## 🧭 三份脚本如何配合

```text
结构脚本 .sql → 定义零件清单（舵机、传感器…）
                  │ 每个零件配一份说明书（自动加载）
驱动脚本 .lua → 教计算机读写这个零件
                  ↑ 按剧本逐行调用
动作脚本 .mt  → 第 1 行做什么 → 第 2 行做什么 → …
```

**不接硬件也能跑**：仓库内置"假零件"（Relay 转发链路、内存仿真虚拟总线），第一条命令就能看到整条链路真实运转。

## 🚀 快速开始

### 安装依赖

```bash
sudo apt update && sudo apt install cmake g++ libluajit-5.1-dev
```

### 构建

```bash
cmake -S . -B build && cmake --build build          # Release（产物 ./build/mybot）
# 排查内存/未定义行为（可选）：
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  && cmake --build build-asan                        # 产物 ./build-asan/mybot
```

### 第一条命令：无硬件跑通 Relay → Timer

`relay-test` 是一台"玩具电梯"，**不需要接任何硬件**：`Relay` 把 `test` 参数转成 `delay`，经 controller 机制转发给 `Timer` 延时 200ms，用来确认环境装好了、程序能跑了。**新手跑通这一条就算入门**：

```bash
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql
```

看到 `Relay: test=200 note=hello-relay -> delay=200` 且正常退出 0，环境就没问题了。开调试开关可以看每一步的参数与结果转储：

```bash
BOT4CJ_DEBUG=1 cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql
```

### 一键回归测试

```bash
bash test/run_all.sh          # 全部：Lua 单测 + virt E2E + 无硬件契约 + TCP E2E
bash test/run_all.sh pca9685  # 文件名过滤
bash test/run_all.sh tcp      # 只跑 TCP 动作源
```

守门共四段，全部在无 /dev/i2c 的开发机上运行：

1. **Lua 离线单测**（`test/test_*.lua`，LuaJIT 运行；真机 i2c 用例自动跳过）
2. **virt 端到端**（`robots/test/*_virt.sql`，23 例走内存仿真总线 `virtbus`，断言 `:f` 浮点透传与守护持续发布）
3. **无硬件契约回归**（守护/别名/转发/引用路径；负值契约如 INSERT 拼错表、别名重复、`$` 引用标量必须**清晰报错**而非裸崩）
4. **TCP 动作源回环**（复用 `scripts/tcp_e2e.sh`，随机端口，缺 python3 自动跳过）

Sanitizer 二进制同样可跑全部 E2E：`BOT=./build-asan/mybot bash test/run_all.sh _virt`。

### 接第一块 PCA9685

- 接线：树莓派 `i2c-1`（SDA1/SCL1）→ PCA9685，地址 `0x40`
- 参照 `robots/spider1.sql` 写自己的结构脚本：I2C 表 → PCA9685 表 → Servo 表（controller 列指向 `pca9685-0`）
- 跑站立动作：

```bash
cat scripts/std.mt | ./build/mybot --robot=robots/spider1.sql
```

## 📱 运行示例

`scripts/` 下的动作脚本适配 `robots/spider1.sql` 的 14 路舵机结构（通道分配见该文件注释）：

```bash
cat scripts/std.mt        | ./build/mybot --robot=robots/spider1.sql  # 站立
cat scripts/left-turn.mt  | ./build/mybot --robot=robots/spider1.sql  # 左转
cat scripts/tilt-r.mt     | ./build/mybot --robot=robots/spider1.sql  # 右倾
cat scripts/one-shake.mt  | ./build/mybot --robot=robots/spider1.sql  # 单腿姿势
```

动作输入源三选一（`src/actionsource.cpp` 工厂，优先级 TCP > 文件 > stdin）：

```bash
# 1) TCP 长驻模式：监听端口等待客户端下发 .mt（适合上位机/远程持续喂动作）
./build/mybot --robot=robots/relay-test.sql --tcp 19240

# 2) 文件输入：不依赖管道
./build/mybot --robot=robots/relay-test.sql --input scripts/relay-test.mt

# 3) 缺省：stdin 管道（前两节示例的写法）
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql
```

## 🔧 环境要求

| 组件 | 说明 | 状态 |
| --- | --- | --- |
| Linux | HAL 经 ffi 直调 glibc（open/ioctl/read/write/usleep） | ✅ 必需 |
| CMake ≥ 3.16、C++17 编译器 | g++ / clang++ | ✅ 必需 |
| LuaJIT 2.1（`libluajit-5.1-dev`） | 驱动与 HAL 全部在 LuaJIT 上 | ✅ 必需 |
| Python 3 | 仅 `scripts/tcp_e2e.sh` 的 TCP 客户端需要（缺则守门自动跳过） | ⭕ 可选 |
| `/dev/i2c-*` | 真机总线 | ⭕ 可选（无总线时全流程可用内存仿真跑） |

## 📚 核心概念与 DSL

正式说法：结构脚本 `.sql` 以 SQL 方言声明硬件结构与调用链，驱动脚本 `.lua` 在 LuaJIT 里实现每个硬件的底层驱动，动作脚本 `.mt` 以行列式 DSL 编排动作。

### 结构脚本（.sql）

由内置 SQL4cj 解析器（`src/sqlparse.cpp`）解析，支持 `--` 行注释与 `/* */` 块注释。一组 `CREATE TABLE / INSERT INTO`：每张表对应一类硬件（表名即矩阵名，决定加载哪个驱动，`modules/<矩阵名>.lua`），每行 INSERT 就是一个实例：

```sql
CREATE TABLE I2C (name String, path String);
INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');

CREATE TABLE PCA9685 (name String, I2C String, address String);
INSERT INTO PCA9685 (name, I2C, address) VALUES ('pca9685-0', '$i2c-1', '0x40');

CREATE TABLE Servo (name String, max String, min String, offset String,
                    controller String, channel String);
INSERT INTO Servo (name, max, min, offset, controller, channel)
  VALUES ('s1', '90', '-', '-', 'pca9685-0', '0');   -- 左下水平关节
```

要点：

- `-` 是占位符，继承上一行同列的值
- `I2C` 列以 `$i2c-1` 引用上方定义的总线（引用列必须 `$` 前缀，裸名一律按字面值）
- 注释整行 INSERT 即可下线一个硬件实例；报错会指明是哪张表、哪一行、哪一列

**引号字面量**：`'` / `"` 包裹的值整体按字面解析——引号内的 `--`、`/* */`、`;`、`,`、`(`、`)` 与空白都是**数据**而非注释/分隔符（注释只在引号外生效）。这是 1.x 起修复的词法器行为，有负值回归守门（`robots/test/sqlquote.sql`）。

### 硬件变量一览（`modules/`，50+ 驱动）

| 类别 | 硬件变量 |
| ---- | -------- |
| 舵机/输出 | `PCA9685`、`Servo`、`Timer`、`MCP4725`、`AD9833`、`TPL0501`、`PCF8574`、`GPIO`、`PWM` |
| IMU/姿态 | `MPU6050`、`BMI270`、`LSM6DSV`、`ADXL345`、`MLX90393`、`BMM150` |
| 气压/环境 | `MS5611`、`LPS22HB`、`BME280`、`BME688`、`SHT3X`、`CHT8305`、`AMG8833`、`AS7262`、`APDS9900`、`BH1745` |
| 测距 | `VL53L0X`、`VL53L4CD`、`AS3935` |
| ADC/电源 | `ADS1015`、`ADS1115`、`ADS1256`、`MCP3421`、`INA219`、`INA226`、`INA3221`、`MAX17043`、`MAX17048`、`LTC2944`、`EMC2101` |
| 气体/其他 | `SGP30`、`SDS011`、`MHZ19`、`MAX30102`、`MT6826S` |
| 框架 | `I2C`、`SPI`、`UART`、`Relay`、`dummy`、`Kalman`、`Ema`、`Monitor` |

### 动作脚本（.mt）

用一张表格描述一帧一帧的动作，表头是「硬件变量.参数」，每行一帧，逐行播放：

```mt
# 行动矩阵为硬件变量输入参数
@Action
s1.A  s2.A  spk.F  spk.delay;   # column name
  45    45     10      200;      # stand
  90     -      -       20;      # left-behind
@
```

- 可内联 `@Alias` 别名矩阵（如把 `s1.A` 写成 `s1.angle`）
- 结构与动作两侧都可定义别名，同名时**动作脚本的定义覆盖**（打印覆盖日志）
- 同一动作脚本内重复定义同名别名报错（负值契约）

### controller 引用（转交任务）

某个硬件收到指令后先干一份活，再把结果转交给另一个硬件：

1. 先运行自己的 Lua 驱动 `call`
2. 框架把「该行元数据（跳过 `name`/`controller` 列）∪ 自己 Lua 返回表」发给 `controller` 指定的硬件变量
3. `controller` 支持 `-` 继承上一行；被引用方必须在结构脚本中先定义

### `$` 引用列

元数据列可以按值引用另一个硬件变量：值以 `$` 开头（如 `$imu0`），被引用方的 `read()` 快照字段实时合并进本变量元数据；**不带 `$` 的裸名一律按字面值**。`$` 指向未定义变量在 build 期显式报错；指向**标量**（如 Alias 的 JsonString）也会清晰报错——标量引用请走 `controller` 列：

```sql
CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$imu0');   -- 引用守护 imu0
```

### HAL（基层硬件抽象）

总线怎么"开口说话"封装在 `modules/lib/HAL.lua`，所有驱动共用，**仅支持 LuaJIT**：

- ffi 直调 glibc，原生性能零 fork；fd 为真实内核句柄，跨 Lua 状态机有效
- I2C 经 `I2C_RDWR` ioctl **原子事务**：读的「写指针 + 读」合并为单次内核传输，消除并发访问同一器件时的寄存器指针竞态；地址由每条 i2c_msg 携带，不再需要 `I2C_SLAVE`（没有 EBUSY 问题）
- `modules/lib/` 还收纳 `kalman.lua` 等公共库；不依赖 HAL 的模块可用 `luajit`/`lua5.4` 独立调试

### @Daemon 守护线程（后台采样）

有些传感器要**一刻不停地读**（如 IMU）。`@Daemon` 用 C++ `std::thread` 起一个后台线程循环采样，主线程随取随读：

```sql
CREATE TABLE Daemon (name String, chunk String);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'lib/daemon_probe.lua');
```

并发模型关键点：

- 每个守护 = 独立线程 + **独立 Lua 状态**，规避 LuaJIT VM 非线程安全
- 共享内存槽 + pthread 互斥锁：守护侧 `P.publish(slot,{...})` 写，主线程侧 `play()` 加锁读
- chunk 先跑一次完成 setup 并定义全局 `daemon_tick`，之后逐拍 `pcall`；**拍间用线程 sleep 让出**，切忌在 chunk 内 ffi `usleep` 自旋（阻塞调度不放）
- `@Daemon` 除 `name`/`chunk` 外的列序列化为 `DAEMON_CONFIG` 串注入 chunk

**万能采样 chunk（零 per-sensor 代码）**：`lib/sensor_daemon.lua` 服务任意 build/call/destroy 型驱动：

```sql
CREATE TABLE Daemon (
	name String, chunk String, driver String, proto String,
	path String, addr String, interval String, fields String, sign String
);
INSERT INTO Daemon (name, chunk, driver, proto, path, addr, interval, fields, sign)
  VALUES ('imu0', 'lib/sensor_daemon.lua', 'MPU6050', 'i2c',
          '/dev/i2c-1', '0x68', '10', 'ACCEL_X|ACCEL_Y|ACCEL_Z|GYRO_X|GYRO_Y|GYRO_Z|TEMP:f', '16');
```

`fields` 用 `|` 分隔以避开 SQL 逗号；`sign=16` 表示 16 位补码符号扩展；逐字段后缀可覆盖（`:f` 原样透传浮点，`:s16` 强制补码）。

**滤波器硬件变量**（虚拟传感器）：每个滤波器 = `lib/<filter>.lua` 库（`.new()` + `:filter(x)`）+ `<Filter>.lua` 驱动：

- `Kalman`：一维卡尔曼，配 `Q`/`R`
- `Ema`：指数移动平均，配 `alpha`∈(0,1]

三级引用链 `imu0 → kf0 → mon0` 靠既有引用机制自动串联（播放 mon0 会递归播放 kf0、imu0）。

> 注意：守护 setup 需数毫秒；动作序列极短时可能守护尚未首发就被停止（publishes=0）。真实动作序列为秒级没问题；测试用 Timer 延时列拉长主线程时间窗。

### 虚拟总线（virtbus，无真机验证驱动链路）

`lib/virtbus.lua` 注入后替换 HAL 的全部 `i2c_*`/`spi_*` 全局函数，真实驱动无需真机即可跑通 build/call/destroy，每次事务记入 `txlog` 供断言，可注入 NACK 负路径。器件模型已内嵌进各驱动文件尾部，成为全局工厂 `<DRIVER>_VIRT`（共 41 个，如 MPU6050、BME280、BH1750、ADS1256、ADXL345、SHT3X、VL53L0X、VL53L4CD、INA219/226/3221、SGP30、BME688、BMI270 等），随 `load_driver()` dofile 驱动一并定义；`lib/virtbus/` 目录现仅保留通用底座 `regmap.lua`。

`@Daemon` 的 `proto` 列填 `virt` 即可做**守护无硬件端到端**：真实驱动 build/call、共享槽 publish、`@Ema/@Kalman` 滤波、`@Monitor` 消费全链路走内存仿真（启动打印 `VIRTUAL HARDWARE` 横幅，仿真数据禁止用于生产）。37 例已接入 `run_all.sh`：

```bash
cat scripts/test/mpu_virt.mt    | ./build/mybot --robot=robots/test/mpu_virt.sql
cat scripts/test/bme280_virt.mt | ./build/mybot --robot=robots/test/bme280_virt.sql
```

### 环境变量

| 变量 | 说明 |
| --- | --- |
| `BOT4CJ_MODULE_PATH` | 驱动模块目录（默认 `./modules`），`<矩阵名>.lua`、`lib/`、`@Daemon` 的 chunk 都按它解析；支持 `:` 分隔多路径按序查找 |
| `BOT4CJ_DEBUG` | 设为 `1` 开启 call/build/destroy 的参数表、结果表、逐键值转储；缺省静默 |

（仓颉原版另有 `BOT4CJ_NO_GC` 长脚本开关，用于规避仓颉运行时并发 GC 缺陷——本 C++ 移植版无仓颉运行时、不存在该缺陷，未实现此变量。）

**沙箱**：加载驱动后中和 `os.execute`/`io.popen`（shell）、`os.exit`、`io.open`/`io.lines`/`io.input`/`io.output`/`os.remove`/`os.rename`（任意文件读写）。这是**防误用护栏，不是安全边界**——驱动可经 `require("ffi")` 直调任意 libc，脚本均为可信输入；接入 `--tcp` 外部数据源时需另行建立信任边界。

## 💡 进阶示例：守护采样 → 滤波 → 消费闭环

```sql
CREATE TABLE Daemon (name String, chunk String);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'lib/sensor_daemon.lua');   -- 采样

CREATE TABLE Kalman (name String, Q String, R String, src String, field String);
INSERT INTO Kalman (name, Q, R, src, field) VALUES ('kf0', '20', '200', '$imu0', 'v0');

CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$kf0');   -- 消费滤波值
```

`modules/Monitor.lua` 即此类示例：`call()` 里 `data.v0` 拿到实时采样值，决策后可经 controller 转发执行器。

## ⚠️ 已知限制

- **仅支持 Linux**：HAL 经 ffi 直调 glibc，Windows/macOS 未支持
- **仅支持 LuaJIT**：总线驱动依赖 ffi（不依赖 HAL 的模块可用 lua5.4 独立调试）
- **守护线程开销**：每个守护一个独立线程 + 独立 Lua 状态，拍间必须让出式 sleep
- **一器件一消费者**：页式寄存器序列器件（如 VL53L0X）与握手式间接访问器件（如 AS7262）只支持单消费者
- 与仓颉原版不同：本版 **没有** 仓颉运行时并发 GC 缺陷，超长动作脚本不受该问题影响

## 🔧 开发与贡献

```bash
git clone <本仓库> && cd bot4cj.cpp
cmake -S . -B build && cmake --build build
bash test/run_all.sh                          # 离线回归（无硬件）
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql   # 无硬件自检
```

- 新增驱动：在 `modules/` 加一个 `<矩阵名>.lua`（dofile HAL，实现 build/call/destroy 语义）
- 新增回归：`robots/test/<用例>.sql` + `scripts/test/<用例>.mt`，按守门契约接入 `test/run_all.sh` 的 e2e 列表或无硬件契约列表（含负值契约）
- 代码规范：本项目 `.cpp` 全量 `-Wall -Wextra -Wpedantic` 零告警为基线，改动请保持

## 📋 更新日志

- **v1.x（C++ 移植版）**
  - 词法器引号感知：引号内 `--` / `/* */` / `;` / `,` / 括号 / 空白一律按字面数据解析（`sqlquote` 正负回归）
  - `$` 引用指向标量（Alias）改为清晰报错而非 Json 类型崩溃（`ref_alias_scalar` 负值回归）
  - E2E 守门落地：mybot 直跑 + INSERT 目标表校验；无硬件契约与 TCP 动作源全部自动化
  - `deactivate` 同因告警去重；`scripts/run.sh` 启动器与文档一致性清理

## 📄 开源协议

本项目基于 MIT 协议开源（见 [LICENSE](LICENSE)）。

## 🙋 常见问题

### Q: 不接硬件也能开发吗？

A: 可以。`bash test/run_all.sh` 用内存仿真（virtbus）与桩替身跑全部离线回归；`relay-test` 链路不需要任何硬件，先跑通它即可验证环境。

### Q: 守护线程 publishes=0？

A: 动作序列极短（几行瞬时 play）时，守护可能在首次采样前就被停止。真实动作序列为秒级没问题；测试时用 Timer 延时列拉长主线程时间窗。

### Q: I2C 设备报 EBUSY？

A: HAL 已用 `I2C_RDWR` 原子事务，地址由每条 i2c_msg 携带，不需要 `I2C_SLAVE` ioctl，一般不会再有内核驱动占用地址导致的 EBUSY。

### Q: 报错提示看不懂怎么办？

A: 结构脚本报错会指明是哪张表哪一行哪一列；动作脚本报错会说明是哪个硬件变量与参数。按提示修正即可。