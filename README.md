# MYBOT机器人控制器

> **一句话**：把机器人的「结构、驱动、动作」拆成三份脚本，给它一份动作剧本，它就能照着"演"出来——而且**不接任何硬件也能在开发机上跑通全流程**。

本仓库是 bot4cj（仓颉语言版）的 **C++17 全量移植**：SQL 结构脚本（`.sql`）、Lua 驱动（`.lua`）、动作脚本（`.mt`）三份脚本的设计与用法完全不变，底层运行时由仓颉运行时换成原生 C++（`std::thread` + LuaJIT C API），构建从 `cjpm` 换成 `cmake`。

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
bash test/run_all.sh          # 全部：Lua 单测 + virt E2E + 无硬件契约 + TCP E2E + C++ 单测
bash test/run_all.sh pca9685  # 文件名过滤
bash test/run_all.sh tcp      # 只跑 TCP 动作源
```

守门共六段，全部在无 /dev/i2c 的开发机上运行：

1. **Lua 离线单测**（`test/test_*.lua`，LuaJIT 运行；真机 i2c 用例自动跳过）
2. **virt 端到端**（`robots/test/*_virt.sql`，37 例走内存仿真总线 `virtbus`，断言 `:f` 浮点透传与守护持续发布）
3. **无硬件契约回归**（守护/别名/转发/引用路径；负值契约如 INSERT 拼错表、别名重复、`$` 引用标量、动作参数覆盖结构元数据必须**清晰报错**而非裸崩；含 `PCA9685` 的**非守护** `@Action` 写路径正/负两条——虚拟总线已不再只服务守护路径）
4. **TCP 动作源回环**（复用 `scripts/tcp_e2e.sh`，随机端口，缺 python3 自动跳过）
5. **输入边界加固**（`--tcp-host` 非法地址、动作脚本超 8 MiB 上限、`@Daemon` 驱动误配无 `build` 时的回滚路径；`mode` 键不得改写 controller 派发目标；驱动 `call()` 漏写 `return` 不得被当成成功；驱动返回自引用表须清晰报错而非宿主栈溢出）
6. **C++ 层单测**（`test/cpp/`，`mybot_tests` 目标；零第三方框架，覆盖 JSON 值语义、`.mt` 矩阵状态机、SQL 词法/语法、元数据收集与 `$` 引用环检测、Lua↔Json 边界与转换深度上限）

Sanitizer 二进制同样可跑全部 E2E 与 C++ 单测：`BOT=./build-asan/mybot BOT_TESTS=./build-asan/mybot_tests bash test/run_all.sh _virt`。
单独跑 C++ 单测：`./build/mybot_tests [用例名过滤]`。

**守门完整性**：编译产物（`mybot` / `mybot_tests`）缺失时，对应分段**判失败**而不是静默跳过——否则「不构建」也能报出「0 失败」，而实际只有 Lua 段在跑。仅两类环境软依赖按跳过处理：真机 i2c 用例（本机无 `/dev/i2c-*`）与 TCP 段缺 `python3`；汇总行如实体报 `N 通过, N 失败, N 跳过` 并列出未执行清单。只想跑 Lua 段：`ALLOW_SKIP=1 bash test/run_all.sh`（显式指定的 `BOT=` / `BOT_TESTS=` 指向不存在的文件时一律判失败，`ALLOW_SKIP` 也不放行）。

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

## 📚 核心概念速览

正式说法：结构脚本 `.sql` 以 SQL 方言声明硬件结构与调用链，驱动脚本 `.lua` 在 LuaJIT 里实现每个硬件的底层驱动，动作脚本 `.mt` 以行列式 DSL 编排动作。

### 结构脚本（.sql）

每张表对应一类硬件（表名决定加载 `modules/<矩阵名>.lua` 驱动），每行 INSERT 就是一个实例：

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

速记：`-` 继承上一行同列的值；`$` 前缀引用已定义的硬件变量；报错会指明表/行/列。

### 动作脚本（.mt）

表头是「硬件变量.参数」，每行一帧，逐行播放：

```mt
# 行动矩阵为硬件变量输入参数
@Action
s1.A  s2.A  spk.F  spk.delay;   # column name
  45    45     10      200;      # stand
  90     -      -       20;      # left-behind
@
```

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

### 进阶机制（一句话版）

- **controller 引用**：硬件干完自己的活，把结果转交给另一个硬件继续处理
- **`$` 引用列**：按值引用另一硬件变量，其实时快照合并进本变量元数据
- **@Daemon 守护线程**：独立线程 + 独立 Lua 状态后台采样，主线程随取随读；支持 `Kalman`/`Ema` 滤波链
- **virtbus 虚拟总线**：内存仿真 I2C/SPI/UART，真实驱动无需真机即可跑通 build/call/destroy。注入不限于守护路径——任何驱动行声明 `proto='virt'` 即由 `lib/drv_common.lua` 的入口前置在该变量自己的状态机里注入，并挂上驱动文件内嵌的 `<矩阵名>_VIRT` 模型（`@Action` 直写的舵机/DAC 走这条路；总线行与器件行都需声明，器件缺模型时报错而非静默回落真机）
- **沙箱**：驱动加载后中和 shell/文件读写等危险 API——防误用护栏，不是安全边界

### 环境变量

| 变量 | 说明 |
| --- | --- |
| `BOT4CJ_MODULE_PATH` | 驱动模块目录（默认 `./modules`），`<矩阵名>.lua`、`lib/`、`@Daemon` 的 chunk 都按它解析；支持 `:` 分隔多路径按序查找 |
| `BOT4CJ_DEBUG` | 设为 `1` 开启 call/build/destroy 的参数表、结果表、逐键值转储；缺省静默 |

（仓颉原版另有 `BOT4CJ_NO_GC` 长脚本开关，用于规避仓颉运行时并发 GC 缺陷——本 C++ 移植版无仓颉运行时、不存在该缺陷，未实现此变量。）

## 📖 深入文档（docs/）

- [DSL 参考：结构脚本与动作脚本](docs/dsl.md)——占位符、引号字面量、`@Alias`、controller 与 `$` 引用完整语义
- [@Daemon 守护线程](docs/daemon.md)——并发模型、万能采样 chunk、滤波器链路、采样→滤波→消费闭环示例
- [HAL 硬件抽象层](docs/hal.md)——ffi 直调 glibc、I2C_RDWR 原子事务、驱动沙箱
- [虚拟总线 virtbus](docs/virtbus.md)——无真机验证驱动链路的完整参考手册
- [架构分析](docs/architecture_analysis.html)——分层图与数据流图（含 [layers](docs/architecture_layers.svg) / [dataflow](docs/architecture_dataflow.svg)）

## ⚠️ 已知限制

- **仅支持 Linux**：HAL 经 ffi 直调 glibc，Windows/macOS 未支持
- **仅支持 LuaJIT**：总线驱动依赖 ffi（不依赖 HAL 的模块可用 lua5.4 独立调试）
- **守护线程开销**：每个守护一个独立线程 + 独立 Lua 状态，拍间必须让出式 sleep
- **一器件一消费者**：页式寄存器序列器件（如 VL53L0X）与握手式间接访问器件（如 AS7262）只支持单消费者
- 与仓颉原版不同：本版 **没有** 仓颉运行时并发 GC 缺陷，超长动作脚本不受该问题影响

## 🔧 开发与贡献

```bash
git clone <本仓库> && cd bot4cj
bash scripts/install-hooks.sh                 # 装 pre-push 守门钩子（一次性）
cmake -S . -B build && cmake --build build
bash test/run_all.sh                          # 离线回归（无硬件）
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql   # 无硬件自检
```

- 守门钩子：`scripts/git-hooks/pre-push` 是版本管理的正身，`install-hooks.sh` 以**软链**方式接进 `.git/hooks/`（不用 `core.hooksPath`——那会连带忽略该目录里已有的其它钩子；不用拷贝——避免源更新后留下过期副本）。装后每次 `git push` 前自动跑 `test/run_all.sh`，未通过即中止。单次绕过：`git push --no-verify`
- 新增驱动：在 `modules/` 加一个 `<矩阵名>.lua`（dofile HAL，实现 build/call/destroy 语义）
- 新增回归：`robots/test/<用例>.sql` + `scripts/test/<用例>.mt`，按守门契约接入 `test/run_all.sh` 的 e2e 列表或无硬件契约列表（含负值契约）
- 新增 C++ 单测：在 `test/cpp/test_*.cpp` 用 `TH_TEST` / `TH_CHECK` / `TH_CHECK_THROWS_MSG` 补用例（`test/cpp/harness.hpp`，无第三方框架），并在 `CMakeLists.txt` 的 `mybot_tests` 源列表里加一行
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