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
| `--tcp <port>` | TCP 模式：监听端口接受**一次**连接并读取对端下发的 `.mt`（EOF 或空闲超时按输入结束；单连接一次性批量下发，非流式长驻） |
| `--tcp-host <addr>` | TCP 监听地址，缺省 `127.0.0.1`（跨机需显式给 `0.0.0.0`） |
| `--tcp-token <tok>` | TCP 可选口令：对端须先发一行 `"<tok>\n"` 再发脚本；校验为**恒时比较**（防时序侧信道，审计 M1）；监听非回环地址而未设口令时启动告警 |
| `--tcp-idle <sec>` | TCP 空闲超时，缺省 30s（超时按输入结束处理） |
| `--input <path>` | 从文件读取动作脚本 |
| （缺省） | 从 stdin 管道读取 |

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

## 环境变量

| 变量 | 说明 |
| --- | --- |
| `BOT4CJ_MODULE_PATH` | 驱动模块目录（默认 `./modules`），`<矩阵名>.lua`、`lib/`、`@Daemon` 的 chunk 都按它解析；支持 `:` 分隔多路径按序查找 |
| `BOT4CJ_DEBUG` | 设为 `1` 开启 call/build/destroy 的参数表、结果表、逐键值转储；缺省静默 |

（仓颉原版另有 `BOT4CJ_NO_GC` 长脚本开关，用于规避仓颉运行时并发 GC 缺陷——本 C++ 移植版无仓颉运行时、不存在该缺陷，未实现此变量。）
