# bot4cj

bot4cj是基于仓颉和 Linux I2C 接口的**机器人硬件控制器**，它是一个**可扩展的结构**，目标是将**机器人结构**、**机器人动作**用DSL解耦出来。通过管道向程序传递机器人结构、动作脚本的内容，就可以驱动机器人做出动作。




## 依赖库
这个项目依赖一些仓颉语言的软件包，以及 LuaJIT（仓颉绑定为独立包 [luajit4cj](https://gitcode.com/qingss0/luajit4cj)）


**你需要安装 LuaJIT 开发库**

```bash
sudo apt update && sudo apt install libluajit-5.1-dev
```


## 项目介绍
机器人软件通常被称为“专用软件”，想要得到一个机器人软件通常比一个普通的软件更为困难，这是因为这个世界上的机器人五花八门，所以它们的软件往往都是独特的，所以我想写一种用于描述机器人硬件和动作的通用的脚本。

`bot4cj` 基于仓颉编程语言和 Linux I2C 接口，提供了便捷的硬件控制能力，目前支持pca9685，我通常使用树莓派作为宿主机。






## 基层硬件抽象层（HAL）

I2C/SPI 总线操作由 `modules/lib/HAL.lua` 实现（issue #14：不再依赖仓颉注册的 cfunc），
**仅支持 LuaJIT**（原 i2c-tools/spi-tools shell-out 降级后端已移除）：

- `ffi` 直调 glibc（open/ioctl/read/write/usleep），原生性能零 fork；
  fd 为真实内核句柄，跨 Lua 状态机天然有效；SPI 经 spidev ioctl（`SPI_IOC_MESSAGE` 单次传输，CS 保持低）
- 驱动模块顶部 `dofile` 加载 HAL，函数名与原 cfunc 完全一致；Lua 5.3+ 位运算经 `bitcompat.lua`
  双兼容（LuaJIT 用内建 bit 库，纯 5.4 用算术回退）
- `modules/lib/` 收纳非驱动的库模块：`HAL.lua`（总线抽象）与 `kalman.lua`
  （一维卡尔曼滤波器，自 src/math/kalman.cj 移植，返回表风格，供驱动 dofile 后 `Kalman.new(Q,R)` / `kf:filter(dat)` 使用）
- 不依赖 HAL 的模块（Servo/dummy/Relay/kalman 等）仍可用 `luajit`/`lua5.4` 独立调试；
  总线驱动需 `luajit`（单测见 `test/test_hal_ffi.lua`、`test/test_kalman.lua`）

SPI 传输延时（delay_usecs）无 per-fd 持久化 ioctl，由传输函数的可选 `delay_us` 尾参逐次携带
（配置写在 @SPI 矩阵的 delay 列，SPI.lua 的 `call()` 随 fd 返回，经元数据引用流入设备驱动，参考 ADS1256.lua）。


## 守护线程（@Daemon）

后台持续采样（如 IMU）用 `@Daemon` 矩阵声明，宿主用 **Cangjie `spawn` 原语**起守护线程：

```sql
CREATE TABLE Daemon (
	name  String,
	chunk String
);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'modules/lib/daemon_probe.lua');
```

**并发模型（关键）**：
- 守护线程由 `spawn { ... }` 创建（返回 `Future`，`future.get()` 即 join）——**不能用 pthread4cj 裸线程**：foreign pthread 无 Cangjie 运行时线程态，在其上跑 @C 代码必崩（PoC 实测 3/3 SIGSEGV）；spawn 线程运行时已附着，安全
- 守护线程持有**独立 lua_State**（`jit.off` + loadfilex 采样 chunk 一次 + pcall 一次，chunk 内自带 `while not should_stop` 循环），与主线程状态机 1:1 隔离——规避 LuaJIT VM 非线程安全
- **IPC 经共享内存槽** `daemon_slot_t`（`modules/lib/pthread.lua` 定义布局：mutex+count+nvals+values[16]+stop）：守护侧 Lua `P.publish(slot, {...})` 写值，主线程侧 `play()` 加锁读值，双方用同一 pthread_mutex 防撕裂

**采样 chunk 契约**（运行在守护线程的独立状态机）：
```lua
if jit then jit.off() end
local P = dofile(.../lib/pthread.lua)
dofile(.../lib/HAL.lua)              -- 采样要用 HAL；节拍用 milli_sleep
local slot = P.slot_attach(DAEMON_SLOT_ADDR)   -- 宿主注入的槽地址全局
while not P.should_stop(slot) do
    local ax, ay, az = 采样()         -- 经 HAL 读传感器
    P.publish(slot, { ax, ay, az })
    milli_sleep(2)
end
```

现成示例：`modules/lib/mpu6050_daemon.lua`（复用 MPU6050.lua 驱动读六轴 → 16 位补码符号扩展 → publish 到槽）。@Daemon 矩阵除 name/chunk 外的列（如 i2c/addr/interval）会被序列化为 `DAEMON_CONFIG` 串注入 chunk，由 chunk 解析。

**启动时序注意**：守护线程 setup（开总线 + 配置传感器）需数毫秒；若动作序列极短（几行瞬时 play），可能在守护首次采样前就 free() 停止（表现为 publishes=0）。真实机器人动作序列为秒级，无此问题；测试时用 Timer 延时列拉长主线程时间窗。


## 解析器

树莓派通过I2C接口连接pca9685后使用如下方法使用：

```bash
cat scripts/std.mt | cjpm run	# 蜘蛛机器人站立脚本
```






## 动作脚本

项目中包含的动作脚本在scripts文件夹下面，这些动作脚本的适配机器人结构如下

```textplain
# PCA9685
1号舵机(s1)		=>	左下水平关节
2号舵机(s2)		=>	左下垂直关节
3号舵机(s3)	 	=>	右下水平关节
4号舵机(s4)		=>	右下垂直关节
5号舵机(s5)		=>	右上水平关节
6号舵机(s6)		=>	右上垂直关节
7号舵机(s7)		=>	左上水平关节
8号舵机(s8)		=>	左上垂直关节
9号舵机(s9)		=>	云台1
10号舵机(s10) 	=>	云台2
扬声器(spk)		=>  仅负责延时
```

适用的机器人建模文件[仓库链接](https://github.com/gopsc/Dashun)

### 机器人脚本语法（.mt）

**.bot文件是旧版动作脚本**

新版机器人脚本是行列式表格，每列之间以一个或多个空格、制表符分隔，每行以分号（;）结尾。

每个矩阵对象以@name 行开头，@ 行结束。


robot机器人结构示例：

```mt

# 在 @Action 矩阵中，硬件变量的参数可以使用别名
@Alias
name      alias;
   A      angle;
   F  frequency;
@


# 定义I2C硬件
@I2C
 name        path;
i2c-0  /dev/i2c-0;
i2c-1  /dev/i2c-1;
@


# 定义pca9685硬件
@PCA9685
     name    I2C   path;
pca9685-0  i2c-1   0x40;
@


# 定义mpu6050硬件
@MPU6050
     name    I2C   path;
#mpu6050-0  i2c-1   0x69;
#mpu6050-1  i2c-1   0x68;
#mpu6050-2  i2c-0   0x68;
#mpu6050-3  i2c-0   0x69;
@

# 定义扬声器硬件
@Speaker
name  channels  sample_rate  volumn  max_f  min_f;
 spk         2        48000     0.6   5000      -;
@

# 定义舵机硬件
@Servo
 name   max  min  offset  controller  channel;
  s1    90     -       -   pca9685-0        0;	# rb0
  s2   180     -       -           -        1;	# rb1 
  s3   -90     -       -           -        2;	# lb0
  s4  -180     -       -           -        3;	# lb1
  s5    90     -       -           -        4;	# lf0
  s6   180     -       -           -        5;	# lf1
  s7   -90     -       -           -        6;	# rf0
  s8  -180     -       -           -        7;	# rf1
  s9   180     -       -           -        8;	# cam0
 s10   180     -       -           -        9;	# cam1
@
```

### controller 引用（通用）

除 `Servo` 外，任何硬件矩阵都可以声明一个 `controller` 列，把该硬件变量的调用转发给另一个硬件变量：

- 先运行自己的 Lua 驱动（`modules/<矩阵名>.lua` 的 `call`）
- 框架再把「该行元数据（跳过 `name`/`controller` 列） ∪ 自己 Lua 的返回表」发送给 `controller` 指定的硬件变量
- `controller` 的值支持 `-` 占位符继承上一行
- 被引用的 controller 必须在结构脚本中先于引用它的矩阵定义

可运行示例（`Relay` 把参数转成 `delay` 后交给 `Timer` 延时）：

```sql
CREATE TABLE Timer (
	name String
);
INSERT INTO Timer (name) VALUES ('timer-0');

CREATE TABLE Relay (
	name       String,
	controller String,
	note       String
);
INSERT INTO Relay (name, controller, note) VALUES ('relay0', 'timer-0', 'hello-relay');
```

```bash
cat scripts/relay-test.mt | ./bot --robot=robots/relay-test.sql
```

action机器人动作脚本实例：

```
# 行动矩阵为硬件变量输入参数
@Action
s1.A  s2.A  s3.A  s4.A  s5.A  s6.A  s7.A  s8.A  s9.A  s10.A  spk.F  spk.delay;	# column name
  45     -    45     -    45     -    45     -    90     90     10        200;	# stand
  90     -     -     -     -     -     -     -     -      -      -         20;	# left-behind

  45     -     -     -     -     -     -     -     -      -      0        500;	# stand
   -     -    90     -     -     -    45     -     -      -      -         20;	# right-behind

   -     -    45     -     -     -     -     -     -      -    200        500;	# stand
   -     -     -     -    90     -     -     -     -      -      0         20;	# right-front

   -     -     -     -    45     -     -     -     -      -      -        500;	# stand
   -     -     -     -     -     -    90     -     -      -      -         20;	# left-front

   -     -     -     -     -     -    45     -     -      -      -        500;	# stand
   -     -     -     -     -     -     -     -     -      -      -         20;	# stand
@

```

动作脚本中也可以内联定义 `@Alias`（别名矩阵），无需写进结构脚本：

```mt
# 与 @Action 写在同一个 .mt 文件中
@Alias
name  alias;
   A  angle;
   F  frequency;
@

@Action
s1.A  spk.F;
   45    10;
@
```

- 结构脚本（.sql）与动作脚本（.mt）两侧都可以定义别名
- 同名时**动作脚本的定义覆盖结构脚本**（会打印覆盖日志）
- 同一动作脚本内重复定义同名别名会报错
