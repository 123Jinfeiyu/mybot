# bot4cj - 基于仓颉的通用机器人控制程序

> **共建理念**：能写文档的写文档，能写代码的写代码，一起共建仓颉生态！

bot4cj 是用仓颉语言编写的**机器人硬件控制器**。可以把它想成一个"播放器"：给机器人一份动作剧本，它就照着"演"出来。

它的核心思路是把**结构、驱动、动作**三件事分开写，各用一种脚本：

| 脚本 | 像什么 | 负责什么 |
| ---- | ------ | -------- |
| 结构脚本 `.sql` | 舞台的**接线图** | 机器人身上有哪些零件、接在哪条总线/哪个地址、谁听谁的 |
| 驱动脚本 `.lua` | 零件的**说明书** | 每个零件怎么点亮、怎么读数（I2C / SPI / UART 都藏在这一层，用快而简单的 LuaJIT 编写） |
| 动作脚本 `.mt` | 演员的**剧本** | 一行就是一个时间点的动作，逐行播放就是一段表演 |

程序把三份脚本拼起来，就能从电脑上用一行命令指挥机器人站立、走路、转向。

[实机演示地址](http://152.136.227.85:8080/videos/robot-show.mp4)

视频由 PC 端与机器人实拍两部分组成，如果播放卡顿可以右键下载后观看。

## 📋 项目信息

- **项目版本**：v0.3.3（`cjpm.toml`）
- **维护状态**：🟢 积极维护中
- **主要负责人**：qingss0（Flowers_Unamed 团队）
- **开发人员**：[qingss0](https://gitcode.com/qingss0)
- **仓颉版本**：支持仓颉语言 1.1.3（stdx 1.1.3）

## 🧭 三份脚本如何配合

```text
结构脚本 .sql → 定义零件清单（舵机、传感器…）
                  │ 每个零件配一份说明书（自动加载）
驱动脚本 .lua → 教计算机读写这个零件
                  ↑ 按剧本逐行调用
动作脚本 .mt  → 第 1 行做什么 → 第 2 行做什么 → …
```

**不接任何硬件也能跑通全流程**：程序内置了"假零件"（传感器桩替身、Relay 转发链路），装好环境就能在开发机上看到整条链路真实运转——下面「快速开始」的第一条命令干的就是这件事。

## 🚀 快速开始

### 安装依赖

首先安装 LuaJIT 开发库（仓颉侧的绑定为独立包 [luajit4cj](https://gitcode.com/qingss0/luajit4cj)）：

```bash
sudo apt update && sudo apt install libluajit-5.1-dev
```

### 构建

```bash
export CANGJIE_STDX_PATH=/path/to/cangjie-stdx   # cjpm.toml 按目标架构读取
bash _set.sh                                     # 等价于 cjpm update + cjpm build
```

### 第一条命令：无硬件跑通 Relay → Timer

`relay-test` 是一台"玩具电梯"，**不需要接任何硬件**：`Relay` 把 `test` 参数转成 `delay`，经 controller 机制转发给 `Timer` 延时 200ms，用来确认环境装好了、程序能跑了。**新手只要跑通这一条就算入门**：

```bash
cat scripts/relay-test.mt | bash _run.sh --robot=robots/relay-test.sql
```

看到 `Relay: test=200 note=hello-relay -> delay=200` 且正常退出，环境就没问题了。想看每一步的参数与结果转储，可以打开调试开关再跑一遍：

```bash
export BOT4CJ_DEBUG=1
cat scripts/relay-test.mt | bash _run.sh --robot=robots/relay-test.sql
```

### 进阶：无硬件测试（可选）

跑通上面那条后，可以在开发机（不接硬件）上把测试跑得更全，分两层：

**Lua 驱动层（桩替身）**：`bash test/run_all.sh` 一键跑全部 Lua 离线测试（需系统装有 `luajit`，可用环境变量 `LUACMD` 指定别的解释器；真机 i2c 用例自动跳过；`test/` 下所有 `test_*.lua` 都会被自动收集）。也可以单跑某个无硬件样例：

```bash
luajit test/test_relay_nohw.lua      # Relay：纯软件驱动，连 HAL/ffi 都不碰
# 该用例不依赖 HAL，用 lua5.4 也能跑
```

**仓颉程序层**：`relay-test` 之上，`@Daemon` 守护线程也备好了无硬件测试命令，见下文「@Daemon 守护线程」小节末尾的「无硬件跑 Daemon」——每条命令退出码 0 即通过。

### 接第一块 PCA9685

- 接线：树莓派 `i2c-1`（SDA1/SCL1）→ PCA9685，地址 `0x40`
- 参照 `robots/spider1.sql` 写自己的结构脚本：I2C 表 → PCA9685 表 → Servo 表（Servo 的 controller 列指向 `pca9685-0`，channel 对应接线通道）
- 跑站立动作：

```bash
cat scripts/std.mt | bash _run.sh --robot=robots/spider1.sql
```

脚本语法细节见下文「机器人结构脚本」「动作脚本」两节；报错信息会指出是哪张表、哪一行、哪一列的问题，按提示修正即可。

## 📱 运行示例

项目中包含的动作脚本都在 `scripts/` 文件夹下，适配 `robots/spider1.sql` 的 14 路舵机结构（通道分配见该文件注释）：

```text
# PCA9685 通道分配（参照 robots/spider1.sql）
s1   rb0  右后腿·髋      ch6     s9   cam0 云台水平  ch0
s2   rb1  右后腿·大腿    ch5     s10  cam1 云台垂直  ch1
s3   lb0  左后腿·髋     ch14     s11  rb2  右后腿·膝  ch4
s4   lb1  左后腿·大腿   ch15     s12  lb2  左后腿·膝  ch7
s5   lf0  左前腿·髋      ch8     s13  lf2  左前腿·膝 ch13
s6   lf1  左前腿·大腿    ch9     s14  rf2  右前腿·膝 ch10
s7   rf0  右前腿·髋     ch11     spk  Timer 仅负责延时
s8   rf1  右前腿·大腿   ch12
```

除基础的站立 `std.mt` 外，`scripts/` 里还有转向、倾斜、举手、躺下、原地跳等动作脚本：

```bash
cat scripts/left-turn.mt | bash _run.sh --robot=robots/spider1.sql   # 左转
cat scripts/tilt-r.mt    | bash _run.sh --robot=robots/spider1.sql   # 右倾
cat scripts/one-shake.mt | bash _run.sh --robot=robots/spider1.sql   # 单腿姿势
```

机器人结构由 `--robot` 指定，动作脚本从管道输入——只要换结构脚本并调整动作脚本，就可以把同一套程序复用到另一台机器人上。

## 🔧 环境要求

### 支持的版本

**已验证的开发环境：**

| 组件          | 版本信息 | 状态    |
| ------------- | -------- | ------- |
| 仓颉语言      | v1.1.3   | ✅ 支持 |
| 仓颉 stdx     | v1.1.3   | ✅ 支持 |
| LuaJIT        | 5.1（libluajit-5.1-dev）| ✅ 必需 |
| Linux glibc   | 发行版自带 | ✅ 必需 |

**依赖包：**

| 依赖包 | 用途 |
| ------ | ---- |
| [luajit4cj](https://gitcode.com/qingss0/luajit4cj) | LuaJIT 的仓颉绑定，Lua C API 一律经其调用 |
| [pthread4cj](https://gitcode.com/qingss0/pthread4cj) | 守护线程共享内存槽的互斥锁封装 |
| [SQL4cj](https://gitcode.com/qingss0/SQL4cj) | 机器人结构脚本（.sql）的解析 |
| mt2、ctype4cj | 矩阵与基础工具支持 |

### 平台兼容性

| 平台                     | 支持状态      | 备注                                             |
| ------------------------ | ------------- | ------------------------------------------------ |
| Linux（aarch64 / x86_64）| ✅ 完全支持   | 主要平台，树莓派实测稳定                         |
| Windows                  | ❌ 暂不支持   | 新版 HAL 加载 Linux glibc，Windows 通常没有     |
| macOS                    | ❌ 暂不支持   | 计划支持                                         |

由于底层的系统接口调用（I2C/SPI 总线、UART）被下沉到了脚本端，程序本身很容易跨平台编译运行。

## 📚 核心概念与 DSL

前文的比喻换成正式说法：结构脚本 `.sql` 以 SQL 方言声明机器人硬件结构与调用链，驱动脚本 `.lua` 在 LuaJIT 里实现每个硬件变量的底层驱动，动作脚本 `.mt` 以行列式 DSL 编排机器人的动作。下面逐一展开。

### 机器人结构脚本（.sql）

结构脚本是 `.sql` 文件，由 SQL4cj 解析，支持 `--` 单行注释与块注释（旧版 `.mt` 结构矩阵与 `.bot` 格式已废弃）。内容就是一组 `CREATE TABLE / INSERT INTO`：每张表对应一类硬件（表名即矩阵名，决定加载哪个驱动），每行 INSERT 就是一个具体的硬件实例：

```sql
/* 定义 I2C 总线 */
CREATE TABLE I2C (
	name String,
	path String
);
INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');

/* 定义 PCA9685 舵机控制器，挂在 i2c-1 的 0x40 地址 */
CREATE TABLE PCA9685 (
	name    String,
	I2C     String,
	address String
);
INSERT INTO PCA9685 (name, I2C, address) VALUES ('pca9685-0', '$i2c-1', '0x40');

/* 定义舵机：max 限幅（负值表示方向反向）、offset 中位漂移，"-" 继承上一行 */
CREATE TABLE Servo (
	name       String,
	max        String,
	min        String,
	offset     String,
	controller String,
	channel    String
);
INSERT INTO Servo (name, max, min, offset, controller, channel) VALUES (s1, 90, -, -, 'pca9685-0', 0);	-- 左下水平关节
```

> 说明：`-` 是占位符，表示继承上一行同列的值（如 s2 之后的 controller 继承 s1 的 `pca9685-0`）；`I2C` 列以 `$i2c-1` 引用上方定义的总线硬件变量（0.3.2 起引用列须 `$` 前缀，裸名一律按字面值），框架把总线 `call()` 返回的 `fd` 合并进本变量的元数据；注释整行 INSERT 即可下线一个硬件实例。

### 硬件变量矩阵一览

**先看结论**：这张表是"零件仓库清单"——用不到的器件直接跳过，等真接了某个传感器再回来对号入座。目前框架内置 53 个硬件驱动，按总线/能力分类如下（每个驱动就是一个 `modules/<矩阵名>.lua`）：

| 类别 | 硬件变量 | 说明 |
| ---- | -------- | ---- |
| 舵机/输出 | `PCA9685`、`Servo`、`Timer`、`MCP4725`、`AD9833`、`TPL0501`、`PCF8574`、`GPIO`、`PWM` | 舵机控制器、DAC、DDS 波形、数字电位器、IO 扩展、GPIO、硬件 PWM |
| IMU/姿态 | `MPU6050`、`BMI270`、`LSM6DSV`、`ADXL345`、`MLX90393`、`BMM150` | 六轴/三轴加速度计、磁力计、罗盘 |
| 气压/环境 | `MS5611`、`LPS22HB`、`BME280`、`BME688`、`SHT3X`、`CHT8305`、`AMG8833`、`AS7262`、`APDS9900`、`BH1745` | 气压计、温湿度、热像、光谱、环境光、颜色 |
| 测距 | `VL53L0X`、`VL53L4CD`、`AS3935` | ToF 激光测距、闪电事件传感器 |
| ADC/电源 | `ADS1015`、`ADS1115`、`ADS1256`、`MCP3421`、`INA219`、`INA226`、`INA3221`、`MAX17043`、`MAX17048`、`LTC2944`、`EMC2101` | 高速 ADC、电流/功率监测计、库仑计、电量计、风扇控制器 |
| 气体/其他 | `SGP30`、`SDS011`、`MHZ19`、`MAX30102`、`MT6826S` | 空气质量、PM2.5、CO2、血氧 PPG、磁编码器 |
| 框架 | `I2C`、`SPI`、`UART`、`Relay`、`dummy`、`Kalman`、`Ema`、`Monitor` | 总线、转发测试、滤波器、数据消费示例 |

### 硬件 PWM（`PWM`）

`PWM` 走 Linux 标准 sysfs 接口（`/sys/class/pwm/pwmchipN/pwmM`），波形由 SoC 的 PWM 控制器产生，Lua 侧只写 `period` / `duty_cycle` / `enable` 三个属性——不掉 CPU、不抖动。

> **为什么不做软件 PWM**：在 `call()` 里循环翻转会阻塞整套动作的播放；放到 `@Daemon` 里用 ffi `usleep` 自旋又会占住 schd-worker 不放、耗尽 worker 池（见下文「@Daemon 守护线程」）。所以本框架只支持硬件 PWM。

```sql
CREATE TABLE PWM (
	name      String,
	chip      String,
	channel   String,
	frequency String,
	duty      String,
	invert    String
);
INSERT INTO PWM (name, chip, channel, frequency, duty, invert)
  VALUES ('pwm0', '0', '1', '20000', '0', '-');
```

- `channel` 是**硬件通道号**（`0` = PWM0，`1` = PWM1），**不是 GPIO 引脚号**——填引脚号会直接报越界
- `frequency` 载波频率 Hz（缺省 1000）：电机 10k~20kHz，LED 1k 上下
- `duty` 占空比 0~100；给了才在 build 期使能输出，不给则保持关闭、等第一帧再输出（与 `GPIO` 的 `init` 语义一致）
- `invert` 填 `1` 输出反相（低电平有效的驱动板）
- 动作脚本里写 `pwm0.duty`；也可以写 `pwm0.value`（`value` 是 `duty` 的别名）

#### 从哪个物理脚输出：设备树说了算

sysfs **没有**"选引脚"这个属性——引脚由开机时的设备树静态绑定，在 `/boot/firmware/config.txt` 里配，**改完必须重启**：

```ini
dtoverlay=pwm-2chan,pin=12,func=4,pin2=13,func2=4
```

BCM2710（CM0 / Pi Zero 2 W / Pi 3）的合法组合：

| 通道 | 可选引脚（func 值） |
| ---- | ------------------ |
| PWM0 | GPIO12 (`func=4`)、GPIO18 (`func=2`)、GPIO40、GPIO52 |
| PWM1 | GPIO13 (`func=4`)、GPIO19 (`func=2`)、GPIO41、GPIO45、GPIO53 |

用 `raspi-gpio get <pin>` 确认复用是否生效（应显示 `ALT0` / `ALT5`，而不是 `INPUT` / `OUTPUT`）。

> ⚠️ `export` 成功**不等于**有物理输出：overlay 没给该通道配脚时，`period` / `duty` 照样写得进去，但引脚仍是普通 GPIO，外面量不到波形。

注意事项：

- 同一引脚不能既被 `GPIO` 用 gpiod 请求、又被配成 PWM（功能复用互斥，二选一）
- `export` / `unexport` 默认只有 root 可写，普通用户需 udev 授权，或以 `sudo -E` 运行（`-E` 保留 `CANGJIE_HOME` / `LD_LIBRARY_PATH`）
- 取消导出后引脚回到未配置状态，停机后的电平要靠硬件上下拉兜底

### 动作脚本（.mt）

动作脚本用一张表格描述一帧一帧的动作：表头是「硬件变量.参数」（相当于台词名），每一行就是一帧，程序逐行往下"播放"：

```mt
# 行动矩阵为硬件变量输入参数
@Action
s1.A  s2.A  s3.A  s4.A  s5.A  s6.A  s7.A  s8.A  spk.F  spk.delay;	# column name
  45     -    45     -    45     -    45     -     10        200;	# stand
  90     -     -     -     -     -     -     -      -         20;	# left-behind
@
```

- 动作脚本也可以内联定义 `@Alias` 别名矩阵（如把 `s1.A` 写成 `s1.angle`），无需写进结构脚本
- 结构脚本与动作脚本两侧都可以定义别名，同名时动作脚本的定义覆盖结构脚本（会打印覆盖日志）
- 同一动作脚本内重复定义同名别名会报错

### controller 引用机制（通用）

可以理解成**"转交任务"**：某个硬件变量收到指令后，自己先干一份活，再把结果转交给另一个硬件变量。这不是 `Servo` 的专利——任何硬件矩阵都可以声明一个 `controller` 列，把自己的调用**转发**给另一个硬件变量：

1. 先运行自己的 Lua 驱动（`modules/<矩阵名>.lua` 的 `call`）
2. 框架再把「该行元数据（跳过 `name`/`controller` 列）∪ 自己 Lua 的返回表」发送给 `controller` 指定的硬件变量
3. `controller` 的值支持 `-` 占位符继承上一行
4. 被引用的 controller 必须在结构脚本中先于引用它的矩阵定义

可运行示例（`Relay` 把参数转成 `delay` 后交给 `Timer` 延时）：

```sql
CREATE TABLE Timer (name String);
INSERT INTO Timer (name) VALUES ('timer-0');

CREATE TABLE Relay (
	name       String,
	controller String,
	note       String
);
INSERT INTO Relay (name, controller, note) VALUES ('relay0', 'timer-0', 'hello-relay');
```

#### 引用列（`$` 前缀）

除 `controller` 列外，元数据列还可以**按值引用**另一个硬件变量：值以 `$` 开头（如 `$imu0`）即为引用列，被引用变量的 `read()` 快照字段会实时合并进本变量的元数据；**不带 `$` 的裸名一律按字面值**（0.3.1 及之前按"值恰好是已定义硬件变量名"隐式判定引用，已废弃）。`$` 指向未定义的硬件变量会在 build 期显式报错：

```sql
CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$imu0');   -- 引用守护 imu0（$ 前缀必填）
```

### 基层硬件抽象层（HAL）

总线怎么"开口说话"（打开设备、读写寄存器的底层动作）都封装在 `modules/lib/HAL.lua`，所有驱动共用这一份，**仅支持 LuaJIT**：

- `ffi` 直调 glibc（open/ioctl/read/write/usleep），原生性能零 fork；fd 为真实内核句柄，跨 Lua 状态机天然有效
- **I2C 经 `I2C_RDWR` ioctl 原子事务**：读寄存器的「写指针 + 读」合并为单次内核传输，消除守护线程与主线程并发访问同一器件时的寄存器指针竞态；地址由每条 i2c_msg 携带，不再需要 `I2C_SLAVE` ioctl（也没有地址被内核驱动占用的 EBUSY 问题）
- 驱动模块顶部 `dofile` 加载 HAL，函数名与原 cfunc 完全一致；Lua 5.3+ 位运算经 `bitcompat.lua` 双兼容
- `modules/lib/` 还收纳 `kalman.lua`（一维卡尔曼滤波器）等公共库，不依赖 HAL 的模块仍可用 `luajit`/`lua5.4` 独立调试

### @Daemon 守护线程

有些传感器需要**一刻不停地读**（比如 IMU 姿态）。这活儿靠 `@Daemon` 声明的**后台小工**来做：宿主用**仓颉 `spawn` 原语**起一个守护线程，在后台循环采样，主线程随时去取最新值：

```sql
CREATE TABLE Daemon (
	name  String,
	chunk String
);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'lib/daemon_probe.lua');
```

> chunk 寻址：与普通 Lua 硬件模块一致，相对路径以 `BOT4CJ_MODULE_PATH`（默认 `./modules`）为基准，不依赖进程 CWD——编译产物拷到任意位置、设置该环境变量指向模块目录即可运行。写全 `modules/lib/xxx.lua`（或绝对路径）也兼容。`BOT4CJ_MODULE_PATH` 支持用 `:` 分隔多个路径（如 `./modules:./extra_modules`），框架与驱动按序查找第一个存在的文件；**约定共享库目录（含 `lib/` 与 `mpath.lua`）放第一段**（issue #19）。

并发模型的关键点：

- **必须用 `spawn`**，不能用 pthread4cj 裸线程：foreign pthread 没有仓颉运行时线程态，在其上跑 @C 代码必崩（PoC 实测 3/3 SIGSEGV）；spawn 是仓颉的 M:N 绿色线程，运行时已附着，安全
- 守护线程持有**独立 lua_State**（`jit.off`），与主线程状态机 1:1 隔离，规避 LuaJIT VM 非线程安全
- **让出式节拍**：拍间用仓颉 `sleep(Duration)` 让出 worker；切忌在 chunk 内用 ffi `usleep` 自旋循环（会阻塞 worker 不放，守护线程数 ≥ worker 数时池耗尽是死锁）
- **IPC 经共享内存槽**：守护侧 Lua `P.publish(slot, {...})` 写值，主线程侧 `play()` 加锁读值，同一 pthread_mutex 防撕裂
- `@Daemon` 矩阵除 `name`/`chunk` 外的列会序列化为 `DAEMON_CONFIG` 串注入 chunk

**万能采样 chunk（零 per-sensor 代码）**：`lib/sensor_daemon.lua` 服务任意 build/call/destroy 型驱动，无需为每个传感器写 Lua：

```sql
CREATE TABLE Daemon (
	name String, chunk String, driver String, proto String,
	path String, addr String, interval String, fields String, sign String
);
INSERT INTO Daemon (name, chunk, driver, proto, path, addr, interval, fields, sign)
  VALUES ('imu0', 'lib/sensor_daemon.lua', 'MPU6050', 'i2c',
          '/dev/i2c-1', '0x68', '10', 'ACCEL_X|ACCEL_Y|ACCEL_Z|GYRO_X|GYRO_Y|GYRO_Z|TEMP:f', '16');
```

`fields` 用 `|` 分隔以避开 SQL 值分隔符逗号；`sign=16` 表示对提取值做 16 位补码符号扩展。字段可带 `:模式` 后缀逐字段覆盖：`:f`/`:float` 原样透传浮点（如 MPU6050 的 `TEMP` °C，避免被 `sign=16` 取整破坏），`:s16` 强制 16 位补码；无后缀跟随全局 `sign`。

**滤波器硬件变量**：每个滤波器 = `lib/<filter>.lua` 库（统一 `.new()` + `:filter(x)` 接口）+ `<Filter>.lua` 驱动。驱动引用守护/传感器，call 逐拍滤波，输出既作 `out` 也别名 `v0`，成为「虚拟传感器」。现有滤波器：

- **Kalman**（一维卡尔曼）：配置 `Q`/`R`（系统/测量噪声协方差）
- **Ema**（指数移动平均）：配置 `alpha`∈(0,1]（越小越平滑）、可选 `init`

三级引用链示例（守护采样 → 滤波 → 消费）：`imu0 → kf0 → mon0` 全靠既有引用机制自动串联（播放 mon0 会递归播放 kf0、imu0）。

> 注意：守护线程 setup（开总线 + 配置传感器）需数毫秒；若动作序列极短（几行瞬时 play），可能在守护首次采样前就 free() 停止（表现为 publishes=0）。真实机器人动作序列为秒级，无此问题；测试时用 Timer 延时列拉长主线程时间窗。

#### 无硬件跑 Daemon（可选自检）

想看守护机制本身转起来，不必上真传感器——两个纯软件"模拟传感器"就够了：`lib/daemon_probe.lua`（内存槽发布计数/正弦）与 `lib/daemon_tid.lua`（gettid + 计数），扎到的仍是同一份 `Daemon.cj` 槽分配/加锁代码。`cjpm build` 后逐条跑，退出码 0 即通过：

```bash
cat scripts/test/daemon.mt      | bash _run.sh --robot=robots/test/daemon.sql     # 单守护并发压测（主线程负载 × 计数守护）
cat scripts/test/multidaemon.mt | bash _run.sh --robot=robots/test/multidaemon.sql # 4 守护并发诊断（各持独立 lua_State）
cat scripts/test/monitor.mt     | bash _run.sh --robot=robots/test/monitor.sql    # 守护采样 → Monitor 引用消费闭环
```

对比：`scripts/test/mpu_daemon.mt` 挂在真实 `MPU6050`（`/dev/i2c-1`, 0x68）上，那一条才需要真机。

#### 虚拟 I2C 总线（issue #18，无真机验证驱动链路）

`lib/virtbus.lua` 提供纯内存的虚拟 I2C 总线：`inject()` 后替换 HAL 的全部全局 `i2c_*` 函数，真实驱动（如 BH1750）无需真机与 `/dev/i2c-*` 即可完整跑通 build/call/destroy。器件行为由 `attach(name, addr, dev)` 挂载的模型回调决定（`write(bytes)` 收命令/寄存器写、`read(n)` 返回 n 字节），每次事务记入 `vbus.txlog` 供断言；未挂载的地址按 NACK 处理（写 -1、读 -1/nil），模型自身也可 `write` 返回 `false`/`read` 返回 `nil` 注入 NACK（模拟掉线），负路径与真机"设备不在总线"行为一致。指针式读族（`read_reg/multi/reg16`）严格复刻 HAL 的 I2C_RDWR「写指针 + 读」事务，模型能知道当前读的是哪个寄存器；`read/read_bytes` 为无指针纯读。

```lua
local vbus = dofile("modules/lib/virtbus.lua")
vbus.attach("光感@0x23", 0x23, {
    write = function(bytes) ... end,
    read  = function(n) return { 0x75, 0x30 } end,   -- 假计数值
})
vbus.load_driver("modules/BH1750.lua")  -- dofile 真实驱动（连带 HAL）并自动重注入
-- build/call/destroy 正常跑；tester 断言 txlog 与返回值
vbus.reset()           -- 还原全局 i2c_* 并清空
```

标准流程五步：`vbus.reset()`（用例隔离）→ `attach(addr, 模型)` → `inject()` 或
`load_driver(驱动路径)`（挂真实驱动时用后者，自动重注入）→ 驱动 build/call/destroy
+ 断言 `txlog`/返回值 → `restore()` 或 `reset()`。

**virtbus.lua API**：

| API | 说明 |
| --- | --- |
| `vbus.attach(name, addr, dev)` | 在 7 位地址挂模型；`name` 仅日志用。保存模型引用并**动态委托**——attach 后替换 `dev.write/read` 或改模型字段（如 `dev.nack=true`）即时生效，无需重挂；同一模型可挂多个地址（主/备地址测试） |
| `vbus.inject()` | 替换全部 12 个全局 `i2c_*` 为虚拟实现；幂等（已注入直接返回） |
| `vbus.restore()` | 还原注入前捕获的原始全局函数（真实 HAL 函数） |
| `vbus.reset()` | `restore()` + 清空器件表、txlog、fd 线索；每个用例开头调用保证隔离 |
| `vbus.load_driver(path)` | `dofile` 真实驱动后强制重注入（驱动首次 dofile HAL 会用真实 ffi 函数覆盖全局）；并保证 `restore()` 能还原到真实 HAL 而非置空 |
| `vbus.txlog` | 事务序列数组（见下），驱动行为断言的唯一凭据 |
| `vbus.devs` | 当前地址 → 挂载项的表 |

`i2c_open` 走与 HAL 相同的路径白名单（仅匹配 `/dev/i2c-%d+`，非法路径返回 -1），
虚拟 fd 从 3 起分配；`i2c_set_addr` 后该 fd 的事务才路由到对应模型，未 set_addr
或地址无模型一律按 NACK 处理。

**模型回调契约**（`attach` 的 dev 表，函数为点定义）：

- `write(bytes)`：收到 1 基字节表（写族为 `{reg, val...}`，指针式读前还会收到
  单字节/双字节的寄存器指针）。**显式 `return false` = NACK**；无返回值/nil = 成功。
- `read(n)`：返回 **1 基字节表**（长度可少于 n，按实际组装）；**返回 nil = NACK**；
  不提供 `read` 时缺省返回 n 个 0。n 已按 HAL 契约钳制（字节表族 1~16）。

```lua
local dev = { armed = false }
function dev.write(bytes)        -- 点定义：无 self 参数，引用自身用 dev
	if bytes[1] == 0x01 then dev.armed = true end
end
function dev.read(n)
	if not dev.armed then return nil end       -- 未武装先 NACK（负路径）
	return { 0x00, 0x68 }                      -- 动态读数（改 dev.armed 即时生效）
end
vbus.attach("自定义@0x68", 0x68, dev)
```

**txlog 条目格式**（按时间序追加；指针式读产生相邻的 `w` + `r` 两条）：

| dir | 含义 | addr | bytes |
| --- | --- | --- | --- |
| `"open"` | 打开虚拟总线 | nil | `{ raw = 路径 }` |
| `"set"` | `i2c_set_addr` 绑定地址 | 地址 | `{}` |
| `"w"` | 写事务（含读操作的寄存器指针写） | 地址 | 1 基字节表，如 `{0x3B}`、`{0x6B, 0x00}` |
| `"r"` | 读事务 | 地址 | `{ n = 请求字节数 }`（实际数据由模型 read 返回，不入日志） |

virtbus.lua 本身是**纯 Lua、无 ffi 依赖**（lua5.4/luajit 都能离线跑总线契约）；
只有经 `load_driver` 加载真实驱动时才需要 LuaJIT（HAL.lua 依赖 ffi）。

**现成器件模型库 `lib/virtbus/`**（协议层模型，只复现寄存器/字节序/burst 等事实，测量值由用例显式注入，避免模型与驱动按同一错误理解互证而假绿）：

- `virtbus/regmap.lua`：通用「寄存器指针 + burst」模型，覆盖 MPU6050/BME280 一类大多数 I2C 传感器（8 位指针；构造传 `{ptr16=true}` 支持 VL53Lx 类 16 位指针）；提供 `set/set_word/get_sword` 预置寄存器、`nack`（读写全断）与 `nack_read/nack_write`（定向故障）、`write_log` 事务留痕、`reset()`。
- `virtbus/mpu6050.lua`：MPU6050 六轴 IMU 模型（regmap 之上固化寄存器事实）——WHO_AM_I 身份（可 `who(0x71)` 模拟错挂）、`set_accel/set_gyro/set_temp_raw` 注入有符号 LSB、`sample()` 一次填满 14 字节 burst、`set_accel_g/set_gyro_dps/set_temp_c` 按 datasheet 灵敏度以物理量造样本（供守护/E2E）、`virt_tick(n)` 造确定性合成波形（供 proto=virt 守护逐拍驱动）、`config_writes()` 取 build 配置序列。
- `virtbus/bme280.lua`：BME280 温压湿环境传感器模型（regmap 包装层）——0xD0 身份（`who(0x58)` 模拟错挂 BMP280）、datasheet 示例校准 NVM 字节布局（0x88 起 T/P 小端系数、0xA1 H1、0xE1 起 7 字节 H 块含 H4/H5 的 s12 nibble 拼装）、0xF7 起 P/T 20bit + H 16bit 数据区、合成 0xF3 STATUS 与 forced 转换状态机（`advance(ms)` 外部虚拟时钟驱动 6ms 转换/`nvm_stuck`/`meas_stuck` 卡死注入；`virt_tick` 走预武装路径无需时钟）、`set_raw(t,p,h)` 注入无符号 ADC 码（0x80000/0x8000 无效读数可测）。
- `virtbus/bh1750.lua`：BH1750 命令式器件模型（opcode 序列、大端计数读回、上电/模式状态解释、`set_count()`、`nack`）。

```lua
local mpmod = dofile("modules/lib/virtbus/mpu6050.lua")
local imu = mpmod.new()                      -- WHO_AM_I 默认 0x68
imu:sample({ ax = 1, ay = 2, az = 3,         -- 有符号原始 LSB
             gx = -4, gy = -3, gz = -2, temp = 340 })
vbus.attach("MPU6050@0x68", 0x68, imu)
```

> 契约对齐：虚拟总线逐函数复刻 HAL 返回值——`i2c_write_reg` 为**布尔**（驱动用 `if not` 判定，若错返 -1 会因 Lua 真值规则造成 NACK 假成功）、`write_multi/reg16` 为 0/-1、读族长度约束（合并值 1~6/1~8 越界显式失败，字节表钳 1~16）一致。

**守护无硬件端到端（`proto=virt`）**：`@Daemon` 的 `proto` 列填 `virt`，sensor_daemon 不碰真机，而是挂 `lib/virtbus/<driver 小写>.lua` 器件模型——真实驱动 build/call、共享槽 publish、`@Ema/@Kalman`、`@Monitor` 引用消费整条链路全部走内存仿真。模型若提供 `virt_tick(n)`，守护每拍先驱动它造**确定性合成波形**再采样（无该方法则输出静态初值）；驱动无对应模型/缺 `addr` 时 setup 显式 fail(-8)，守护空转不崩。守护启动打印醒目 `VIRTUAL HARDWARE` 横幅（仿真数据禁止用于生产环境）。现有两例：`mpu_virt`（七字段 IMU 守护 + 七路 EMA + 两 Monitor）与 `bme280_virt`（三字段温压湿守护 + 三路 EMA + 两 Monitor，真实驱动跑附录 8.3 补偿、模型只给 NVM/位场）：

```bash
cat scripts/test/mpu_virt.mt    | bash _run.sh --robot=robots/test/mpu_virt.sql
cat scripts/test/bme280_virt.mt | bash _run.sh --robot=robots/test/bme280_virt.sql
```

`run_all.sh` 在检测到编译产物 `target/release/bin/main` 与仓颉 runtime 时，按用例表（用例名｜`:f` 浮点字段断言｜count 下限）自动执行仓颉级 E2E：通用断言横幅、无错误、守护持续发布 count 增长、EMA 至少一拍 `out ≠ raw`（键序无关的逐行数值比对），另加每用例的浮点字段未被取整断言；环境缺失则逐例跳过，不阻断纯 Lua 回归。

回归：`test/test_virtbus.lua`（纯 Lua，43 断言：功能/指针事务/长度约束/两级 NACK/还原）、`test/test_virtbus_regmap.lua`（纯 Lua，37 断言：regmap + ptr16 + 定向故障 + 总线集成）、`test/test_virtbus_bh1750.lua`（LuaJIT，真实 BH1750 build/call/destroy + 动态改读数 + 掉线 + 无器件负路径）、`test/test_mpu6050.lua`（LuaJIT，真实 MPU6050 全链路：build 配置序列/WHO_AM_I 甄别/0x69 备地址/14 字节 burst/六轴补码/温度换算/读写两个方向 NACK/virt_tick 仿真波形，50 断言；原手写桩 test_mpu6050_sign 为其严格子集已合并删除）、`test/test_bme280.lua`（LuaJIT，真实 BME280 全链路 51 断言：复位/校准块 6×4B+7B 长读/CTRL 回读复核/forced 状态机/20bit 数据位场/0x80000 与 0x8000 无效读数/超时/主备地址/附录 8.3 独立参考交叉验证——迁移时模型按字节宽度编码戳破旧桩 H1=260 的超宽假值，datasheet 真值 H1=75 对应 45.3%RH 档）；均由 run_all.sh 自动纳入。

### 环境变量

| 变量 | 说明 |
| --- | --- |
| `BOT4CJ_MODULE_PATH` | 驱动模块目录（默认 `./modules`），框架加载 `<矩阵名>.lua`、驱动 dofile `lib/` 与 `@Daemon` 的 chunk 均按其解析；支持 `:` 分隔多个路径（如 `./modules:./extra_modules`）按序查找，**共享库目录放第一段**（issue #19） |
| `BOT4CJ_DEBUG` | 设为 `1` 开启 call/build/destroy 的参数表、结果表、逐键值、lua status 详细转储；缺省静默，错误信息与生命周期摘要不受影响 |

安全说明：框架加载 Lua 驱动后会中和 `os.execute` / `io.popen`（shell）、`os.exit`（进程退出）、`io.open` / `io.lines` / `io.input` / `io.output` / `os.remove` / `os.rename`（任意文件读写）；`os.getenv`、`dofile`、`require`（HAL 依赖 ffi）保留。**该沙箱是防误用护栏，不是安全边界**——Lua 驱动可经 `require("ffi")` 直调任意 libc，且结构/驱动/动作脚本均为可信输入；未来接入 TCP 等外部输入时需另行建立信任边界（输入校验或白名单 API）。

## 💡 进阶示例：守护采样 → 滤波 → 消费闭环

用 `@Daemon` 起守护线程采样 IMU，接 Kalman 滤波，再用 `Monitor` 消费并转发执行器，形成闭环：

```sql
CREATE TABLE Daemon (name String, chunk String);
INSERT INTO Daemon (name, chunk) VALUES ('imu0', 'lib/sensor_daemon.lua');   -- 采样

CREATE TABLE Kalman (name String, Q String, R String, src String, field String);
INSERT INTO Kalman (name, Q, R, src, field) VALUES ('kf0', '20', '200', '$imu0', 'v0');  -- 卡尔曼

CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$kf0');   -- 消费滤波值
```

`modules/Monitor.lua` 即此类示例：`call()` 里 `data.v0` 拿到实时采样值，决策后可经 controller 列转发执行器。

## ⚠️ 已知限制

### 当前版本限制

- **仅支持 Linux**：HAL 通过 `ffi` 直调 glibc，Windows 平台没有现成的 glibc，暂不支持（旧版曾支持，0.3.0 起移除）
- **仅支持 LuaJIT**：总线驱动依赖 `ffi`，需 `luajit` 解释器；不依赖 HAL 的模块可用 `lua5.4` 独立调试
- **守护线程的开销**：每个守护持有独立 lua_State，拍间必须让出式 sleep，否则会耗尽 worker 池
- **一器件一消费者**：页式寄存器序列的器件（如 VL53L0X）与握手式间接访问器件（如 AS7262）只支持单消费者

### 待实现功能

- [ ] **TCP 传输动作**：目前动作脚本经管道输入，Windows 上可能没有管道，未来计划推出 TCP 持续传输
- [ ] **神经网络生成动作**：神经网络/强化学习路径（DRL、CNN）已在本仓库历史上实现过，当前版本暂时注释，计划结合 `nn4cj` 重新接入
- [ ] **更多操作系统适配**：鸿蒙终端、macOS、开源鸿蒙机器人
- [ ] **驱动生态扩充**：按模板新增一个 Lua 文件即可即插即用，欢迎社区贡献

## 🔧 开发与贡献

### 本地开发

```bash
# 克隆仓库
git clone https://gitcode.com/qingss0/bot4cj.git
cd bot4cj

# 安装依赖并构建
bash _set.sh

# 运行离线测试（桩替身不触硬件）
bash test/run_all.sh

# 运行示例（无硬件自检链路）
cat scripts/relay-test.mt | bash _run.sh --robot=robots/relay-test.sql
```

### 贡献指南

我们欢迎所有形式的贡献！

- **🐛 报告 Bug**：[提交 Issue](https://gitcode.com/qingss0/bot4cj/issues)
- **💡 功能建议**：[功能请求](https://gitcode.com/qingss0/bot4cj/issues)
- **📝 完善文档**：改进 README、添加示例、翻译文档
- **🔧 代码贡献**：新增驱动、修复 Bug、优化性能
- **🧪 测试验证**：在不同总线上测试、补充离线用例

> 💡 **记住**：能写文档的写文档，能写代码的写代码，一起共建生态！

### 开发者

- **主要维护者**：[@qingss0](https://gitcode.com/qingss0)
- **团队**：Flowers_Unamed（BadHope）

## 📋 更新日志

### v0.3.3（2026-09）

- ✅ 新增硬件 PWM 驱动 `PWM`：走 Linux sysfs（`/sys/class/pwm`），可配频率与占空比。软件 bit-bang PWM 在本框架不可行（会阻塞动作播放或耗尽 worker 池），故仅支持硬件 PWM
- ✅ 修复 `.gitignore` 残留的 `cjpm.lock`（0.3.2 已把锁文件入库，忽略项与之矛盾）
- ✅ 修正 `_run.sh` 堆大小注释与默认值不符（注释写 512MB，实际 128MB）
- ✅ 修复 BME280 驱动 build 恒 ERR：湿度校准块 7 字节读超 HAL 合并读上限 6 字节，改走字节表版 `i2c_read_multi_bytes`；新增 `test/test_bme280.lua` 离线桩测试 33 用例（旧代码在桩下复现恒 ERR，防回归）

### v0.3.2（2026-09）

- ✅ 引用列改 `$` 前缀显式标记：元数据列引用硬件变量须写 `$名字`（如 `src='$imu0'`），裸名一律按字面值，消除「字面值恰与硬件变量名相同被误判为引用」的隐式行为；`$` 指向未定义变量在 build 期显式报错（含表/列/行信息）

### v0.3.1（2026-09）

- ✅ HAL 合并值读函数（i2c_read / i2c_read_multi / i2c_read_reg16 / spi_read_multi）n 越界改显式失败（返回 -1），消除静默钳到 1 字节放大成错误数据的隐患
- ✅ HAL uart_read 落实错误契约：非 EAGAIN/EINTR 的读错误立即返回 nil，不再空转到超时
- ✅ Servo 逐次打印收敛到 `BOT4CJ_DEBUG` 门控；`_run.sh` 增加过期二进制告警与产物缺失友好提示
- ✅ README 安全说明明确「护栏非安全边界」定位（详见 [CHANGELOG.md](CHANGELOG.md)）

### v0.3.0（2026-09）

- ✅ 守护线程机制 `@Daemon`：仓颉 `spawn` 绿色线程 + 每守护独立 lua_State + 共享内存槽 IPC
- ✅ 万能采样 chunk `sensor_daemon.lua`：零 per-sensor 代码
- ✅ 滤波器硬件变量：Kalman 与 Ema，支持「守护 → 滤波 → 消费」三级引用链
- ✅ 新增一批驱动：SHT3X、INA219/INA226/INA3221、MAX17043/17048、LTC2944、GPIO、MLX90393、AS3935、VL53L0X/VL53L4CD、MS5611、AS7262、BME688、BMI270、LSM6DSV、BMM150 等
- ✅ UART 总线能力（HAL termios + MH-Z19B 参考驱动）
- ✅ HAL 新增 16 位寄存器指针读写与多字节读；驱动样板收敛为 `drv_common.lua`
- ✅ Lua 驱动沙箱（中和 shell 与任意文件读写）
- ✅ 两轮审计加固：驱动返回值判读补齐、ADS1x15/ADS1256 datasheet 级修正、守护 IPC 护锁、总线 fd 生命周期收紧（详见 [CHANGELOG.md](CHANGELOG.md)）

### v0.2.0（2026-09）

- ✅ HAL 彻底甩掉宿主注册的 cfunc，进入 LuaJIT ffi 时代（此后仅支持 LuaJIT）
- ✅ LuaJIT 绑定抽取为独立包 [luajit4cj](https://gitcode.com/qingss0/luajit4cj)

### v0.1.1（2026-09）

- ✅ controller 引用机制：硬件变量可在矩阵里把执行转发给其它硬件变量
- ✅ 动作脚本支持内联 `@Alias` 定义

### v0.0.6（2026-09）

- ✅ 机器人结构脚本迁移到 SQL（SQL4cj 解析，支持注释）
- ✅ 新驱动：Timer、SGP30、Dummy

### v0.0.3（2026-05）

- ✅ 项目最大转折点：嵌入 Lua 状态机，硬件变量改由 Lua 脚本实现

### v0.0.1beta（2026-04）

- 🚧 项目起步：PCA9685 舵机驱动、蜘蛛机器人步态脚本、FFT/神经网络探索

完整历史见 [CHANGELOG.md](CHANGELOG.md)。

## 📄 开源协议

本项目基于 [MIT](LICENSE) 协议开源（Copyright (c) 2025 卿松），请自由地享受和参与开源。

## 📸 效果展示

### 运行视频

> 🎥 [查看完整演示视频](http://152.136.227.85:8080/videos/robot-show.mp4)

演示视频由 PC 端与机器人实拍两部分组成，展示了平面蜘蛛机器人按动作脚本完成站立、转向、倾斜、举手等动作。

## 🙋 常见问题

### Q: 报错提示看不懂怎么办？

A: 结构脚本的报错信息会明确指出是哪张表、哪一行、哪一列的问题；动作脚本报错会说明是哪个硬件变量与参数。按提示修正即可，也可以在 [Issues](https://gitcode.com/qingss0/bot4cj/issues) 中反馈。

### Q: @Daemon 守护线程为什么卡死？

A: 最常见的原因是拍间用 ffi `usleep` 自旋，占用 schd-worker 不放，导致 worker 池耗尽。请让守护采样 chunk 只定义全局 `daemon_tick`，拍间由仓颉 `sleep(Duration)` 让出。

### Q: 守护线程 publishes=0？

A: 动作序列极短（几行瞬时 play）时，守护可能在首次采样前就被 free() 停止。真实动作序列为秒级没有问题；测试时用 Timer 延时列拉长主线程时间窗。

### Q: I2C 设备报 EBUSY？

A: HAL 已改用 `I2C_RDWR` 原子事务，地址由每条 i2c_msg 携带，不再需要 `I2C_SLAVE` ioctl，一般不会再遇到内核驱动占用地址导致的 EBUSY。

### Q: 为什么不支持 Windows？

A: 新版 HAL 加载 Linux 的 glibc（open/ioctl/read/write/usleep），Windows 通常没有；动作脚本经管道输入，Windows 的 cmd/PowerShell 也没有原生的管道重定向。旧版曾支持 Windows，后续计划推出 TCP 传输与多平台适配。

### Q: 不接硬件也能开发吗？

A: 可以。`bash test/run_all.sh` 用桩替身跑全部 Lua 离线测试；`relay-test` 链路（Relay → Timer）不需要任何硬件，先跑通它即可验证安装与环境是否正确。

---

## 📞 联系我们

- 🐛 **问题反馈**：[GitCode Issues](https://gitcode.com/qingss0/bot4cj/issues)
- 🏠 **代码仓库**：[gitcode.com/qingss0/bot4cj](https://gitcode.com/qingss0/bot4cj)
- 📹 **演示视频**：[实机演示地址](http://152.136.227.85:8080/videos/robot-show.mp4)

**一起建设仓颉生态！** 🚀
