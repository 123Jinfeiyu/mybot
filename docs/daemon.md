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
- chunk 先跑一次完成 setup 并定义全局 `daemon_tick`，之后逐拍 `pcall`；**拍间用线程 sleep 让出**，切忌在 chunk 内 ffi `usleep` 自旋（阻塞调度不放）
- `@Daemon` 除 `name`/`chunk` 外的列序列化为 `DAEMON_CONFIG` 串注入 chunk

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

## 注意事项

守护 setup 需数毫秒；动作序列极短时可能守护尚未首发就被停止（publishes=0）。真实动作序列为秒级没问题；测试用 Timer 延时列拉长主线程时间窗。
