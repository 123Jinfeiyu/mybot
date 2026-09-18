# 回归测试手册

> 本文是 `test/run_all.sh` 守门脚本的完整说明。[README](../README.md) 中只保留命令入口。
> CI（`.gitlab-ci.yml`，F-22）在每次推送后自动跑 Release 全量守门 + sanitizer 双跑，
> 与本地 pre-push 钩子同一条守门。

## 用法

```bash
bash test/run_all.sh          # 全部离线守门（分段见下，顺序与条数以脚本为准）
bash test/run_all.sh pca9685  # 文件名过滤
bash test/run_all.sh tcp      # 只跑 TCP 动作源
```

全部守门在无 `/dev/i2c` 的开发机上即可运行。

## 守门分段明细

> 分段顺序与用例条数以 `test/run_all.sh` 为准（本节只讲每段职责，不维护条数）。

1. **Lua 离线单测**（`test/test_*.lua`，LuaJIT 运行；断言统一经 `test/lib/tester.lua` 出 `[PASS]`/`[FAIL]` 与 `[DONE]` 完成标记；真机 i2c 用例自动跳过）
2. **virt 端到端**（`robots/test/*_virt.sql` 走内存仿真总线 `virtbus`，断言 `:f` 浮点透传与守护持续发布——条数见 `e2e_cases`，明细见 [virtbus.md](virtbus.md)「守护无硬件端到端」一节）
3. **无硬件契约回归**（守护/别名/转发/引用路径；负值契约如 INSERT 拼错表、别名重复、`$` 引用标量、动作参数覆盖结构元数据必须**清晰报错**而非裸崩；含 `PCA9685` 与 `SCSCL` 的**非守护** `@Action` 写路径正/负四条——虚拟总线已不再只服务守护路径，UART 直写路径经 `BUS="uart"` 模型附着 + 跨状态机 fd 采纳）
4. **mtmerge 合成器**（`@Action` 相位归并 + `@Alias`/`@Vars` 按行叠加同名报错；末尾把产物回喂 `mybot` 证明内联别名仍生效）
5. **TCP 动作源回环**（复用 `scripts/tcp_e2e.sh`，随机端口，缺 python3 自动跳过）
6. **TCP 异常注入**（复用 `scripts/tcp_adversarial.sh` 场景矩阵：半包/断连/空闲挂死/垃圾帧/口令负路径，每场景独立服务端 + 看门狗）
7. **agent 模式端到端**（复用 `scripts/agent_e2e.sh` + 本地假模型服务 `scripts/fake_llm_server.py`，四个场景）：① 默认协议驱动（LlmOpenAI）跑完 `list_hardware → run_action_script → run_command → 最终回答`，断言工具循环真的迭代、动作脚本在**常驻**机器人上被执行、最终回答只走 stdout，并抓取线上报文断言是 OpenAI 形态；② `--llm-driver LlmDeepSeek` 断言协议差异——带 `thinking`、不发 `temperature`、历史 `reasoning_content` 被回传（官方文档里的 400 级有状态要求）；③ `--llm-timeout` 必须在 4s 内生效，专防 `curl_easy_setopt` 退回裸变参调用（那时整型选项被静默忽略）；④ `--llm-config` 配置文件断言 URL/KEY/MODEL 全部来自文件、且环境里不放任何密钥；
⑤ `capture_image` 拍照后断言线上报文确实带 `image_url` 多模态内容（data URL 包的真 JPEG）。缺 python3 或 libcurl 时按跳过计）
8. **mtmerge 相位语义**（复用 `scripts/mtmerge_e2e.sh`：相位 0/±1 逐格比对、负相位丢行告警、前导 `-` 的设备在运行端被跳过）
9. **输入边界加固**（`--tcp-host` 非法地址、未知 `--key` 退出码 2、动作脚本/结构脚本超 8 MiB 上限、`@Daemon` 驱动误配无 `build` 时的回滚路径；`mode` 键不得改写 controller 派发目标；驱动 `call()` 漏写 `return` 不得被当成成功；驱动返回自引用表须清晰报错而非宿主栈溢出；agent 专属选项出现在非 agent 模式、`--agent` 与 `--tcp/--input` 互斥、缺 API key 均须退出码 2）
10. **守护 tick 故障注入**（总线持续 NACK → 驱动 ERR → 守护不发布、`count` 停滞，主流程仍正常 join）
11. **进程级 fd 探针**（长动作 + 守护采样期间轮询 `/proc/<pid>/fd`，稳态 fd 数须有界平稳）
12. **信号/退出路径**（F-08：播放中 SIGTERM → rc=0 + 停派发行 + 守护 `joined, total publishes=`；`--tcp` 等待连接中 SIGTERM → rc=1 + 明确中断报错——覆盖 `main.cpp` 的 sigaction 与 `robot.cpp` 的停机分支）
13. **fixture 孤儿检测**（F3/N-32，无过滤器全量跑时执行）：`robots/test/*.sql` 与 `scripts/test/*.mt` 中未被任何用例表引用的夹具判失败；实机夹具（需 `/dev/i2c-*`、`/dev/spidev*`、`/dev/gpiochip*`、`/dev/pwmchip*`）在脚本的 `orphan_whitelist` 显式声明
14. **系统常量自检**（F-24，无过滤器全量跑时执行）：`test_ioctl_nums` / `test_uart_consts` / `test_v4l2_consts` 三个 C 自检二进制把 HAL.lua 硬编码的 ioctl/termios 编号与系统头做编译期断言比对（`_Static_assert`），漂移在构建期即失败，执行期再跑一遍作双保险
15. **C++ 层单测**（`test/cpp/`，`mybot_tests` 目标；零第三方框架，覆盖 JSON 值语义、JSON 文本解析、`.mt` 矩阵状态机、SQL 词法/语法、元数据收集与 `$` 引用环检测、Lua↔Json 边界与转换深度上限、守护 never-spawn/abandon 分支、mtmerge 归并核心白盒，以及 agent 模式的规范化协议编解码 / 配置文件解析与取值优先级 / 驱动注入与 `ChatTransport` 适配（用假 Lua 驱动，不联网）/ 工具派发与路径沙箱 / 工具循环轮次上限与回滚与 extras 透传）。另有两条纯 Lua 用例：`test/test_json_lua.lua`（Lua 侧 JSON 编解码）与 `test/test_llm_drivers.lua`（两个协议驱动的报文整形规则，不碰网络）

## 守门完整性

编译产物（`mybot` / `mybot_tests`）缺失时，对应分段**判失败**而不是静默跳过——否则「不构建」也能报出「0 失败」，而实际只有 Lua 段在跑。仅三类环境软依赖按跳过处理：

- 真机 i2c 用例（本机无 `/dev/i2c-*`）、真机摄像头用例（本机无 `/dev/video*`，文件标记 `-- requires: video`）
- TCP / agent 端到端缺 `python3`
- agent 端到端缺 `libcurl`（agent 模式的运行期依赖，Lua 驱动经 ffi 加载；驱动 build 会明确报「libcurl 不可用」，本入口据此判跳过而非假绿）

汇总行如实体报 `N 通过, N 失败, N 跳过` 并列出未执行清单。

- 只想跑 Lua 段：`ALLOW_SKIP=1 bash test/run_all.sh`
- 显式指定的 `BOT=` / `BOT_TESTS=` 指向不存在的文件时一律判失败，`ALLOW_SKIP` 也不放行

## sanitizer 与单测二进制

构建开关随仓库分发（`CMakeLists.txt` 的 `MYBOT_SANITIZE`，F-23），避免各人手工拼
`CXX_FLAGS` 造成两套构建不一致：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMYBOT_SANITIZE=address,undefined && cmake --build build-asan
bash test/run_san.sh        # 双跑：Release 全量 + ASan/UBSan 全量，两跑都过才算过
```

`run_san.sh` 为第二跑导出 `ASAN_OPTIONS=suppressions=<仓库根>/asan.supp`：agent 模式
加载 Lua 协议驱动时会经 ffi 拉起 libcurl → libcrypto，后者留下一批进程级初始化残留
（即使只请求明文 `http://` 也会发生；栈全在 `CRYPTO_malloc` 内，`curl_global_cleanup`
与 `OPENSSL_cleanup` 之后依然存在）。它与 `tsan.supp` 同性质——外部库的残留噪声，
且**不加载驱动**的 C++ 单测不受影响，故不会掩盖本项目自身的泄漏。理由详见 `asan.supp`。

线程竞争用同一开关换 TSan：`-DMYBOT_SANITIZE=thread -B build-tsan`。注意 LuaJIT 是
未插桩外部库，FFI 侧的槽访问不在 TSan 视野内——TSan 覆盖的是 C++ 侧并发路径
（守护槽指针、信号标志、生命周期分支）。本机实测（树莓派 5 / 内核 6.18 aarch64）：
TSan 运行时以 `FATAL: ThreadSanitizer: unsupported VMA range (Found 47 - Supported
39, 42 and 48)` 退出——进程 VMA 布局不在 TSan 支持范围，属**环境限制**（`setarch -R`
无效；需 root 调低 `vm.mmap_rnd_bits` 后重试），换内核/宿主再跑。

单独跑 sanitizer 段（例如只看 virt E2E）：

```bash
BOT=./build-asan/mybot BOT_TESTS=./build-asan/mybot_tests bash test/run_all.sh _virt
```

单独跑 C++ 单测（可用用例名过滤）：

```bash
./build/mybot_tests [用例名过滤]
```

## 新增回归

`robots/test/<用例>.sql` + `scripts/test/<用例>.mt`，按守门契约接入 `test/run_all.sh` 的 e2e 列表或无硬件契约列表（含负值契约）。流程详见 [development.md](development.md)。

**漏登记会被抓**：`fixture-orphans` 段（F3/N-32）对「夹具文件存在但没人引用」判失败——新增夹具必须接进用例表；若是实机夹具（离线跑不了），把它加进该段的 `orphan_whitelist` 显式声明。
