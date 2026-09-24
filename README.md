# MYBOT 机器人控制器

**用大模型对话控制真实机器人** —— 说「让机器人站立」，它就站起来；说「拍照看看前面」，它就拍照并告诉你看到了什么。

---

## ✨ 核心特性

🤖 **智能体控制** - 大模型理解自然语言指令，自主选择并执行动作  
🔧 **三层架构** - 结构脚本（`.sql`）+ Lua驱动（`.lua`）+ 动作脚本（`.mt`）完全解耦  
🚀 **零硬件开发** - 虚拟总线 + 仿真驱动，笔记本上就能跑通全流程  
🔒 **工业级安全** - 多重防护、配置隔离、响亮拒绝不静默  
📦 **零C++依赖** - 仅需 LuaJIT + pthread + dl，模型调用是Lua驱动  
✨ **友好体验** - 进度提示动画，响应过程可见不焦虑  

---

## 🎯 快速开始（5分钟上手）

### 1. 安装依赖

```bash
sudo apt update && sudo apt install cmake g++ libluajit-5.1-dev libcurl4
```

### 2. 构建项目

```bash
cmake -S . -B build && cmake --build build
```

### 3. 启动智能体（无需硬件）

```bash
# 配置API密钥与接入点（endpoint 必须显式配置，无内置缺省）
export BOT4CJ_LLM_KEY=sk-xxx
export BOT4CJ_LLM_ENDPOINT=https://你的接入点/v1/chat/completions

# 启动交互式REPL（使用虚拟机器人）
./build/mybot --robot=robots/relay-test.sql --agent
```

**现在你可以直接对话**：

```
mybot[relay-test|llm0|2hw]> 让继电器动作一次
已执行动作脚本 relay-test.mt（42 字节）

mybot[relay-test|llm0|2hw]> /help
内置命令:
  /hardware  - 显示硬件清单
  /scripts   - 显示可用动作脚本清单
  /help      - 显示此帮助
  /exit      - 退出

mybot[relay-test|llm0|2hw]> /exit
```

**看到这个提示符就算成功** → `mybot[机器人名|LLM驱动|硬件数]>`

---

## 🧠 Agent 模式（主要使用方式）

Agent模式让大模型成为机器人的"大脑"，它可以：

✅ **理解自然语言** - 「让机器人走到桌子旁边」自动分解成多个动作  
✅ **自主决策** - 根据传感器数据或摄像头画面判断下一步  
✅ **连续执行** - 硬件常驻内存，可连续执行多个任务  
✅ **视觉感知** - 支持摄像头（`capture_image`工具）看环境  
✅ **工具调用** - 内置工具安全执行动作脚本（查询/拍照/回读；危险开关缺省关闭）  
✨ **进度提示** - 模型响应时显示"思考中..."动画，用户体验友好

### 配置文件方式（推荐）

```bash
# 复制配置模板
cp mybot.llm.json.example mybot.llm.json
chmod 600 mybot.llm.json

# 编辑配置文件，填入你的API信息
{
  "endpoint": "https://api.openai.com/v1",
  "model": "gpt-4",
  "api_key": "sk-xxx",
  "scripts_dir": "scripts",
  "prompt_files": ["prompts/spider_robot"]
}

# 启动（自动读取配置）
./build/mybot --robot=robots/spider1.sql --agent
```

### 环境变量方式（快速测试）

```bash
export BOT4CJ_LLM_KEY=sk-xxx
export BOT4CJ_LLM_ENDPOINT=https://你的接入点/v1/chat/completions
./build/mybot --robot=robots/spider1.sql --agent --llm-model gpt-4
```

### 一次性任务

```bash
./build/mybot --robot=robots/spider1.sql --agent --task "让机器人站立然后左转"
```

### 分段帮助

```bash
./build/mybot --help agent    # Agent模式选项
./build/mybot --help llm      # LLM配置选项
./build/mybot --help camera   # 摄像头选项
```

---

## 📚 三层架构解析

MYBOT采用**演员-剧本-舞台**模型，将硬件、驱动、动作完全解耦：

| 层级 | 文件类型 | 比喻 | 职责 |
|------|---------|------|------|
| **结构脚本** | `.sql` | 舞台接线图 | 机器人有哪些零件、接在哪条总线/地址 |
| **驱动脚本** | `.lua` | 零件说明书 | 每个零件怎么点亮、怎么读数（I2C/SPI/UART） |
| **动作脚本** | `.mt` | 演员剧本 | 一行一个动作，逐行执行 |

### 工作流程

```
┌─────────────┐
│  自然语言   │  "让机器人站立"
└──────┬──────┘
       ↓
┌─────────────┐
│   大模型    │  理解意图 → 选择工具
└──────┬──────┘
       ↓
┌─────────────┐
│ 动作脚本    │  scripts/std.mt（站立动作）
│  .mt        │
└──────┬──────┘
       ↓
┌─────────────┐
│ 驱动脚本    │  modules/Servo.lua（舵机驱动）
│  .lua       │
└──────┬──────┘
       ↓
┌─────────────┐
│ 结构脚本    │  robots/spider1.sql（14路舵机定义）
│  .sql       │
└──────┬──────┘
       ↓
    真实硬件
```

---

## 🛠️ Agent工具系统

智能体自带下列安全工具（**缺省 5 个**；带「显式开启」的按启动开关增减）：

| 工具 | 功能 | 安全级别 |
|------|------|---------|
| `list_action_scripts` | 列出可用动作脚本 | ✅ 只读 |
| `list_hardware` | 显示硬件清单（不读值、无副作用） | ✅ 只读 |
| `query_sql` | 结构 SQL 只读查询/语法校验（`SELECT ... [WHERE 列 = 值 [AND ...]]`、`SHOW TABLES`） | ✅ 只读 |
| `run_action_script` | 执行动作脚本（**回执带驱动返回的读数**） | ⚠️ 写入 |
| `capture_image` | 拍照并送入上下文 | ⚠️ 隐私 |
| `read_variables` | 主动读硬件变量当前值 | ✅ 只读，**需 `--read-var` 声明可读变量** |
| `apply_sql` | 结构 SQL **带电生效**（建表/插行；`DELETE FROM` 当场断电摘行、`DROP TABLE` 撤整张表，**不可逆**；`UPDATE ... SET ...` 按断电重建改写命中行） | 🔴 危险，**需 `--agent-hot-sql`** |
| `run_command` | 执行 shell 命令 | 🔴 危险，**需 `--agent-shell`** |

**安全设计**：
- ✅ 动作脚本只能在 `scripts/` 目录下
- ✅ 不允许符号链接和路径遍历
- ✅ 三个危险/放宽权限的开关（`run_command` / `apply_sql` / 可读变量清单）**缺省全关，且只能命令行给**——配置文件写 `allow_shell: true` / `allow_hot_sql: true`（放宽）或出现 `read_vars` 字段会被**拒绝加载**（写 `false` 合法，模板 `mybot.llm.json.example` 即如此）
- ✅ `read_variables` 只放行已注册且种类为 `lua`/`daemon` 的变量，且只有 `--read-var` 列过的名字可读（执行器与传感器同属 `lua`，靠种类分不出，故由人列名）
- ✅ 所有操作有体积上限和超时限制

---

## 🎬 传统模式（脚本直接执行）

如果你已经有写好的动作脚本，可以直接执行：

```bash
# 从文件读取
./build/mybot --robot=robots/spider1.sql --input scripts/std.mt

# 从管道读取
cat scripts/std.mt | ./build/mybot --robot=robots/spider1.sql

# TCP模式（远程控制）
./build/mybot --robot=robots/spider1.sql --tcp 8080
```

动作脚本示例（`scripts/std.mt`）：
```mt
# 站立动作
servo-lf-hip  angle=90
servo-rf-hip  angle=90
servo-lb-hip  angle=90
servo-rb-hip  angle=90
```

---

## 🔌 硬件支持

### 支持的器件（47款）

**舵机与电机**：PCA9685（16路PWM舵机控制）、SCSCL飞特总线舵机、普通舵机、H03ARR电机驱动  
**传感器**：IMU与磁力计（MPU6050、BMI270、LSM6DSV、ADXL345、MLX90393、BMM150）、气压与环境（MS5611、LPS22HB、BME280、BME688）、测距（VL53L0X/L4CD）、闪电检测（AS3935）  
**总线**：I2C、SPI、UART  
**摄像头**：V4L2 MJPEG（USB摄像头；树莓派CSI摄像头走libcamera、不经此路径）  
**执行器与系统接口**：GPIO、PWM、DAC（MCP4725）、数字电位器（TPL0501）、IO扩展（PCF8574）  

完整清单见 [docs/drivers.md](docs/drivers.md)

### 接真实硬件

以PCA9685舵机驱动板为例：

```sql
-- robots/spider1.sql
CREATE TABLE I2C (name String, path String);
INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');

CREATE TABLE PCA9685 (name String, I2C String, address String);
INSERT INTO PCA9685 (name, I2C, address) VALUES ('pca9685-0', '$i2c-1', '0x40');

CREATE TABLE Servo (name String, max String, min String, offset String,
                    controller String, channel String);
INSERT INTO Servo (name, max, min, offset, controller, channel)
  VALUES ('s1', '90', '-', '-', 'pca9685-0', '0');
```

```bash
# 接线：树莓派 I2C-1 (SDA1/SCL1) → PCA9685 (地址 0x40)
# 运行
./build/mybot --robot=robots/spider1.sql --agent
```

### 虚拟总线（无硬件开发）

**不接任何真实硬件也能跑通全流程**：

```bash
# 使用虚拟继电器测试
./build/mybot --robot=robots/relay-test.sql --agent --task "让继电器动作一次"

# 使用虚拟摄像头测试视觉功能
./build/mybot --robot=robots/spider1.sql --agent --camera-src test.jpg
```

内置 `Relay` 转发链路和内存仿真虚拟总线，所有驱动逻辑真实运转。详见 [docs/virtbus.md](docs/virtbus.md)

---

## 📁 项目结构

```
mybot/
├── src/           # C++17核心：SQL解析、硬件派发、agent模式
├── modules/       # Lua驱动生态（60个模块 + 24个共享库）
│   ├── Servo.lua        # 舵机驱动
│   ├── PCA9685.lua      # 16路PWM驱动板
│   ├── Camera.lua       # 摄像头（V4L2）
│   ├── LlmOpenAI.lua    # OpenAI协议驱动
│   └── lib/             # HAL、virtbus、JSON、滤波器等共享库
├── robots/        # 结构脚本（机器人定义）
│   ├── spider1.sql      # 14路舵机蜘蛛
│   ├── relay-test.sql   # 虚拟继电器（无硬件）
│   └── test.sql         # 测试夹具
├── scripts/       # 动作脚本库（33个动作脚本）
│   ├── std.mt           # 站立
│   ├── left-turn.mt     # 左转
│   └── jump.mt          # 跳跃
├── prompts/       # Agent提示词包
├── test/          # 三层测试（86 Lua + 27 C++ + E2E）
└── docs/          # 深入文档（15个文档）
```

---

## 🧪 测试与验证

### 快速测试

```bash
# 完整回归测试（无需硬件）
bash test/run_all.sh

# 双跑：Release + ASan/UBSan
bash test/run_san.sh

# 只测Agent相关
bash test/run_all.sh agent-all
```

### CI门禁（GitLab CI，`.gitlab-ci.yml`）

✅ **硬门禁**（每次推送必过）：
- 零告警构建（`-Werror`）
- Release全量测试
- ASan/UBSan单跑

⚡ **软门禁**（转硬中）：
- TSan（线程竞争）
- clang + clang-tidy
- 覆盖率报告
- musl libc兼容性

详见 [docs/testing.md](docs/testing.md)

---

## 📖 深入文档

### 快速入口

| 文档 | 内容 |
|------|------|
| [Agent模式完整指南](docs/agent.md) | CLI选项、工具系统、安全边界、提示词包 |
| [命令行参考](docs/reference.md) | 所有CLI参数、环境变量、安全约束 |
| [DSL语言参考](docs/dsl.md) | 结构脚本、动作脚本完整语法 |
| [驱动清单](docs/drivers.md) | 60个模块分类速查 |
| [HAL硬件抽象层](docs/hal.md) | FFI直调glibc、I2C原子事务、沙箱 |
| [守护线程](docs/daemon.md) | 后台采样、滤波器链路 |
| [虚拟总线](docs/virtbus.md) | 无硬件验证驱动链路 |
| [测试手册](docs/testing.md) | 回归测试、sanitizer、覆盖率 |
| [开发指南](docs/development.md) | 贡献流程、新增驱动、单测 |
| [FAQ](docs/faq.md) | 已知限制、排障问答 |
| [更新日志](docs/changelog.md) | 版本演进与变更记录 |

从 [docs/README.md](docs/README.md) 查看完整文档索引。

---

## 🎓 使用示例

### 示例1：自然语言控制

```bash
export BOT4CJ_LLM_KEY=sk-xxx
export BOT4CJ_LLM_ENDPOINT=https://你的接入点/v1/chat/completions
./build/mybot --robot=robots/spider1.sql --agent
```

```
mybot[spider1|llm0|14hw]> 让机器人做一个站立动作
[工具] run_action_script
  script_name: std.mt
[结果] 已执行动作脚本 std.mt（126 字节）

机器人已完成站立动作，14个舵机已就位。

mybot[spider1|llm0|14hw]> 现在左转
[工具] run_action_script
  script_name: left-turn.mt
[结果] 已执行动作脚本 left-turn.mt（204 字节）

已执行左转动作。

mybot[spider1|llm0|14hw]> /exit
```

### 示例2：带视觉的决策

```bash
./build/mybot --robot=robots/spider1.sql --agent --llm-model gpt-4o
```

```
mybot[spider1|llm0|14hw]> 拍照看看前面有什么
[工具] capture_image
[结果] 已拍摄照片（1920x1080 JPEG）

我看到前面是一个空旷的地面，没有障碍物。建议可以安全前进。

mybot[spider1|llm0|14hw]> 那就前进吧
[工具] run_action_script
  script_name: forward.mt
...
```

### 示例3：一次性任务

```bash
# 无交互，执行完自动退出
./build/mybot --robot=robots/spider1.sql --agent \
  --task "依次执行站立、左转、右转、然后回到初始姿势"
```

---

## 🔒 安全特性

MYBOT在设计时将安全放在首位：

### 配置隔离

✅ **配置错误不碰真机** - 所有配置校验通过才上电  
✅ **API密钥保护** - 不通过argv传递，自动清洗环境变量  
✅ **文件权限检查** - 配置文件权限过大时警告

### 运行时防护

✅ **路径遍历防护** - 拒绝符号链接、相对路径越界  
✅ **输入长度限制** - 所有输入有体积上限，响亮拒绝  
✅ **Shell命令缺省关闭** - `run_command` 只能命令行显式开启  
✅ **超时保护** - LLM调用、子进程执行都有超时限制

### 审计可追溯

✅ **详细日志** - 工具调用与结果默认打印到日志流（stderr），无需开关
✅ **审计轨迹** - `--audit-log <路径>` 落 JSON Lines（工具/模型轮次/会话头尾三类记录）
✅ **人话报错** - 所有错误都给出清晰的上下文和解决方案

详见 [docs/agent.md#安全边界](docs/agent.md)

---

## 🌟 项目亮点

### 技术特性

- **零C++依赖** - 仅需 LuaJIT + pthread + dl + libm，连HTTP库都是Lua FFI实现
- **多模型支持** - 内置 OpenAI / DeepSeek / Anthropic 三套协议，换模型=换lua文件
- **工业级测试** - 580+ C++单测 + 86 Lua测试文件 + 完整E2E场景 + 三道sanitizer门禁
- **跨平台** - Linux aarch64/x86_64，树莓派实测可用

### 架构优势

- **驱动热插拔** - 新增器件不改C++，只写lua驱动
- **动作可复用** - 动作脚本独立于机器人结构
- **结构可迁移** - 同一结构脚本换驱动板零修改

### 生态完整

- **60个驱动模块** - 47款器件驱动 + 3条总线 + 10个框架模块（另有24个共享库）
- **丰富文档** - 15篇文档 + 架构分析报告与两张全景图，从入门到源码分析
- **回归测试** - 86个Lua用例文件 + 27个C++单测文件 + 完整E2E场景，覆盖全链路

---

## 🛠️ 高级用法

### 换模型/换厂商

```bash
# 使用DeepSeek（思考模式）
export DEEPSEEK_API_KEY=sk-xxx
./build/mybot --robot=robots/spider1.sql --agent \
  --llm-driver LlmDeepSeek --llm-model deepseek-chat

# 使用本地Ollama
./build/mybot --robot=robots/spider1.sql --agent \
  --llm-endpoint http://localhost:11434/v1 \
  --llm-model llama3.2 --llm-config none
```

### 自定义提示词

```bash
# 编写提示词包
mkdir -p prompts/my_robot
echo "你是一个谨慎的机器人..." > prompts/my_robot/01-identity.md

# 加载提示词
./build/mybot --robot=robots/spider1.sql --agent \
  --prompt-file prompts/my_robot
```

### TCP远程控制

```bash
# 监听8080端口（需口令）
./build/mybot --robot=robots/spider1.sql --tcp 8080 \
  --tcp-host 0.0.0.0 --tcp-token secret123

# 客户端发送
(echo "secret123"; cat scripts/std.mt) | nc server_ip 8080
```

### 远程对话（agent over TCP）

```bash
# 机器人侧：远程 REPL，对端每行一条任务
./build/mybot --robot=robots/spider1.sql --agent --tcp 9000

# 客户端：先发口令（跨机时），再逐行下发任务
(echo "$TOKEN"; echo "让机器人站立"; sleep 5; echo "/exit") | nc robot-ip 9000
```

缺省只监听回环；跨机必须 `--tcp-token`。对端断开连接即**远程取消**（在飞的模型
调用被立即中止），空闲超时缺省 600 秒——设计取舍见 [docs/agent.md](docs/agent.md)。

### Sanitizer调试

```bash
# 构建asan版本
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMYBOT_SANITIZE=address,undefined
cmake --build build-asan

# 运行
./build-asan/mybot --robot=robots/spider1.sql --agent
```

---

## 🚧 已知限制

- ⚠️ 仅支持 Linux + LuaJIT
- ⚠️ 守护线程有一线程一Lua状态的开销
- ⚠️ 部分器件只支持单消费者
- ⚠️ Agent模式运行期需要 libcurl.so

完整限制与解决方案见 [docs/faq.md](docs/faq.md)

---

## 📜 关于本项目

### 版本历史

本仓库是 **bot4cj（仓颉语言版）的 C++17 全量移植**：

- ✅ 三层脚本设计与用法不变
- ✅ 底层运行时：仓颉运行时 → 原生C++（`std::thread` + LuaJIT C API）
- ✅ 构建系统：`cjpm` → `cmake`
- ✅ 修复原版问题：作用域收敛、SQL校验加强

> 「用法不变」≠「逐字节兼容」：移植时修复了若干旧版问题（详见 [docs/faq.md](docs/faq.md) 和 [docs/changelog.md](docs/changelog.md)）

### 技术规格

- **实现**: C++17（零第三方C++依赖）
- **构建**: CMake ≥ 3.16
- **编译器**: g++ 12+ / clang 14+
- **平台**: Linux (aarch64 / x86_64)
- **运行期**: LuaJIT + libcurl（仅agent模式）

### CI状态

GitLab CI（`.gitlab-ci.yml`，GitCode 托管）多道门禁：
- ✅ 硬门禁：零告警构建（`-Werror`）、Release 全量守门、ASan/UBSan 单跑
- ⭕ 软门禁（`allow_failure`，转硬中）：TSan、clang + clang-tidy、覆盖率、musl 兼容

---

## 🤝 开发与贡献

欢迎贡献代码、驱动、文档！

### 快速开始

```bash
# 安装pre-push钩子
bash scripts/install-hooks.sh

# 开发时运行测试
bash test/run_all.sh

# 全绿即可提交
```

### 贡献指南

- 新增器件驱动：参考 [docs/development.md#新增驱动](docs/development.md)
- 新增测试用例：`test/cpp/test_*.cpp` 或 `test/test_*.lua`
- 文档改进：`docs/` 目录

详见 [docs/development.md](docs/development.md)

---

## 📄 开源协议

本项目基于 **MIT 协议**开源（见 [LICENSE](LICENSE)）

---

## 🔗 相关资源

- **文档中心**: [docs/README.md](docs/README.md)
- **架构分析**: [docs/architecture_analysis.html](docs/architecture_analysis.html)
- **更新日志**: [docs/changelog.md](docs/changelog.md)
- **问题反馈**: GitHub Issues

---

**立即开始** → `export BOT4CJ_LLM_KEY=sk-xxx BOT4CJ_LLM_ENDPOINT=https://你的接入点/v1/chat/completions && ./build/mybot --robot=robots/relay-test.sql --agent`
