# 回归测试手册

> 本文是 `test/run_all.sh` 守门脚本的完整说明。[README](../README.md) 中只保留命令入口。
> CI（`.gitlab-ci.yml`，F-22）在每次推送后自动跑 Release 全量守门 + sanitizer 双跑，
> 构建步带 `-DMYBOT_WERROR=ON`（README 声称的「零告警」由此成为门禁而非约定）；
> 与本地 pre-push 钩子同一条守门。另有 `gate:tsan`（只跑守护并发相关 C++ 用例）、
> `gate:clang`（clang 构建 + C++ 单测烟雾）与 `gate:clang-tidy`（`src/` 静态分析）
> 三条 **allow_failure 软门禁**，观察若干轮后再收紧。

## 用法

```bash
bash test/run_all.sh          # 全部离线守门（分段见下，顺序与条数以脚本为准）
bash test/run_all.sh pca9685  # 文件名过滤
bash test/run_all.sh tcp      # 只跑 TCP 动作源
```

全部守门在无 `/dev/i2c` 的开发机上即可运行。

### ctest 逐用例入口（可选）

`run_all.sh` 是权威编排；CMake 同时把每个 `test/test_*.lua` 与 C++ 单测注册成了独立
ctest 用例，便于定位单条失败、享受逐用例超时（Lua 用例 60s），不必整段重跑：

```bash
cd build && ctest --output-on-failure        # 全部注册用例
cd build && ctest -R lua_test_pca9685        # 按名字过滤（Lua 用例前缀 lua_）
cd build && ctest -R 'lua_|cpp_unit'         # 只跑 Lua 段 + C++ 单测
```

注册项来自 `CMakeLists.txt`：每个 `test/test_*.lua` 一条，统一经
`test/lib/device_gate.sh` 包装——读文件内 `-- requires: <dev>` 标记，本机无
`/dev/<dev>*` 时以 125 退出，由 `SKIP_RETURN_CODE 125` 记为跳过（2026-09-20 阶段 5.2
之前 ctest 侧按**文件名**特判 `test_jit_warmup.lua`，同样带 `video` 标记的
`test_camera_v4l2.lua` 被裸注册，无摄像头机器上报 Failed 而非 Skipped；现在两条入口
共用同一个标记解析）。外加 `cpp_unit` / `const_ioctl` / `const_uart` / `const_v4l2` /
`const_curl`（最后一个可选：构建机装了 libcurl 开发头文件才有）。只跑 ctest
**不覆盖** `run_all.sh` 的端到端、TCP、agent 与守门完整性分段——提交前仍以
`bash test/run_all.sh` 为准。

## 守门分段明细

> 分段顺序与用例条数以 `test/run_all.sh` 为准（本节只讲每段职责，不维护条数）。

1. **Lua 离线单测**（`test/test_*.lua`，LuaJIT 运行；断言统一经 `test/lib/tester.lua` 出 `[PASS]`/`[FAIL]` 与 `[DONE]` 完成标记；真机用例经 `test/lib/device_gate.sh` 按 `-- requires:` 标记自动跳过）
   - 其中 `test_driver_contract.lua` 是**驱动入口契约 sweep**（2026-09-20 阶段 4.5）：逐个真的加载全部 `modules/*.lua`，断言 ① 可加载（模块路径引导写错即刻红，不必等 E2E）、② `build`/`call`/`destroy` 三入口齐全、③ 非表入参返回 ERR 契约表而非抛 Lua error。豁免只有两个且都注明理由：`bitcompat`（纯 Lua 库）与 `Camera`（头注声明不挂 `arg_table`）
2. **lint 段**（2026-09-20 阶段 2 收尾防线）：`grep` 断言驱动入口一律走 `arg_table`，且模块路径引导一律走 `__mpath`（见 [reference.md](reference.md) 的 `BOT4CJ_MODULE_PATH`）——都是「约定靠不住、用一行 grep 钉住」的便宜防线
3. **virt 端到端**（`robots/test/*_virt.sql` 走内存仿真总线 `virtbus`，断言 `:f` 浮点透传与守护持续发布——条数见 `e2e_cases`，明细见 [virtbus.md](virtbus.md)「守护无硬件端到端」一节）
4. **无硬件契约回归**（守护/别名/转发/引用路径）：
   - 负值契约：INSERT 拼错表、别名重复、`$` 引用标量、动作参数覆盖结构元数据必须**清晰报错**而非裸崩
   - 含 `PCA9685`/`MCP4725`/`SCSCL` 的**非守护** `@Action` 写路径正/负六条——虚拟总线已不再只服务守护路径，UART 直写路径经 `BUS="uart"` 模型附着 + 跨状态机 fd 采纳
5. **mtmerge 合成器**（`@Action` 相位归并 + `@Alias`/`@Vars` 按行叠加同名报错；末尾把产物回喂 `mybot` 证明内联别名仍生效）
6. **TCP 动作源回环**（复用 `scripts/tcp_e2e.sh`，随机端口，缺 python3 自动跳过）
7. **TCP 异常注入**（复用 `scripts/tcp_adversarial.sh` 场景矩阵：半包/断连/空闲挂死/垃圾帧/口令负路径，每场景独立服务端 + 看门狗）
8. **agent 模式端到端**（复用 `scripts/agent_e2e.sh` + 本地假模型服务 `scripts/fake_llm_server.py`，六个场景；缺 python3 或 libcurl 时按跳过计）：
   - ① 默认协议驱动（LlmOpenAI）跑完 `list_hardware → run_action_script → run_command → 最终回答`：断言工具循环真的迭代、动作脚本在**常驻**机器人上被执行、最终回答只走 stdout，并抓取线上报文断言是 OpenAI 形态
   - ② `--llm-driver LlmDeepSeek` 断言协议差异：带 `thinking`、不发 `temperature`、历史 `reasoning_content` 被回传（官方文档里的 400 级有状态要求）
   - ③ `--llm-timeout` 必须在 4s 内生效：专防 `curl_easy_setopt` 退回裸变参调用（那时整型选项被静默忽略）
   - ④ `--llm-config` 配置文件：断言 URL/KEY/MODEL 全部来自文件、且环境里不放任何密钥
   - ⑤ `capture_image` 拍照：断言线上报文确实带 `image_url` 多模态内容（data URL 包的真 JPEG）
   - ⑥ `--prompt-file` 提示词包：目录 .md 按文件名自然排序、以「## 文件名」分节追加在系统提示词内置段落之后
9. **mtmerge 相位语义**（复用 `scripts/mtmerge_e2e.sh`：相位 0/±1 逐格比对、负相位丢行告警、前导 `-` 的设备在运行端被跳过）
10. **输入边界加固**，覆盖：
   - `--tcp-host` 非法地址、未知 `--key` 退出码 2、动作脚本/结构脚本超 8 MiB 上限
   - `@Daemon` 驱动误配无 `build` 时的回滚路径
   - `mode` 键不得改写 controller 派发目标；驱动 `call()` 漏写 `return` 不得被当成成功；驱动返回自引用表须清晰报错而非宿主栈溢出
   - agent 专属选项出现在非 agent 模式、`--agent` 与 `--tcp/--input` 互斥、缺 API key，均须退出码 2
11. **守护 tick 故障注入**（总线持续 NACK → 驱动 ERR → 守护不发布、`count` 停滞，主流程仍正常 join）
12. **进程级 fd 探针**（长动作 + 守护采样期间轮询 `/proc/<pid>/fd`，稳态 fd 数须有界平稳）
13. **信号/退出路径**（F-08：播放中 SIGTERM → rc=0 + 停派发行 + 守护 `joined, total publishes=`；`--tcp` 等待连接中 SIGTERM → rc=1 + 明确中断报错——覆盖 `main.cpp` 的 sigaction 与 `robot.cpp` 的停机分支）
14. **fixture 孤儿检测**（F3/N-32，无过滤器全量跑时执行）：`robots/test/*.sql` 与 `scripts/test/*.mt` 中未被任何用例表引用的夹具判失败；实机夹具（需 `/dev/i2c-*`、`/dev/spidev*`、`/dev/gpiochip*`、`/dev/pwmchip*`）在脚本的 `orphan_whitelist` 显式声明
15. **系统常量自检**（F-24，无过滤器全量跑时执行）：`test_ioctl_nums` / `test_uart_consts` / `test_v4l2_consts` 三个 C 自检二进制把 HAL.lua 硬编码的 ioctl/termios 编号与系统头做编译期断言比对（`_Static_assert`），漂移在构建期即失败，执行期再跑一遍作双保险；第四个 `test_curl_consts` 把 `llm_http.lua` 硬编码的 CURLOPT/CURLINFO 编号与 curl.h 比对——libcurl 开发头文件**不是**构建依赖（agent 模式运行期才 dlopen libcurl.so），装了头文件才构建本目标，缺失产物按跳过计（与缺 python3 同口径），CI 安装 `libcurl4-openssl-dev` 保证覆盖
16. **C++ 层单测**（`test/cpp/`，`mybot_tests` 目标，零第三方框架），覆盖：
    - 基础设施：JSON 值语义、JSON 文本解析、Lua↔Json 边界与转换深度上限、util 全局助手（安全标识符判据 `isSafeIdent`、深度护栏 `DepthLimitGuard`、CLI 切分 `cli::parseKeyValue`、越界文案模板 `rangeErrorText`）
    - 解析管线：`.mt` 矩阵状态机、SQL 词法/语法、元数据收集与 `$` 引用环检测、mtmerge 归并核心白盒
    - 守护：never-spawn/abandon 分支
    - 会话变量作用域（2026-09-20 阶段 1）：动作脚本内联 `@Alias` 不跨 `run()` 残留、不遮蔽结构变量、报错脚本不改写注册表、200 轮重复执行后结构名字索引不增长
    - I/O 与错误语义（2026-09-20 阶段 4）：`retryEintr` 只在 EINTR 重试、整文件读取超限报「字节上限」而非「不是合法 JSON」、TCP 口令三态（空闲超时/对端关闭不算鉴权失败，只有凭据错才计数）、表达式语法错误截断回显
    - agent 模式：规范化协议编解码、配置文件解析与取值优先级、驱动注入与 `ChatTransport` 适配（用假 Lua 驱动，不联网）、工具派发与路径沙箱、工具循环轮次上限与回滚与 extras 透传

    另有两条纯 Lua 用例：`test/test_json_lua.lua`（Lua 侧 JSON 编解码）与 `test/test_llm_drivers.lua`（两个协议驱动的报文整形规则，不碰网络）

## 守门完整性

编译产物（`mybot` / `mybot_tests`）缺失时，对应分段**判失败**而不是静默跳过——否则「不构建」也能报出「0 失败」，而实际只有 Lua 段在跑。仅三类环境软依赖按跳过处理：

- 真机用例（本机无 `/dev/i2c-*` / `/dev/video*` 等）：统一经 `test/lib/device_gate.sh` 读文件内 `-- requires: <dev>` 标记判定，`run_all.sh` 与 ctest 共用同一实现（可用 `BOT4CJ_TEST_VIDEO=<路径>` 指定摄像头设备）
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
bash test/run_san.sh              # 双跑：Release 全量 + ASan/UBSan 全量，两跑都过才算过
bash test/run_san.sh --asan-only  # 只跑 sanitizer 跑（CI 的 gate:sanitizer 用法：
                                  # Release 全量交给 gate:release，不重复跑一遍）
```

`run_san.sh` 为第二跑导出 `LSAN_OPTIONS=suppressions=<仓库根>/asan.supp`（用 `:` 追加，
保留调用方已有选项）：agent 模式
加载 Lua 协议驱动时会经 ffi 拉起 libcurl → libcrypto，后者留下一批进程级初始化残留
（即使只请求明文 `http://` 也会发生；栈全在 `CRYPTO_malloc` 内，`curl_global_cleanup`
与 `OPENSSL_cleanup` 之后依然存在）。它与 `tsan.supp` 同性质——外部库的残留噪声，
且**不加载驱动**的 C++ 单测不受影响，故不会掩盖本项目自身的泄漏。理由详见 `asan.supp`。
注意：抑制必须走 `LSAN_OPTIONS` 而非 `ASAN_OPTIONS`——ASan 的抑制解析器不认识 `leak:`
类型，放进 `ASAN_OPTIONS` 会让每个用例都因 `failed to parse suppressions` 失败。

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
