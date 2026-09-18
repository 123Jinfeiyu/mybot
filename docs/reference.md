# 命令行与环境变量参考

> 本文收录 bot4cj 运行时的全部可调接口：CLI 参数、动作输入源、安全约束与环境变量。[README](../README.md) 中只保留快速开始。

## 环境要求

| 组件 | 说明 | 状态 |
| --- | --- | --- |
| Linux | HAL 经 ffi 直调 glibc（open/ioctl/read/write/usleep） | ✅ 必需 |
| CMake ≥ 3.16、C++17 编译器 | g++ / clang++ | ✅ 必需 |
| LuaJIT 2.1（`libluajit-5.1-dev`） | 驱动与 HAL 全部在 LuaJIT 上 | ✅ 必需 |
| Python 3 | 仅 `scripts/tcp_e2e.sh` 的 TCP 客户端需要（缺则守门自动跳过） | ⭕ 可选 |
| `/dev/i2c-*` | 真机总线 | ⭕ 可选（无总线时全流程可用内存仿真跑） |

## 命令行参数

程序名为 `mybot`。缺省无参数时打印帮助。参数支持 `--key=value` 与 `--key value` 两种写法（`src/main.cpp:parseArgs`）；未知或拼错的 `--key` 一律报错并以退出码 **2** 结束（不再静默忽略），`--help` 始终优先。

| 参数 | 说明 |
| --- | --- |
| `--robot <path>` | **必选**，机器人结构脚本 `.sql` 路径；打不开或解析失败会给出人话提示并退出 1 |
| `--help` | 打印帮助手册 |
| `--tcp <port>` | TCP 模式：监听端口接受**一次成功连接**并读取对端下发的 `.mt`（EOF 或空闲超时按输入结束；单连接一次性批量下发，非流式长驻）。口令校验失败只关该连接回 accept 等待，**连续 5 次**失败才退出（防单次连接 DoS，也防无限爆破） |
| `--tcp-host <addr>` | TCP 监听地址，缺省 `127.0.0.1`；跨机监听（如 `0.0.0.0`）**必须**同时提供 `--tcp-token`，否则拒绝启动 |
| `--tcp-token <tok>` | TCP 口令：跨机监听必填；对端须先发一行 `"<tok>\n"` 再发脚本；校验为**恒时比较**（防时序侧信道，审计 M1）。也可用环境变量 `BOT4CJ_TCP_TOKEN` 传递（argv 出现在 `/proc/*/cmdline`，多用户机器上建议用环境变量） |
| `--tcp-idle <sec>` | TCP 空闲超时，缺省 30s（超时按输入结束处理）。另有**连接总时长预算**（600s，随该值×20 放大）从连接建立起算：慢滴发送（每 <idle 秒发 1 字节）不能无限期挂住进程 |
| `--input <path>` | 从文件读取动作脚本 |
| （缺省） | 从 stdin 管道读取 |
| `--agent` | **agent 模式**：机器人结构脚本加载后硬件常驻内存，由大模型工具循环决定执行哪些动作脚本（与上面的动作输入源互斥，详细见 [agent.md](agent.md)） |
| `--task <文本>` | agent 模式一次性任务，跑完即退出；缺省则进交互 REPL |
| `--scripts-dir <目录>` | agent 动作脚本根目录，也是 `run_action_script` 的路径沙箱边界（缺省 `scripts`） |
| `--llm-endpoint <URL>` | agent 模型接口地址（缺省 `https://moma.cmecloud.cn/v1/chat/completions`） |
| `--llm-model <名称>` | agent 模型名（缺省 `zhipu/glm-5.3-flash`） |
| `--llm-key <KEY>` | agent API key（不推荐：argv 对本机所有用户可读；优先用环境变量） |
| `--llm-max-turns <N>` | 单次任务最大模型轮数，范围 1-100（缺省 8） |
| `--llm-max-tokens <N>` | 单次生成最大 token 数（缺省 4096） |
| `--llm-timeout <秒>` | 单次模型调用超时（缺省 180） |
| `--agent-no-shell` | 不向智能体提供 `run_command`（去掉任意 shell 执行能力） |

agent 专属选项出现在非 agent 模式下、或 `--agent` 与 `--tcp`/`--input` 同时给出，一律报错并以退出码 **2** 结束。

Ctrl-C（SIGINT/SIGTERM）只置停止标志，走正常清理链 `deactivate()`。

## 动作输入源

`src/actionsource.cpp` 工厂按 **TCP > 文件 > stdin** 的优先级三选一：

```bash
# 1) TCP 模式：监听端口接受一次连接并读取 .mt（EOF/空闲超时结束）
./build/mybot --robot=robots/relay-test.sql --tcp 19240

# 2) 文件输入：不依赖管道
./build/mybot --robot=robots/relay-test.sql --input scripts/relay-test.mt

# 3) 缺省：stdin 管道
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql
```

## 安全约束

- 动作脚本只能**引入新的参数键**，不得覆盖结构脚本的元数据列（`fd` / `address` / `max` / `min` / `offset` 等）
- 单次输入上限 **8 MiB**：动作脚本（`.mt`）与结构脚本（`.sql`）统一适用，超限即拒绝并指明输入类别（`action script` / `structure script exceeds the byte limit`）
- `result` / `message` / `mode` 三个键名由框架保留，`.mt` 不得用作参数列（`MtVars_Lua::rejectReservedKeys`）
- 驱动沙箱会禁用 shell 与任意文件读写，但这是**防误用护栏而非安全边界**——接入 `--tcp` 外部数据源时需另行建立信任边界（详见 [hal.md](hal.md)）

### 沙箱威胁模型（N-9，显式声明）

`modules/` 下的驱动与 `@Daemon` chunk 按**可信输入**对待，沙箱的边界是
「驱动作者边界」而非「网络攻击边界」：

- 驱动可经 `require("ffi")` 调 `ffi.C`/`ffi.load` 触达任意 native 符号，
  `os.execute` 中和与文件 I/O 白名单在 ffi 面前形同虚设——这是设计取舍
  （驱动本来就持有真实总线的读写能力，I2C/SPI 事务本身即不可逆向沙箱化）
- 结构脚本与动作脚本落盘前只是数据；**驱动文件（`modules/**/*.lua`）是
  任意原生代码执行的入口**，其信任级别等同运行 mybot 的用户
- `--tcp` 模式下外部对端只能提交**动作脚本**（受 C-1/metadata/rejectReservedKeys
  多层防线约束），不能注入驱动；但动作脚本可触达驱动已暴露的任意 call 路径——
  驱动作者须自行保证参数校验（如 SCSCL 的烧号须结构期 `allow_burn` 授权）
- 若需对抗恶意输入源，应在进程外建立隔离（专用用户、容器、网络命名空间），
  库内 Lua 沙箱对此无能为力

## 环境变量

| 变量 | 说明 |
| --- | --- |
| `BOT4CJ_MODULE_PATH` | 驱动模块目录（默认 `./modules`），`<矩阵名>.lua`、`lib/`、`@Daemon` 的 chunk 都按它解析；支持 `:` 分隔多路径按序查找 |
| `BOT4CJ_DEBUG` | 设为 `1` 开启 call/build/destroy 的参数表、结果表、逐键值转储；缺省静默 |
| `BOT4CJ_LLM_KEY` | agent 模式的 API key。回退顺序：`ROBOT_AGENT_API_KEY` → `DEEPSEEK_API_KEY`。该变量名含 `key`，会被 `run_command` 的环境清洗剔除，不会泄漏给子进程 |

（仓颉原版另有 `BOT4CJ_NO_GC` 长脚本开关，用于规避仓颉运行时并发 GC 缺陷——本 C++ 移植版无仓颉运行时、不存在该缺陷，未实现此变量。）
