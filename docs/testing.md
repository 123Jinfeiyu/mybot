# 回归测试手册

> 本文是 `test/run_all.sh` 守门脚本的完整说明。[README](../README.md) 中只保留命令入口。

## 用法

```bash
bash test/run_all.sh          # 全部：Lua 单测 + virt E2E + 无硬件契约 + TCP E2E + C++ 单测
bash test/run_all.sh pca9685  # 文件名过滤
bash test/run_all.sh tcp      # 只跑 TCP 动作源
```

全部守门在无 `/dev/i2c` 的开发机上即可运行。

## 六段守门明细

1. **Lua 离线单测**（`test/test_*.lua`，LuaJIT 运行；真机 i2c 用例自动跳过）
2. **virt 端到端**（`robots/test/*_virt.sql`，37 例走内存仿真总线 `virtbus`，断言 `:f` 浮点透传与守护持续发布——明细见 [virtbus.md](virtbus.md)「守护无硬件端到端」一节）
3. **无硬件契约回归**（守护/别名/转发/引用路径；负值契约如 INSERT 拼错表、别名重复、`$` 引用标量、动作参数覆盖结构元数据必须**清晰报错**而非裸崩；含 `PCA9685` 的**非守护** `@Action` 写路径正/负两条——虚拟总线已不再只服务守护路径）
4. **TCP 动作源回环**（复用 `scripts/tcp_e2e.sh`，随机端口，缺 python3 自动跳过）
5. **输入边界加固**（`--tcp-host` 非法地址、动作脚本超 8 MiB 上限、`@Daemon` 驱动误配无 `build` 时的回滚路径；`mode` 键不得改写 controller 派发目标；驱动 `call()` 漏写 `return` 不得被当成成功；驱动返回自引用表须清晰报错而非宿主栈溢出）
6. **C++ 层单测**（`test/cpp/`，`mybot_tests` 目标；零第三方框架，覆盖 JSON 值语义、`.mt` 矩阵状态机、SQL 词法/语法、元数据收集与 `$` 引用环检测、Lua↔Json 边界与转换深度上限）

## 守门完整性

编译产物（`mybot` / `mybot_tests`）缺失时，对应分段**判失败**而不是静默跳过——否则「不构建」也能报出「0 失败」，而实际只有 Lua 段在跑。仅两类环境软依赖按跳过处理：

- 真机 i2c 用例（本机无 `/dev/i2c-*`）
- TCP 段缺 `python3`

汇总行如实体报 `N 通过, N 失败, N 跳过` 并列出未执行清单。

- 只想跑 Lua 段：`ALLOW_SKIP=1 bash test/run_all.sh`
- 显式指定的 `BOT=` / `BOT_TESTS=` 指向不存在的文件时一律判失败，`ALLOW_SKIP` 也不放行

## sanitizer 与单测二进制

Sanitizer 二进制同样可跑全部 E2E 与 C++ 单测：

```bash
BOT=./build-asan/mybot BOT_TESTS=./build-asan/mybot_tests bash test/run_all.sh _virt
```

单独跑 C++ 单测（可用用例名过滤）：

```bash
./build/mybot_tests [用例名过滤]
```

## 新增回归

`robots/test/<用例>.sql` + `scripts/test/<用例>.mt`，按守门契约接入 `test/run_all.sh` 的 e2e 列表或无硬件契约列表（含负值契约）。流程详见 [development.md](development.md)。
