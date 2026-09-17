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

## 守门钩子（pre-push）

`scripts/git-hooks/pre-push` 是版本管理的正身，`install-hooks.sh` 以**软链**方式接进 `.git/hooks/`：

- 不用 `core.hooksPath`——那会连带忽略该目录里已有的其它钩子
- 不用拷贝——避免源更新后留下过期副本

装后每次 `git push` 前自动跑 `test/run_all.sh`，未通过即中止。单次绕过：`git push --no-verify`。

## 新增驱动

在 `modules/` 加一个 `<矩阵名>.lua`（dofile HAL，实现 build/call/destroy 语义）。契约要点：

- `build`/`call`/`destroy` 必须返回带 `result` 键的表（`"OK"` 或 `"ERR"`）；返回非表或漏写 `return` 会被判契约违反并中断
- `result`/`message`/`mode` 三键由框架保留（见 [dsl.md](dsl.md)）
- 建议随驱动在文件尾部内嵌 `<矩阵名>_VIRT` 仿真模型，接入 virtbus 回归（见 [virtbus.md](virtbus.md)）

## 新增回归

`robots/test/<用例>.sql` + `scripts/test/<用例>.mt`，按守门契约接入 `test/run_all.sh` 的 e2e 列表或无硬件契约列表（含负值契约）。完整守门说明见 [testing.md](testing.md)。

## 新增 C++ 单测

在 `test/cpp/test_*.cpp` 用 `TH_TEST` / `TH_CHECK` / `TH_CHECK_THROWS_MSG` 补用例（`test/cpp/harness.hpp`，无第三方框架），并在 `CMakeLists.txt` 的 `mybot_tests` 源列表里加一行。

## 代码规范

本项目 `.cpp` 全量 `-Wall -Wextra -Wpedantic` 零告警为基线，改动请保持。
