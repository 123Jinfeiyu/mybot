# pymybot

跨平台的 Python 机器人控制器：用 Lupa（LuaJIT）加载 Lua 硬件驱动，用 sqlparse 解析受限 SQL 结构文件，用 MT 动作脚本驱动设备。

兼容 bot4cj 的「模块 → 结构 → 动作」三层脚本模型，但不依赖原 C++ 运行器、TCP 服务或 MCP。

## 特性

- **三层脚本**：`modules/*.lua` 驱动、`robots/*.sql` 结构、`scripts/*.mt` 动作，一一对应。
- **一次性 CLI**：每次调用独立装配、执行、清理，stdout 只输出一条 JSON，诊断走 stderr。
- **默认安全**：静态命令不激活驱动；`run` / `sample` / `apply-sql` 默认只允许虚拟总线、串口回环或图片文件，必须显式 `--allow-hardware` 才打开物理设备。
- **跨平台 HAL**：UART 走 pyserial，I2C/SPI 走 pyftdi（MPSSE），Camera 走 OpenCV；Linux 可选 smbus2/spidev。Windows / Linux / macOS 均可运行。
- **无硬件验证**：内置虚拟总线（`virtbus`）、寄存器映射模型、串口 `loop://` 回环，支持 NACK / 超时 / 短读等故障注入。
- **后台采样**：Daemon 表基于 Python 线程和独立 Lua 状态做周期采样，支持单设备、多设备同总线和聚合。
- **按需驱动**：不预置具体芯片驱动，只提供 I2C / SPI / UART / Camera 基础资源和公共工具库，由使用者按数据手册编写。

## 目录结构

```
pymybot/                  Python 包：CLI、SQL 解析、动作执行、HAL、Daemon
modules/                  Lua 基础资源与公共库
  I2C.lua SPI.lua UART.lua Camera.lua     四种基础总线/设备资源
  Timer.lua Relay.lua dummy.lua           纯软件模块（Relay 仅转发参数，非物理继电器）
  Ema.lua Kalman.lua Monitor.lua          滤波与监视
  bitcompat.lua                           LuaJIT 5.1 位运算兼容层
  lib/drv_common.lua lib/HAL.lua          驱动框架与 HAL 桥接
  lib/bus_contract.lua                    总线参数与引用契约
  lib/sensor_daemon*.lua sensor_aggregate.lua  采样 chunk
  lib/virtbus.lua lib/virtbus/regmap.lua  虚拟总线与寄存器模型
robots/                   SQL 结构文件（示例：relay-test.sql）
scripts/                  MT 动作脚本（示例：relay-test.mt）
tests/                    pytest 测试
```

## 安装

要求 Python 3.11+。

```sh
python -m venv .venv
# Linux/macOS
.venv/bin/pip install -e ".[test]"
# Windows
.venv\Scripts\pip install -e ".[test]"
```

Linux 上需要原生 `/dev/i2c-*` / `/dev/spidev*` 时追加 `.[linux]`。安装后可用 `pymybot` 命令或 `python -m pymybot`。

验证依赖：

```sh
.venv/bin/python -c "from lupa.luajit21 import LuaRuntime; import sqlparse, serial, pyftdi, cv2"
.venv/bin/python -m pytest
```

## 快速开始

项目附带一个不接触任何硬件的转发演示：

```sh
python -m pymybot check --robot robots/relay-test.sql --input scripts/relay-test.mt
python -m pymybot run   --robot robots/relay-test.sql --input scripts/relay-test.mt --timeout 10
```

输出：

```json
{"ok":true,"command":"run","outputs":[{"slept":1,"result":"OK"}]}
```

### 结构文件（SQL）

每张普通表对应 `modules/<表名>.lua`，每个 INSERT 行创建一个变量。`$变量` 引用先前声明的总线，`-` 继承上一行同列。

```sql
CREATE TABLE UART (name String, path String, baud Number);
INSERT INTO UART (name, path, baud) VALUES ('bus', 'loop://', 115200);
CREATE TABLE LoopbackProbe (name String, UART String);
INSERT INTO LoopbackProbe (name, UART) VALUES ('echo', '$bus');
```

### 动作脚本（MT）

`@Action` 起始、单独 `@` 结束；表头为 `变量.参数`，每数据行是一帧。

```mt
@Action
echo.value;
         0;
       127;
@
```

支持 `@Alias`、`@Vars`、`@if(...)`、`@while(...)`、`$(表达式)`、`capture` 捕获结果和 Include 片段复用。

### 驱动模块（Lua）

```lua
local mpath = assert(__mpath)
dofile(mpath.resolve("lib/drv_common.lua"))

require_driver("uart", {
    name = "LoopbackProbe",
    build = function(ctx, data) return {result = "OK"} end,
    call = function(ctx, data)
        if uart_write(ctx.fd, {tonumber(data.value)}) ~= 1 then return err("short write") end
        local reply = uart_read(ctx.fd, 1, 200)
        if reply == nil or #reply ~= 1 then return err("read failed") end
        return {result = "OK", rx = reply[1]}
    end,
})
```

`build` / `call` / `destroy` 返回非空表，推荐 `{result="OK", ...}` 或 `err("原因")`；可选 `read(data)` 提供快照。

## CLI 命令

| 命令 | 是否激活驱动 | 说明 |
| --- | --- | --- |
| `check --robot SQL [--input MT]` | 否 | 静态检查 SQL、模块存在性、引用及 MT/Include 语法 |
| `hardware --robot SQL` | 否 | 列出声明的硬件及元数据 |
| `scripts [--scripts-dir DIR]` | 否 | 列出 `.mt` 脚本 |
| `query --robot SQL --sql "..."` | 否 | `SHOW TABLES` 或 `SELECT * FROM 表 [WHERE ...]` |
| `run --robot SQL --input MT` | 是 | 激活、执行动作、清理 |
| `sample --robot SQL --name VAR` | 是 | 激活并读取命名变量，支持 `--count` / `--interval-ms` |
| `apply-sql --robot SQL --sql "..."` | 是 | 对临时激活的会话应用受限 SQL，不改磁盘文件 |

通用选项：`--module-path DIR`（可重复，显式指定时须同时包含 `modules`）、`--camera-src-dir DIR`、`--timeout SECONDS`、`--var NAME=VALUE`、`--allow-hardware`。

退出码：0 成功，1 运行错误，2 用法错误，130 被中断。

### 在项目外目录使用

`--robot` / `--input` 可以指向任意位置的文件，例如把驱动、结构和动作放在 `~/robots/<任务>/{modules,robots,scripts}`。显式 `--module-path` 会替换默认搜索路径，因此必须同时给出自定义目录和基础目录：

```sh
cd ~/robots/<任务>
pymybot run --robot robots/<任务>.sql --input scripts/<任务>.mt \
  --module-path modules --module-path /path/to/pymybot/modules --timeout 10
```

也可以用环境变量 `BOT4CJ_MODULE_PATH`（按系统路径分隔符连接多个目录）代替重复的 `--module-path`。`@Include` 只能引用动作脚本所在目录内的文件，`--camera-src-dir` 只允许读取该目录内的图片。

## 平台说明

- UART：pyserial；Windows 使用确认过的 `COMn`，`loop://` 为无硬件回环。
- I2C/SPI：pyftdi；需要支持 MPSSE 的 FTDI 适配器和 libusb 驱动，地址形如 `ftdi://ftdi:232h/1`，SPI 可带 `?cs=0`。
- `/dev/i2c-*`、`/dev/spidev*`：仅 Linux，依赖可选 smbus2/spidev。
- Camera：OpenCV；文件输入必须指定 `--camera-src-dir`。
- 不提供 GPIO/PWM/Speaker 原生后端；相应外设通过受支持总线编写协议实现。
- Lua 指令预算和可取消等待不能中断阻塞的原生库调用；进程退出不等于执行器物理急停。

## 安全边界

- 只执行已审阅且可信的 Lua。Lupa 使用 LuaJIT 2.1，FFI 可用；路径与属性限制不是恶意代码隔离。
- `active` / `build` / `destroy` 以及 `sample` 都可能写硬件：初始化会配置器件，`read` 可能回退到驱动 `call`。
- 不把真实路径静默替换为虚拟路径，不把合成数据报告为真实测量；虚拟和真机使用分别命名的 SQL。
- 结构文件、动作脚本和 SQL 文本上限 8 MiB，结果体同样受限。

## 构建

```sh
.venv/bin/pip install build
.venv/bin/python -m build
```

`build_support.py` 会把 `modules/` `robots/` `scripts/` 复制到包内 `pymybot/assets/`，安装后无需源目录即可运行。

## 许可

[MIT](LICENSE)
