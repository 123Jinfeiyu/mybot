# MYBOT机器人控制器

> **一句话**：把机器人的「结构、驱动、动作」拆成三份脚本，给它一份动作剧本，它就能照着"演"出来——而且**不接任何硬件也能在开发机上跑通全流程**。

本仓库是 bot4cj（仓颉语言版）的 **C++17 全量移植**：SQL 结构脚本（`.sql`）、Lua 驱动（`.lua`）、动作脚本（`.mt`）三份脚本的设计与用法完全不变，底层运行时由仓颉运行时换成原生 C++（`std::thread` + LuaJIT C API），构建从 `cjpm` 换成 `cmake`。

| 脚本 | 像什么 | 负责什么 |
| ---- | ------ | -------- |
| 结构脚本 `.sql` | 舞台的**接线图** | 机器人身上有哪些零件、接在哪条总线/哪个地址、谁听谁的 |
| 驱动脚本 `.lua` | 零件的**说明书** | 每个零件怎么点亮、怎么读数（I2C / SPI / UART 都藏在这一层，用 LuaJIT 编写） |
| 动作脚本 `.mt` | 演员的**剧本** | 一行就是一个时间点的动作，逐行播放就是一段表演 |

## 项目信息

- **实现**：C++17（零第三方 C++ 依赖，仅 LuaJIT + pthread + dl）
- **构建**：CMake ≥ 3.16；g++ ≥ 14 已验证（CI 基线 debian:bookworm，本地实测至 g++ 15）
- **平台**：Linux（aarch64 / x86_64），树莓派实测可使用
- **驱动生态**：`modules/` 下 50+ 硬件驱动（舵机、IMU、气压、测距、ADC、电源……），分类清单见 [docs/drivers.md](docs/drivers.md)

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
# 排查内存/未定义行为（可选，MYBOT_SANITIZE 为仓库内置开关）：
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMYBOT_SANITIZE=address,undefined && cmake --build build-asan   # 产物 ./build-asan/mybot
```

### 第一条命令：无硬件跑通 Relay → Timer

`relay-test` 是一台"玩具电梯"，**不需要接任何硬件**：`Relay` 把 `test` 参数转成 `delay`，经 controller 机制转发给 `Timer` 延时 200ms，用来确认环境装好了、程序能跑了。**新手跑通这一条就算入门**：

```bash
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql
```

看到 `Relay: test=200 note=hello-relay -> delay=200` 且正常退出 0，环境就没问题了。开调试开关 `BOT4CJ_DEBUG=1` 可以看每一步的参数与结果转储（环境变量详见 [docs/reference.md](docs/reference.md)）。

### 一键回归测试

```bash
bash test/run_all.sh          # 全部离线守门（分段明细见 docs/testing.md），开发机无硬件可跑
bash test/run_san.sh          # 双跑：Release + ASan/UBSan 两套产物各跑全量
bash test/run_all.sh pca9685  # 文件名过滤
```

守门分段明细、守门完整性规则与 sanitizer 跑法见 [docs/testing.md](docs/testing.md)。

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

动作输入源三选一（TCP 单连接 / 文件 / stdin 管道），完整参数与环境变量见 [docs/reference.md](docs/reference.md)。

## 🤖 agent 模式（可选）

管道模式是「给它一份剧本，演一遍就下台」；**agent 模式**（`--agent`）让**大模型工具循环成为主执行流程**：
机器人结构脚本加载后硬件**常驻内存**，智能体通过工具挑选并执行动作脚本，多轮决策后才退出。

```bash
export BOT4CJ_LLM_KEY=sk-xxx
./build/mybot --robot=robots/relay-test.sql --agent --task "让继电器动作一次"  # 一次性（无硬件自检）
./build/mybot --robot=robots/spider1.sql --agent                             # 交互 REPL
```

智能体可用工具：`list_action_scripts`（枚举 .mt）、`run_action_script`（在常驻机器人上执行）、
`list_hardware`（器件清单）、`run_command`（shell，可用 `--agent-no-shell` 关闭）。
完整选项、工作流程、安全说明与源码分层见 **[docs/agent.md](docs/agent.md)**。

**模型调用是一颗 Lua 协议驱动**（和舵机、传感器平级），所以宿主不链接任何 HTTP 库——
「零第三方 C++ 依赖」在这条路径上依然成立；换模型/换厂商 = 换一个 Lua 文件：

```bash
./build/mybot --robot=robots/spider1.sql --agent                        # 缺省 LlmOpenAI（当前协议）
DEEPSEEK_API_KEY=sk-xxx ./build/mybot --robot=robots/spider1.sql \
    --agent --llm-driver LlmDeepSeek                                    # 换 DeepSeek 协议
```

内置 `LlmOpenAI` 与 `LlmDeepSeek`（思考模式 / `reasoning_content` 回传 / 采样参数限制都在驱动里）。
运行期需要 `libcurl.so`（`apt install libcurl4`，Raspberry Pi OS 通常已装）；缺它时驱动会给出清晰报错且机器人不上电。

**目标 URL、API key、模型等信息放配置文件里**（模板 `mybot.llm.json.example`）：

```bash
cp mybot.llm.json.example mybot.llm.json && chmod 600 mybot.llm.json   # 填 endpoint/model/api_key
./build/mybot --robot=robots/spider1.sql --agent                        # 自动查找 ./mybot.llm.json
```

取值优先级：**命令行 > 环境变量 > 配置文件 > 驱动内置缺省**。字段表、查找顺序与严格校验规则见 [docs/agent.md](docs/agent.md)。

## 📖 深入文档（docs/）

从 [docs/README.md](docs/README.md) 进入全部文档。常用入口：

- [命令行与环境变量参考](docs/reference.md)——动作输入源、CLI 参数、安全约束、`BOT4CJ_*`
- [DSL 参考：结构脚本与动作脚本](docs/dsl.md)——占位符、`@Alias`/`@Vars`、`$(...)` 表达式、controller 与 `$` 引用完整语义
- [硬件变量（驱动）清单](docs/drivers.md)——50+ 驱动按类别速查
- [@Daemon 守护线程](docs/daemon.md)——后台采样、滤波器链路
- [agent 模式](docs/agent.md)——大模型工具循环驱动常驻机器人：CLI 选项、工具、安全边界、构建依赖
- [HAL 硬件抽象层](docs/hal.md)——ffi 直调 glibc、I2C_RDWR 原子事务、驱动沙箱
- [虚拟总线 virtbus](docs/virtbus.md)——无真机验证驱动链路的完整参考手册
- [架构分析](docs/architecture_analysis.html)——分层图与数据流图
- [回归测试手册](docs/testing.md) / [开发与贡献](docs/development.md) / [已知限制与 FAQ](docs/faq.md) / [更新日志](docs/changelog.md)

## ⚠️ 已知限制

仅支持 Linux + LuaJIT；守护线程有一线程一 Lua 状态的开销；部分器件只支持单消费者。完整列表与排障问答见 [docs/faq.md](docs/faq.md)。

## 🔧 开发与贡献

装 pre-push 守门钩子（`bash scripts/install-hooks.sh`）、跑 `bash test/run_all.sh` 全绿即可提交。新增驱动 / 回归 / C++ 单测的流程见 [docs/development.md](docs/development.md)。

## 📄 开源协议

本项目基于 MIT 协议开源（见 [LICENSE](LICENSE)）。
