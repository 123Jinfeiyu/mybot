# 已知限制与常见问题

> [README](../README.md) 中只保留一句话提示，排障细节都在这里。

## 已知限制

- **仅支持 Linux**：HAL 经 ffi 直调 glibc，Windows/macOS 未支持
- **非 glibc libc 未验证**：HAL.sys 已后端接口化（`BOT4CJ_HAL_BACKEND`，缺省
  glibc，接口契约见 `modules/lib/hal_backend_glibc.lua` 头注），但**仅 glibc 实测**；
  musl 等平台移植前需逐项验证：termios 常量与 60B 结构体布局断言、UART 波特率
  编码分叉探测（glibc 2.42 前后两套，`gnu_get_libc_version` 缺失时按旧 ABI 编码
  处理）、ioctl 编号编译期断言（`test_ioctl_nums.c` 等）在对应 libc 头文件下的取值。
  后端接口只为「换实现」收敛了入口，不免除对目标 libc 的 ABI 实证（[hal.md](hal.md)）
- **仅支持 LuaJIT**：总线驱动依赖 ffi（不依赖 HAL 的模块可用 lua5.4 独立调试）。
  缓解：新架构（如 riscv64）可关注 LuaJIT 社区分支移植；纯算法类驱动
  （Ema/Kalman/fft 等）已保持不依赖 HAL，可用 lua5.4 跨解释器复用
- **守护线程开销**：每个守护一个独立线程 + 独立 Lua 状态，拍间必须让出式 sleep（见 [daemon.md](daemon.md)）。
  缓解：守护数量随器件线性增长，规模化时优先合并同类采样到单个守护
  （如一个守护轮询多只同型号传感器），并为低频守护放宽 tick 间隔
- **一器件一消费者（仅限声明者）**：判据不是"页式寄存器"，而是**一次访问是否跨多个
  总线事务、器件侧是否留有地址/握手相位**。HAL 的每次 I2C 访问都是单次 `I2C_RDWR`
  ioctl（寄存器指针写 + 数据读合并成一个内核事务，`i2c_read_reg` /
  `i2c_read_multi_bytes` 皆然），内核在 adapter 层串行化，所以「主线程直连 + 守护
  同址采样」对绝大多数器件是安全的——`robots/test/` 下 20 个夹具正是刻意这么写的。
  真正需要单消费者的是握手式间接访问器件：如 `AS7262`（poll `HW_STATUS` → 写
  `HW_WRITE`=vaddr → poll → 读 `HW_READ`，地址相位在器件侧，跨 3~4 个事务）。
  这类驱动在模块源里**行首声明** `SINGLE_CONSUMER`，装配期按（总线设备路径, 归一化
  地址）判重，第二个消费者即报错并点名双方（`src/hw_factory.cpp` 的
  `checkSingleConsumer`；负值契约见 `robots/test/single_consumer.sql`）。
  守护走 `proto=virt` 时是内存仿真，不算真器件消费者（`as7262.sql` 即此形态）。
  注：本条此前把 `VL53L0X` 当页式器件列入，属**误分类**——它走
  `i2c_read_multi_bytes` 的原子合并读，未声明该标记。
- **SQL/矩阵脚本保留字符**：
  - SQL 值内**不得出现裸同类引号**——`''` 双写转义不实现（`'it''s'` 会被
    拆成 `'it'` + `'s'`）；值必须用另一种引号包裹
  - `#` 是矩阵脚本的注释保留字符——含 `#` 的单元格值会被截断，且经
    `mtmerge` 保存后无法原样读回；需要 `#` 字面值时用 SQL 值列传入
  - `--` 与 `/*...*/` 是 SQL 注释（等价空白）；矩阵脚本 `#` 至行尾
- 与仓颉原版不同：本版**没有**仓颉运行时并发 GC 缺陷，超长动作脚本不受该问题影响
- **动作脚本内联 `@Alias`/`@Vars` 只在本次执行内有效**（2026-09-20 起）：跨脚本复用别名请写进结构脚本的 `@Alias` 表；此前是进程级生效（会永久遮蔽同名结构变量），属已修复的缺陷而非可依赖特性，详见 [dsl.md](dsl.md)

## 常见问题

### Q: 不接硬件也能开发吗？

A: 可以。`bash test/run_all.sh` 用内存仿真（virtbus）与桩替身跑全部离线回归；`relay-test` 链路不需要任何硬件，先跑通它即可验证环境。详见 [virtbus.md](virtbus.md) 与 [testing.md](testing.md)。

### Q: 守护线程 publishes=0？

A: 动作序列极短（几行瞬时 play）时，守护可能在首次采样前就被停止。真实动作序列为秒级没问题；测试时用 Timer 延时列拉长主线程时间窗。

### Q: I2C 设备报 EBUSY？

A: HAL 已用 `I2C_RDWR` 原子事务，地址由每条 i2c_msg 携带，不需要 `I2C_SLAVE` ioctl，一般不会再有内核驱动占用地址导致的 EBUSY（见 [hal.md](hal.md)）。

### Q: 报错提示看不懂怎么办？

A: 结构脚本报错会指明是哪张表哪一行哪一列；动作脚本报错会说明是哪个硬件变量与参数。按提示修正即可。

### Q: 不知道 SCSCL 舵机现在的站号（二手舵机 / 被别人改过号）怎么办？

A: 结构脚本的 `id` 列描述的是**舵机此刻的真实站号**，不是你想设的值——不知道就没法装配（`build()` 会 PING 它，不应答即报 `No PING response`）。`modules/SCSCL.lua` 把 `id` 限死在 1~253，广播地址 `0xFE` 被明确拒绝（驱动不承担扫总线职责，且放行广播会让一个 typo 同时动到总线所有舵机），所以用独立扫描工具：

```bash
luajit scripts/scscl_scan.lua --scan            # 逐号 1~253 定向 PING（多舵机用这个，不会冲突）
luajit scripts/scscl_scan.lua                   # 广播 PING（只挂一块舵机时最快）
luajit scripts/scscl_scan.lua --scan --to 32    # 限定范围，省时间
luajit scripts/scscl_scan.lua --raw             # 附原始字节 dump，用来分辨「线路不通」与「协议对不上」
```

全程只发 PING(0x01)，不写任何寄存器、不碰 EPROM，可反复跑。输出末尾会给出可直接粘贴的 `INSERT INTO SCSCL ...` 片段。注意两点：

- 打开 `/dev/serial0` 需要 root 或 `dialout` 组
- 广播 PING 会让总线上**每块**舵机同时应答，半双工单总线会帧冲突——只适合只挂一块时"捉"它，多舵机请用 `--scan`

想把舵机换成你指定的号，按三步走（未声明 `allow_burn` 的结构脚本会拒绝 `set_id` 烧号，回归 `scscl_burn_guard`；完整流程见 `modules/SCSCL.lua` 头部注释）：

1. 在 `.sql` 的 SCSCL 表显式声明授权列：`allow_burn String`，且该行值为 `1`——烧号是不可逆广播写，须结构期授权
2. 在 `.mt` 里单开一行写 `set_id=N`（解锁 EPROM → 广播写号，断电重启生效；真机烧号只接目标舵机）
3. 重启后把 `.sql` 的 `id` 改成 N

### Q: agent 模式（`--agent`）有什么限制？

A: 完整清单在 [agent.md](agent.md) 的「已知限制」与「安全说明」，这里只说两件最容易绊到的：

- **`run_command`（任意 shell）缺省不注册**，要 `--agent-shell` 或配置 `allow_shell: true` 显式开启；`capture_image` 也只有当 `--camera-var` 在结构脚本里解析到一个 **Lua 驱动**时才提供（没接摄像头就不提供，不拖累上电）。
- **非流式**：一轮回答要等整轮生成完；单次任务受 `--llm-max-turns`（缺省 8）约束，到顶会报错而不是继续转圈。到顶时本轮历史**原样保留**——那些动作脚本已经真跑过、硬件不会倒回去，所以模型看得见自己做过什么；把任务拆小比调大上限更管用。

接口超时用 `--llm-timeout`（1~600 秒），会话历史按字节预算裁剪（缺省 4 MiB，单张图另有 2 MiB 上限）。
