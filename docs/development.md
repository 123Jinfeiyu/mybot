# 开发与贡献

> 本文是贡献者指南。[README](../README.md) 中只保留入口链接。

## 起步

```bash
git clone <本仓库> && cd bot4cj
bash scripts/install-hooks.sh                 # 装 pre-push 守门钩子（一次性）
cmake -S . -B build && cmake --build build
bash test/run_all.sh                          # 离线回归（无硬件）
bash test/run_san.sh                          # 双跑：Release + sanitizer（改动并发/生命周期/内存相关时建议跑）
cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql   # 无硬件自检
```

调试单条用例时可用 ctest 逐用例入口（比整段守门快得多，但**不替代**提交前的
`run_all.sh`，见 [testing.md](testing.md#ctest-逐用例入口可选)）：

```bash
cd build && ctest --output-on-failure -R lua_test_pca9685
cd build && ctest --output-on-failure -R cpp_unit
```

## 目录约定

| 路径 | 放什么 | 注意 |
| ---- | ------ | ---- |
| `src/` | C++17 宿主（解析、注册、派发、守护、agent） | 全量 `-Wall -Wextra -Wpedantic` 零告警 |
| `modules/` | 驱动 `<矩阵名>.lua` 与库 `lib/` | 驱动契约见 [drivers.md](drivers.md) |
| `modules/lib/` | HAL、virtbus、mpath、JSON、滤波、模型协议等共享库 | 纯算法库不依赖 HAL，可被 lua5.4 复用 |
| `robots/`、`scripts/` | 结构脚本与动作脚本；`test/` 子目录为回归夹具 | 夹具必须登记进用例表，否则被孤儿检测判失败 |
| `test/` | Lua 单测、`cpp/` C++ 单测、`run_all.sh` 总编排 | 编译产物缺失时对应分段判失败 |
| `docs/` | 深入文档，入口 [docs/README.md](README.md) | 改动后同步索引与链接 |

## 提交信息规范

主体提交信息质量很高（设计论证、被否决方案留档、守门数据），但历史混有过零信息提交
（`^-^`、`add "speed"`、`新的动作`），破坏 changelog 可追溯性（外部评估 2026-09-20 扣分点 #2、
第 3 轮问题清单 R1）。
据此约定**最小信息底线**（历史不回改，防新增）：

- 格式：`<type>(<scope>): <一句 why>`；scope 可省略（如 `docs: ...`）
- `type` ∈ `feat` / `fix` / `docs` / `test` / `refactor` / `gate`
- subject 至少 10 字符，说清「改了什么、为什么」；正文可选，鼓励记录设计取舍与守门数据
- pre-push 钩子在守门**之前**按本规范校验待推送提交的 subject，不合规即中止
  （绕过路径同守门：`git push --no-verify`；详见下节钩子说明）

## 守门钩子（pre-push）

`scripts/git-hooks/pre-push` 是版本管理的正身，`install-hooks.sh` 以**软链**方式接进 `.git/hooks/`：

- 不用 `core.hooksPath`——那会连带忽略该目录里已有的其它钩子
- 不用拷贝——避免源更新后留下过期副本

装后每次 `git push` 前自动做两件事，任一未通过即中止：

1. **提交信息校验**（快、先跑）：对 `@{push}..HEAD` 的每个非 merge 提交检查 subject
   非空、长度 ≥ 10、带类型前缀（规范见上节）。远端分支无 `@{push}` 时（如推送新分支）跳过校验
2. **离线守门**：`bash test/run_all.sh`

单次绕过：`git push --no-verify`。

放行分支（fail-open，fix-plan 3-6 补记）：仓库根缺 `test/run_all.sh` 时钩子**打印提示后直接放行**（`exit 0`）——这是对「在非仓库根/精简检出里推送」场景的兜底，不是安全设计：钩子的威胁模型是**防手滑**而非防绕过（`--no-verify` 本就随时可绕）。在正常仓库根里该分支不可达；守门的强制力以 CI/复核流程为准。

## 新增驱动

在 `modules/` 加一个 `<矩阵名>.lua`（dofile HAL，实现 build/call/destroy 语义）。契约要点：

- `build`/`call`/`destroy` 必须返回带 `result` 键的表（`"OK"` 或 `"ERR"`）；返回非表或漏写 `return` 会被判契约违反并中断
- 入口第一行必须是 `local ok, ek = arg_table(data)` + `if not ok then return ek end`（`drv_common`）——它同时承担入参类型校验与 `proto=virt` 虚拟总线注入前置，手写 `type(data) ~= "table"` 内联检查会绕过注入钩子（`run_all.sh` 的 lint 段会拦；Camera 是唯一豁免，见其头注）
- `result`/`message`/`mode` 三键由框架保留（见 [dsl.md](dsl.md)）
- 建议随驱动在文件尾部内嵌 `<矩阵名>_VIRT` 仿真模型，接入 virtbus 回归（见 [virtbus.md](virtbus.md)）

## 新增回归

`robots/test/<用例>.sql` + `scripts/test/<用例>.mt`，按守门契约接入 `test/run_all.sh` 的 e2e 列表或无硬件契约列表（含负值契约）。完整守门说明见 [testing.md](testing.md)。

## 新增 C++ 单测

在 `test/cpp/test_*.cpp` 用 `TH_TEST` / `TH_CHECK` / `TH_CHECK_THROWS_MSG` 补用例（`test/cpp/harness.hpp`，无第三方框架），并在 `CMakeLists.txt` 的 `mybot_tests` 源列表里加一行。

## 代码规范

本项目 `.cpp` 全量 `-Wall -Wextra -Wpedantic` 零告警为基线，改动请保持。

- **报错文案分语言**（有意分区，非漂移）：结构脚本装配/解析期的报错（`robot`/`sqlparse`/
  `hardware`/`daemonvar` 等，读者是 `.sql`/`.mt` 脚本作者）用**英文**——这类报错常被
  截断/转码后进日志或 CI 输出，ASCII 最稳；agent 与运行期操作者可见的报错
  （`agentloop`/`agentmode`/`agenttools` 等，读者是与机器人对话的人）用**中文**。
  两区都收口到同一异常体系（`IllegalArgumentException` 为根，运行期硬件失败用
  `std::runtime_error`），不要新增 `std::invalid_argument` 等散装类型。

## 文档约定

- **入口两级**：[README](../README.md) 只留「快速开始 + 入口链接」，机制细节写进 `docs/`；
  新增 `docs/*.md` 必须同时登记到 [docs/README.md](README.md)。
- **单一事实源**：文档里的**条数/枚举措辞**尽量指向脚本或目录本身（如「以 `test/run_all.sh` 为准」
  「以 `modules/` 目录为准」），避免代码演进后文档数字漂移——数字只在确需给读者量级时写死，
  并在同句给出权威来源。
- **代码路径要跟着重构走**：文件被拆分/改名时同步更新引用（历史上 `src/luaend.cpp`
  拆成 `luabridge.cpp` + `luasandbox.cpp`，文档因此滞后过）。
- **命令要能直接跑**：文档里的命令按仓库根为 CWD 书写，且必须是当前真实可用的形式
  （如 `--llm-config none`、`LSAN_OPTIONS` 抑制）。改脚本时回头核对文档。
