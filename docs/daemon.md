# @Daemon 守护线程（后台采样）

> 本文介绍 `@Daemon` 守护线程的并发模型、万能采样 chunk 与滤波器链路。[README](../README.md) 只留入口链接，机制细节以本文为准。

## 概述

有些传感器要**一刻不停地读**（如 IMU）。`@Daemon` 用 C++ `std::thread` 起一个后台线程循环采样，主线程随取随读：

```sql
CREATE TABLE Daemon (name String, chunk String);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'lib/daemon_probe.lua');
```

## 并发模型关键点

- 每个守护 = 独立线程 + **独立 Lua 状态**，规避 LuaJIT VM 非线程安全
- 共享内存槽 + pthread 互斥锁：守护侧 `P.publish(slot,{...})` 写，主线程侧 `play()` 加锁读
- 共享槽 600B：`mutex[64]` / `count` / `nvals` / `values[64]`（TD-3，2026-09-22 扩容） / `stop@584` / `status@588` / `quiesce@592`（BQ-1 停机灭火位，2026-09-29）（C++ 侧 `src/daemonvar.hpp` 的 `static_assert` 与 Lua 侧 `pthread.lua` 的 `assert_layout` 双侧互锁，布局漂移在守护启动时即暴露）
- chunk 先跑一次完成 setup 并定义全局 `daemon_tick`，之后逐拍 `pcall`；**拍间用线程 sleep 让出**，切忌在 chunk 内 ffi `usleep` 自旋（阻塞调度不放）
- `@Daemon` 除 `name`/`chunk` 外的列序列化为 `DAEMON_CONFIG` 串注入 chunk
- **停机顺序（断电屏障）**：`deactivate()` 先对全体守护置 `stop`，再**统一等到退场**
  （全体共享一个 deadline = 各守护 `joinTimeout` 的最大值，避免最坏退出延迟随守护数线性增长），
  之后才逆序 `destroy()` 硬件。防的是「执行器已断电、某个在途 tick 又写它一次」——
  最后一次写随机胜出，进程退出时器件可能重新带电。
  `stop` 只在两拍之间被看见，所以屏障最坏要等一整拍。
  超时弃管的守护仍可能碰总线（它卡在某一拍里出不来），弃管告警会如实说明：
  `not exited within <N>ms, abandoning slot (… may still touch the bus after hardware de-init)`，
  退出前另有统一计数告警（`abandonedDaemonCount`）。

## 万能采样 chunk（零 per-sensor 代码）

`lib/sensor_daemon.lua` 服务任意 build/call/destroy 型驱动：

```sql
CREATE TABLE Daemon (
	name String, chunk String, driver String, proto String,
	path String, addr String, interval String, fields String, sign String
);
INSERT INTO Daemon (name, chunk, driver, proto, path, addr, interval, fields, sign)
  VALUES ('imu0', 'lib/sensor_daemon.lua', 'MPU6050', 'i2c',
          '/dev/i2c-1', '0x68', '10', 'ACCEL_X|ACCEL_Y|ACCEL_Z|GYRO_X|GYRO_Y|GYRO_Z|TEMP:f', '16');
```

`fields` 用 `|` 分隔以避开 SQL 逗号；`sign=16` 表示 16 位补码符号扩展；逐字段后缀可覆盖（`:f` 原样透传浮点，`:s16` 强制补码）。

## 滤波器硬件变量（虚拟传感器）

每个滤波器 = `lib/<filter>.lua` 库（`.new()` + `:filter(x)`）+ `<Filter>.lua` 驱动：

- `Kalman`：一维卡尔曼，配 `Q`/`R`
- `Ema`：指数移动平均，配 `alpha`∈(0,1]

三级引用链 `imu0 → kf0 → mon0` 靠既有引用机制自动串联（播放 mon0 会递归播放 kf0、imu0）。

## 守护无硬件端到端

`@Daemon` 的 `proto` 列填 `virt` 即可做**守护无硬件端到端**：真实驱动 build/call、共享槽 publish、`@Ema/@Kalman` 滤波、`@Monitor` 消费全链路走内存仿真（启动打印 `VIRTUAL HARDWARE` 横幅，仿真数据禁止用于生产）。

## 进阶示例：守护采样 → 滤波 → 消费闭环

```sql
CREATE TABLE Daemon (name String, chunk String);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'lib/sensor_daemon.lua');   -- 采样

CREATE TABLE Kalman (name String, Q String, R String, src String, field String);
INSERT INTO Kalman (name, Q, R, src, field) VALUES ('kf0', '20', '200', '$imu0', 'v0');

CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$kf0');   -- 消费滤波值
```

`modules/Monitor.lua` 即此类示例：`call()` 里 `data.v0` 拿到实时采样值，决策后可经 controller 转发执行器。

## 配置 schema 加载期校验

`@Daemon` 的框架列在**结构脚本加载期**（`src/daemonvar.cpp` 的工厂，与 `interval` 校验同处）即校验，非法值当场报错并点名守护名/列/表/行号——不等守护线程 setup 才失败，也不静默降级：

- `proto`：只接受 `i2c`（缺省）/ `spi` / `uart` / `virt`。拼错（如 `i2`）此前会静默落进 i2c 分支去开总线；
- `fields`：`<名>[:<模式>]`，模式 ∈ 缺省（跟随 `sign`）/ `f` / `float` / `s16`；空项（`A||B`、首尾分隔符）与未知模式（`TEMP:xyz`）此前分别被静默丢弃、静默当浮点透传；
- `sign`：必须是整数（如 `16`）；非数值此前被 `tonumber` 折成 nil，符号扩展静默失效。
- **任意列的字符集**：值里不得含 `NUL` 或 `;`（审计 P4）。`DAEMON_CONFIG` 是
  `k=v;k=v` 串、经 `lua_pushstring` 注入（C 字符串语义），Lua 侧按
  `gmatch("([^;=]+)=([^;]*)")` 解析——NUL 会把它**其后所有列**（含 `proto`/`sign`）
  在 Lua 侧静默抹掉，而上面几条 schema 校验跑在逐列原始串上全部通过，
  即「校验看到的」与「守护拿到的」不是同一份配置；`;` 则截断该值，且若其后片段
  形如 `key=` 还会**注入/覆盖**守护理解的键。放过之后运行期只剩
  `no daemon value` 这类间接症状，故一律加载期报错。
  `=` 是**允许**的（键的模式不含 `=`、值的模式可含 `=`，`extra=a=b` 能原样往返）。
  词法器契约夹具 `robots/test/sqlquote.sql` 因此把带 `;` 的字面值放在普通硬件表里，
  而不是 `@Daemon` 列——词法路径与表名无关，覆盖不减。

矩阵空占位符 `-` 一律视作「未提供」。其余任意键是驱动的 build 参数（`channel`/`pga`/`rshunt`/`osr`…），框架不拦截。

## setup 失败的响亮契约

守护 setup 失败（驱动缺失/加载失败、开总线失败等）是**永久性**失败，**绝不伪装成采样值**：

- 失败码（-9 缺 driver、-8 驱动加载失败、-1 开总线失败、-3 spi setup 失败、-2 build 失败）写进槽的 `status` 字段（`P.set_status`），不是发进 v0 的一个负样本——否则下游 `$` 引用无从甄别，配置错误会被当温度做闭环，只有 `count` 停滞这种间接迹象；
- 宿主侧读路径见 `status ≠ 0` 即抛错（`daemon setup failed (slot status -8); no samples are published`），动作链响亮中止；失败码与原因以守护日志为准；
- 与「尚未出首帧」区分：那是**瞬时**状态，由消费方（`@Monitor`/`@Ema`/`@Kalman`）按 ERR 契约自行表达，不在读路径抛。

## 注意事项

守护 setup 需数毫秒；动作序列极短时可能守护尚未首发就被停止（publishes=0）。真实动作序列为秒级没问题；测试用 Timer 延时列拉长主线程时间窗。
