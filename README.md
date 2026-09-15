# bot4cj.cpp（mybot）

bot4cj 的 C++ 移植：LuaJIT 驱动的机器人控制框架。结构脚本（SQL4cj）定义
硬件矩阵，动作脚本（.mt 矩阵）驱动 `变量.参数; 值;` 调用，守护线程采样 → 引用
消费全链路。

## 目录结构

- `src/`      C++ 核心：SQL4cj 词法/语法解析器、矩阵解析器、硬件变量、守护线
  程、LuaJIT 桥接（json.hpp 为值语义 JSON 子集）
- `modules/` Lua 驱动（`<大写名>.lua` 对应同名结构表）与 `lib/` 通用库（含
  内存仿真模型 `virtbus/`、`daemon_probe.lua` 常量发布守护）
- `robots/`  结构脚本 `.sql`（`robots/test/` 为回归用）；`scripts/` 动作脚本
  `.mt`（`scripts/test/` 为回归用）
- `test/`    回归入口 `run_all.sh` + Lua 单测 `test_*.lua`

## 构建

```sh
cmake -S . -B build && cmake --build build          # Release
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBOT4CJ_SANITIZE=ON \
  && cmake --build build-asan                       # Debug + ASan/UBSan
```

## 运行

```sh
cat scripts/xxx.mt | ./build/mybot --robot=robots/xxx.sql
./build/mybot --robot=robots/xxx.sql --tcp <port>   # TCP 动作源
```

`scripts/run.sh` 是构建产物定位启动器（`BOT=xxx` 可覆盖二进制；`scripts/
tcp_e2e.sh` 同理）。

## 测试

```sh
bash test/run_all.sh              # Lua 单测 + virt E2E + 无硬件契约回归
bash test/run_all.sh <过滤词>     # 文件名过滤
BOT=./build-asan/mybot bash test/run_all.sh _virt   # 内存仿真链路走 Sanitizer
```

守门分三段：`test/test_*.lua`（LuaJIT）、mybot virt E2E（I2C/SPI 内存仿真模型，
断言 `:f` 浮点透传与守护持续发布）、无硬件契约回归（守护/别名/转发/引用路径，
含负值契约如 INSERT 拼错表、别名重复、`$` 引用标量必须清晰报错）。

## SQL4cj 关键语义

- **引号字面量**：`'` / `"` 包裹的值整体按字面解析，引号内的 `--`、`/* */`、
  `;`、`,`、`(`、`)` 与空白都是数据而非注释/分隔符（注释只在引号外生效）。
- **注释**：`--` 行注释与 `/* */` 块注释。
- **`$` 引用列**：某列值以 `$` 前缀引用另一硬件变量（如 `Monitor.src =
  '$env0'`），运行期将该变量实时 `read()` 快照（字段对象）合并进本变量元数据。
  被引用变量若返回标量（如 Alias 的 JsonString）会清晰报错——标量引用请走
  `controller` 列。
- **别名**：`Alias`/动作脚本内 `@Alias` 给参数起别名；动作脚本内重复定义同名
  别名报错。