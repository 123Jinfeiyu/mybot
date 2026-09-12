# 更新日志

本文件记录 bot4cj 的所有重要变更，依据 git 提交历史整理（仅覆盖 main 主线；
`feature/bidirectional-communication` 等未合并分支不计入）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与 `cjpm.toml` 的 `version` 字段对应。

## [未发布]

审计修复批次：BH1750 MTreg 编码、多路径加载收尾、守护 CRC 坏样本过滤等；
新增 INA219 / INA3221 / SHT3X / LTC2944 / MAX17043 / TPL0501 离线桩测试覆盖；
@Daemon 共享槽采样通道 16 → 32（支撑单守护聚合多路传感器）。

### 新增

- **SHT3X 离线测试 `test/test_sht3x.lua`**（29 用例）：桩内模拟「软复位 0x30A2 ->
  轮询 status/ready -> 读 6 字节测量帧（status3+meas3）-> CRC8（poly 0x31/init 0xFF）
  校验 -> 温度 `T=-45+175*raw/65535` / 湿度 `RH=100*raw/65535` 换算」全链路，
  锁定关键预期值：CRC 通过样本 T/RH 换算结果、`CRC_OK=1`、坏样本（`bitc.bxor`
  翻转一字节）`CRC_OK=0` 负路径、缺 fd/address 与 read/set_addr/总线读失败、
  poll 模式 0x2400 写序列与 16ms 等待。
- **LTC2944 离线测试 `test/test_ltc2944.lua`**（30 用例）：桩内模拟「build 控制字
  计算（`0xC0 | (code<<3)`，code=预设表索引-1；缺省 M4096 -> 0xF0、M16 -> 0xD0、
  M1 -> 0xC0）-> VCELL/I/TEMP/CHARGE 换算」全链路，锁定关键预期值：
  `V=raw/65535*70.8`、`I=(raw-0x7FFF)/0x7FFF*0.064/rsense`、
  `T=raw/65535*510-273.15`、`CHARGE=chg*0.34e-3*(M*0.05)/(rsense*4096)`
  （M=4096、rsense=0.05、chg=1000 -> 0.34Ah）、缺省 M/rsense 归一、缺 fd/address
  与写控制字/读总线失败负路径。
- **MAX17043 离线测试 `test/test_max17043.lua`**（20 用例）：桩内模拟「build
  读版本寄存器 0x08 甄别存在性（只读型器件）-> VCELL（0x02，12-bit 左移 4 ->
  `>>4`，LSB 1.25mV）/ SOC（0x04，`raw/256` %）读」全链路，锁定关键预期值：
  满量程/标称电压换算、SOC 百分比、缺 fd/address 与版本读/总线读失败负路径、
  只读 build 不写任何器件寄存器（纯读取型驱动特征）。
- **TPL0501 离线测试 `test/test_tpl0501.lua`**（28 用例）：桩内模拟「SPI 写
  256 抽头电位器，优先级 level > percent > ohms，越界 clamp 0~255，
  `ohms=level/255*rtotal` 换算」全链路，锁定关键预期值：三档入参优先级、
  clamp 边界（-1->0、256->255、percent 80 -> 204）、`spi_write` 失败负路径、
  缺 fd 与 rtotal 缺省 100kΩ。
- **INA219 离线测试 `test/test_ina219.lua`**（32 用例）：桩内模拟「软复位 ->
  读回配置默认值 0x399F 甄别 -> 写工作配置 -> 等首拍」全链路，锁定的关键预期值：
  缺省工作配置 0x3DDF（bit13|PGA3|avg16code11|cont）、avg 降档 0x3C47/0x3E67/0x3F77、
  总线寄存器 >>3 后 LSB 4mV（16384 读值 -> 8.192V）、分流 16 位完整有符号无移位
  LSB 10µV（4000 -> 40mV）、反流 0xFC00 -> -10.24mV、INA226 误挂（复位配置 0x4127）
  识别、缺 fd/address 与 set_addr/总线读失败负路径、rshunt='-' 归缺省 0.1Ω；
  与 `test_ina226.lua` 形成姊妹对照——两颗芯片寄存器布局同构但换算完全不同
  （总线 13 位左移 3/4mV vs 16 位无移位/1.25mV，LSB 差 32 倍），专抓「照搬 INA226
  的换算」类移植笔误。
- **INA3221 离线测试 `test/test_ina3221.lua`**（34 用例）：桩内模拟「ID 甄别
  （0xFE=0x5449/0xFF=0x3220）-> 软复位 -> 写工作配置 -> 等首拍」全链路，锁定的关键
  预期值：缺省工作配置 0x7527（3ch|avg16code2|CT4|cont）、avg 降档 0x7127/0x7F27、
  三通道分流/总线 >>3 后分流 LSB 40µV/总线 LSB 8mV、反流 0xFC00 经 LuaJIT
  `bit.rshift` 无符号语义 -> -128 LSB -> -5.12mV、缺 fd/address 与 set_addr/总线读
  失败负路径、rshuntN='-' 归缺省 0.01Ω。
- **@Daemon 共享槽采样通道 16 → 32**（`src/HV/Daemon.cj` + `modules/lib/pthread.lua`
  两侧 ABI 同步）：单守护可由 16 通道提升到 32 通道，支撑"一个守护线程聚合多路
  传感器"（多 IMU / 多气感 / 里程计）场景。槽布局前缀 `count(64)/nvals(68)/values(72)`
  不变，仅尾部 `values[32]` 占 256B 后 `stop` 经 C 编译器 8 字节对齐落 328、槽总大小
  208 → 336（`gcc __builtin_offsetof` 实测；仓颉手写 `SLOT_*_OFF` 与 Lua `ffi.offsetof`
  逐项断言双向锁定，漂移 fail-fast）。`test/test_pthread.lua` 同步扩容（截断 32 +
  满额 32 通道往返），回归 58/0。

- **BH1750 MTreg 编码错误**（datasheet 级）：低字节命令原写 `0x60|((mt>>1)&0x0F)`，
  依 ROHM 手册改为 `0x60|(mt&0x1F)`（MT[4:0]）——原编码丢 bit0 且比特错位，
  mtreg 设置越大偏差越大（如 mt=127 实际生效约 103）；`test/test_bh1750.lua`
  原将错误编码固化为断言，一并修正
- **多路径模块查找对守护 chunk 与滤波驱动不生效**（issue #19 收尾）：
  `lib/sensor_daemon.lua`、`lib/daemon_probe.lua`、`lib/daemon_tid.lua`、
  `Ema.lua`、`Kalman.lua` 原用 `BOT4CJ_MODULE_PATH` 原始整串拼路径，环境变量含
  `:` 时 daemon setup 即失败（`cannot open ./modules:./extra_modules/lib/...`）；
  统一改为与其余驱动一致的「首段加载 mpath + `mpath.resolve` 按序查找」惯用法
- **守护消费端过滤 CRC 失败样本**：SHT3X/SGP30 的 CRC 失败响应（`result=OK` 且
  `CRC_OK=0`，直接 play 时避免 halt-on-error 中断动作）此前会被 sensor_daemon
  原样 publish 给下游；现 `daemon_tick` 对 `CRC_OK=0` 不发布、按 tick 错误计数与限频日志
- `Lua.cj` 沙箱补中和 `io.input` / `io.output`（LuaJIT 的这两者在 C 层直调 fopen、
  不经过已中和的 `io.open`）；模块层无使用，纯收紧
- `Lua.cj` Lua 表转 JSON 两处：混合键表（数字键 + 字符串键）原只出数组、静默丢字符串键，
  现整体转对象（数字键转字符串键）；数字键排序原对 `toArray()` 拷贝排序（无效），
  现按排序后的拷贝迭代
- `INA219.lua` call 补 not-built 守卫（原 build 前调用时 rshunt 为 nil，
  除法抛裸 Lua error，报错信息难看）
- `I2C.lua` / `GPIO.lua` build 重复调用先关旧 fd / 释放旧句柄（防重建泄漏，
  当前流程 build 恰一次，纯防御）
- **2026-09 审计修复批次**（驱动 datasheet 级四严重 + 中低项）：
  - **LTC2944 控制字写偏**（严重 S1）：CONTROL(0x01) 是 8 位寄存器，原 `write_reg16`
    发 `[0x01, 0x00, ctrl]` 三字节，器件寄存器指针自增使 CONTROL 实际收到 0x00
    （ADC 模式 00 = Sleep，build 后器件完全不转换、V/I/T 读陈旧值、电荷不累计），
    且每次 build 把电荷 MSB(0x02) 覆写成控制字（默认量程下约 ±21mAh 错位）；
    改单字节 `i2c_write_reg`。`test_ltc2944.lua` 桩改为逐字节自增建模 +
    `charge_msb_clobbered` 回归哨兵（旧实现在新桩下必红），并补 call-before-build 守卫
  - **BMM150 三轴多乘 0.3**（严重 S2）：补偿管线三轴均已 `/16` 收尾输出 µT
    （与 Bosch BMM150-SensorAPI master 的 int16 compensate_*「data(micro-tesla)」
    文档一致，Z 轴按驱动自身头注释即自相矛盾），call 再 ×0.3 使读数整体缩小
    3.33 倍；去掉系数并同步 `test_bmm150.lua` 黄金向量期望值
  - **INA219 SADC 平均档表错一位**（严重 S3）：datasheet 里 8 是 12 位单次的别名码，
    2~128 次平均应为 9~15；原表把 2 次映射到 8，使 avg≥2 的每一档实际平均次数
    减半（缺省"16 次"实为 8 次）。改 `{3,9,10,11,12,13,14,15}`、缺省 code 12，
    重算 `test_ina219.lua` 工作配置期望值（0x3E67/0x3CCF/0x3EEF/0x3FFF）
  - **MHZ19 帧头扫描死代码**（严重 S4）：`uart_read(fd, 9)` 单次最多返回 9 字节，
    帧前有任何噪声字节时"帧头扫描"数学上不可达（start>1 则帧越界），且
    `test_mhz19.lua` 旧桩忽略 `n` 返回 11 字节造成假绿；改 SDS011 同款多轮
    累积聚合读（校验和兜底），桩改 FIFO 逐轮弹出镜像 HAL 契约——旧实现在新桩下必红
  - **SHT3X 状态位映射错误**（中 M1）：数据手册 Table 18 实为 bit0=写校验和状态、
    bit1=命令状态、bit4=系统复位检测；原把 bit0/bit1 当命令/复位位，真命令错误
    漏检、复位检测形同虚设；`test_sht3x.lua` 状态值同步按正确位映射喂入
  - **MPU6050 补 WHO_AM_I(0x75=0x68) 甄别**：纯写配置的 build 让总线上任意会 ACK
    的器件蒙混过关（同批 IMU 均已甄别）；`test_mpu6050.lua` 补 0x75 读桩与
    错挂/读失败用例
  - **call-before-build 守卫补齐**：INA226/INA3221/MLX90393/LTC2944 未 build 先
    call 会做 nil 算术裸 error（违反 ERR 契约，INA219 原有守卫）；统一返回
    `not built (build first)`
  - **重复 build 守卫补齐**：SPI/UART/PWM 与 I2C/GPIO 对齐（旧 fd/导出通道泄漏）
  - **MCP4725 RDY 轮询区分总线错误**：i2c_read 失败（-1）原被当作"未就绪"、
    断线误报成"EEPROM 写超时"；现立即报总线读失败
  - **AMG8833 call 补 set_addr**：共享总线 fd 的从地址是 per-fd 状态，两次 call
    之间被别的器件寻址会把读数打到错误从地址
  - **Timer 延时上限**：>4294967ms 超 usleep unsigned int 上限会 32 位回绕成
    随机短延时，现显式报错
  - **Kalman build 非法 Q/R 转 ERR**：`KalmanLib.new` 对 Q<0/R<=0 直接 error，
    现 pcall 转 ERR 契约（与 Ema 一致）
  - **HAL.EINVAL 定义补上**：errno_hint 的 EINVAL 分支原引用未定义常量（死分支），
    现 `HAL.EINVAL = 22`
- `Servo.lua` / `Relay.lua` 日志格式化对浮点改 `%s`+`tostring`（原 `%d` 口径依赖
  LuaJIT 截断行为）；Relay 的逐帧输出为 README 首条命令的验收标记，保持不变
- `test_h07_fd.lua` 桩目录补拷 `lib/mpath.lua`（适配守护 chunk 的新加载链路）
- `sensor_daemon.lua` 守护退出清理先调驱动 `destroy()`（若有，`type(destroy)=='function'`
  守卫）再关总线 fd：让 VL53L0X 停止 ranging、ADS1x15 置单次态断电等器件退出活跃态
- 新增 `test/test_spi.lua`（26 断言：build 参数校验、call 前置检查、destroy 关闭、
  open/ioctl 失败路径）与 `test/test_ads1256.lua`（44 断言：build 寄存器写序列、
  call 单端/差分/负值/PGA 电压换算、destroy SDATAC、越界归缺省、失败路径）

### 文档

- `BME280.lua` / `SHT3X.lua` 头注释更新为现 HAL（纯 Lua 大端合并）语义，
  移除旧 cfunc 桥「有符号 Int64 回绕」的过时描述；负值还原分支注明为防御遗留

## [0.3.3] - 2026-09-11

新增硬件 PWM 驱动，修复 BME280 恒 ERR 与两处构建/配置一致性问题。

### 新增

- **硬件 PWM 驱动 `PWM`**（`modules/PWM.lua` + `modules/lib/HAL.lua` PWM 段）：
  走 Linux 标准 sysfs 接口（`/sys/class/pwm/pwmchipN/pwmM`），波形由 SoC 控制器
  产生，Lua 侧只写 `period`/`duty_cycle`/`enable`，不掉 CPU、不抖动。
  - **不做软件 bit-bang PWM**：`call()` 内循环翻转会阻塞整套动作播放；
    放到 `@Daemon` 里用 ffi `usleep` 自旋又占住 schd-worker 不放、耗尽 worker 池
    （README 已记该死锁）。故本框架只支持硬件 PWM。
  - **走 ffi 而非 io 库**：`sandbox_lua` 中和了 `io.open`/`io.lines`，sysfs 写不了；
    改用 `HAL.sys.open/write` 可保持沙箱不变。
  - **写序固定 duty→period→duty**：内核要求 `duty_cycle <= period`，降频时
    「新 period 小于旧 duty」会被 EINVAL 拒绝，故先把 duty 归零。
  - `chip`/`channel` 限定非负整数（≤65535）：既防路径穿越，也避免 `tostring`
    对大数输出科学计数法污染路径。
  - 驱动侧：`duty` 未配置时 build 期不使能输出（与 `GPIO.init` 语义一致，
    避免意外驱动负载）；配置失败回滚已导出的通道；destroy 先停输出再 unexport。
  - 离线测试 `test/test_pwm.lua` 分两层 mock（HAL 段断言 sysfs 写入路径/数值/写序，
    驱动层断言 build/call/destroy 参数校验与返回契约）；
    示例 `test/test_pwm.sql` + `.mt`（20kHz 占空比渐增渐减）

### 修复

- **`pwm_exported` 存在性探测改 `O_RDONLY`**：原用 `O_WRONLY` 打开 `pwmN/enable`
  判断是否已导出，而该文件默认 `0644 root:root`，非 root 用户拿到的是 EACCES
  而非「不存在」，被误判成未导出并反复 export
- **越界通道号前置校验 + 新增 `pwm_npwm`**：内核对超出 `npwm` 的通道号只回一个
  EINVAL 且静默不生成目录，「did not take effect」无法自解释。现 export 前先读
  `npwm` 校验并给出合法范围——最常见的误用是把 `channel` 填成 GPIO 引脚号
  （如 17），而引脚实际由 dtoverlay 决定，与 channel 无关
- **sysfs 写失败带 errno 人话提示**：原只打路径，无法区分 ENOENT
  （overlay 未启用/编号不对）与 EACCES（权限不足）
- **`.gitignore` 移除 `cjpm.lock`**：0.3.2 已把锁文件入库以保证构建可复现，
  但忽略规则未同步删除，二者矛盾——锁文件一旦被删/重建会再次被忽略
- **`_run.sh` 堆大小注释与默认值不符**：注释写「显式给 512MB」，实际默认 128MB。
  512MB 堆在 512MB 内存的板子（如 CM0）上会 OOM，128MB 为折中值；注释已改为
  说明折中理由与 `cjHeapSize` 覆盖方式（该中间值尚未经 A/B 验证）
- **BME280 湿度校准块读取超 HAL 合并读上限，build 恒 ERR（驱动完全不可用）**：
  build 读 0xE1~0xE7 共 7 字节湿度校准系数时调用合并读 `i2c_read_multi`，
  而该函数上限 6 字节（Lua number 2^53 精度约束，越界显式返回 -1，见
  `lib/HAL.lua`）——HAL 收紧（1~8 收至 1~6）后此调用点漏网，任何 BME280
  build 恒报 "Failed to read calibration"；因该驱动此前零测试覆盖，
  离线测试全绿也未能拦截。现改走字节表版 `i2c_read_multi_bytes`
  （HAL 注释明示的长块读指定路径，与 BMM150/MPU6050/VL53L0X 惯例一致），
  单次 I2C_RDWR 事务读全块不合并；文件头「实现要点」注释同步（旧注释
  描述的合并读符号回绕为收紧前行为）。新增 `test/test_bme280.lua` 离线
  桩测试 33 用例：桩侧 `i2c_read_multi` 镜像真 HAL 契约（n>6 显式 -1）
  守护本回归——旧代码在该桩下 build 复现恒 ERR；校准注入用 datasheet
  附录 8.3 示例系数组，补偿输出与测试内独立转录的官方浮点公式交叉验证
  （专抓端序/H4·H5 nibble 装配转录笔误），并覆盖 chip id 校验、NVM 拷贝
  超时、读写无应答、无效温度/湿度读数（0x80000/0x8000 哨兵）、测量超时、
  destroy 置睡眠与缺参安全返回等负路径

### 文档

- README：硬件变量矩阵表补 `PWM`，驱动计数 50 → 53，项目版本同步 v0.3.3
- README 新增「硬件 PWM」小节：引脚由设备树静态绑定（附 BCM2710 的引脚/func
  映射表与 `dtoverlay` 写法）、`channel` 不是引脚号、`export` 成功不等于有物理
  输出、权限与 gpiod 复用互斥等注意事项

## [0.3.2] - 2026-09-10

引用列显式化：`$` 前缀替代隐式按值判定（行为变更，存量结构脚本需同步加 `$`）。

### 变更

- **引用列须以 `$` 前缀显式标记**：元数据列引用硬件变量须写 `$名字`
  （如 `src='$imu0'`），裸名一律按字面值——原实现把「值恰好等于已定义
  硬件变量名」的列隐式判为引用列，纯字面值与设备名撞名（如 note 列值
  恰为 `timer-0`）会被静默当引用、字段丢失；`$` 指向未定义硬件变量在
  build 期显式报错（信息含表/列/行/值）。`MetaProvider` 引用判定同步
  更新；test_monitor / test_filter / test_ema_chain / test_bh1750 /
  test_mpu_daemon 五个用例与 README、Monitor.lua 示例同步加 `$`；
  新增 `test/test_ref_prefix.sql|.mt` 回归用例（撞名字面值保持字面传递）

### 修复

- **INSERT 目标表名拼错由静默吞掉改解析期显式报错**：`readSQL` 的
  INSERT_TABLE 分支只处理表名匹配的 CREATE TABLE，无一匹配时整条
  INSERT 被无声跳过——硬件实例静默消失，错误延迟到动作期以误导性
  信息（`'timer-0' ... not defined`）暴露，排查方向完全错位。现无
  匹配表时解析期抛错（信息含拼错的表名并提示检查 CREATE TABLE）；
  新增 `test/test_insert_typo.sql` 负路径回归用例（期望退出码 1）
- **ADS1115 轮询预算覆盖最慢档**：OS 位轮询预算 100ms < dr=0（8SPS）
  的 125ms 转换周期，真机该档必误报 Conversion timeout；预算扩至
  200ms，`test_ads1115` 补 8SPS 用例回归守护
- **ADS1115/ADS1015 PGA 冗余编码读回换算错 8 倍**：call 无 pga 参数时
  回读芯片配置的 PGA 字段（3 位，0~7），字段值 6/7 与 5 同为 ±0.256V
  （datasheet 冗余编码），原实现按「越界」重置为 2（±2.048V），电压
  换算放大 8 倍；现 6/7 映射到 5，两驱动测试各补 PGA=6 用例
- **HAL `i2c_read_bytes` n<1 反向钳制**：`n < 1 or n > 16` 统一钳到 16，
  请求 0 字节反而读出 16 字节超长表——0.3.1「越界显式失败」修复批的同类
  漏网；现与 `i2c_read_multi_bytes` 对齐（n<1 钳 1、n>16 钳 16），
  `test_hal_ffi` 补 4 个 `i2c_read_bytes` 用例（此前该函数零覆盖）
- **`test/run_all.sh` 空匹配假绿与死循环挂死**：过滤器拼错时输出
  「0 通过, 0 失败」且退出码 0（CI 假绿）——现无匹配用例显式报错退出 1；
  新增单测超时（`TEST_TIMEOUT` 秒，默认 60，经 coreutils `timeout`，
  缺失时回退不限时），防单个用例死循环挂死整个入口
- **构建可复现性：`cjpm.lock` 入库**：原 `.gitignore` 忽略锁文件、
  `_set.sh` 每次构建都 `cjpm update` 主动拉新，二者叠加导致依赖版本
  漂移、历史构建无法复现；现锁文件入库，`_set.sh` 仅在锁文件缺失时
  update（升级依赖：手动 `cjpm update` 后连同锁文件一起提交）
- **README 舵机表与 spider1.sql 对齐**：原 README 列 10 路 s1~s10
  「左下/右下水平垂直」命名，spider1.sql 实际 14 路 s1~s14 + spk，
  通道分配 rb/lb/lf/rf 髋/大腿/膝/云台，接线参照严重误导；现同步
  为完整 14 路通道表并标注「参照 robots/spider1.sql」
- **robots/spider.mt 过时残存删除**：旧版 @表格式结构脚本，含已消失
  的 @MPU6050_BIND / @Speaker 表，总线列裸名（无 0.3.2 强制的 `$`
  前缀），按当前解析器无法工作；README/CHANGELOG 均未引用
- **.gitignore 裸名模式锚定**：`bot`/`bot?`/`main`/`log`/`report`/`tmp`
  匹配任意深度同名文件/目录（如未来新增 `src/log/`）；现锚定为
  `/bot`/`/bot?`/`/main`/`/log`/`/report`/`/tmp`
- **结构脚本头注释更新**：spider1.sql 补充 `_run.sh` 用法，relay-test.mt
  补充完整命令行；消除过时的 `cat xxx.mt | ./bot` 引用
- **`_run.sh` cjc 缺失显式报错**：`CANGJIE_HOME` 未设且 `cjc` 不在
  PATH 时原实现静默拼出错误路径，最终报晦涩的动态库缺失；现显式
  报错提示设置环境变量或安装编译器。同时修复 ldp/rt 拼接空项等价
  于把当前目录纳入库搜索路径的安全隐患（LD_LIBRARY_PATH 开头空项）
- **`test/run_all.sh` 真机跳过改为文件内标记**：原硬编码文件名
  `test_jit_warmup.lua`，新增真机用例需改 runner；现改为文件内声明
  `-- requires: i2c`，runner 按标记判断（可移植到 SPI/UART 等）
- **test_relay_nohw.lua 迁移至共享测试框架**：消除本地重复的
  check/passed/failed/summary 样板，统一使用 test/lib/tester.lua
- **ADS1115/ADS1015 call 补 i2c_set_addr**：call() 读写寄存器前未调用
  `i2c_set_addr`，靠 build() 遗留的 `HAL.i2c_addr[fd]` 兜底——当前
  「一变量一 lua_State」下可用，但违背 drv_common 声明的「build 与 call
  双份」契约，状态机一旦复用即串址读错器件；现补上与 build/destroy
  一致的 set_addr 调用
- **Kalman.new 校验 Q≥0、R>0**：Q=0 且 R=0 时 `kg=0/0` → NaN 永久
  传播（EMA 的 alpha 已有校验，契约不一）；现 Q<0 / R≤0 显式报错，
  `test_kalman` 补 4 个负路径用例
- **sensor_daemon tick 期错误限频日志**：tick 期总线错误（HAL 打印 +
  call 返回 ERR）完全无声，仅 count 停滞可察觉；现失败计数 + 首 3 次
  及每 100 次打印摘要，保留诊断信息
- **drv_common.lua 注释笔误修正**：`i2c_pair` 返回值注释
  `(addr, addr, nil)` → `(fd, addr, nil)`
- **共享槽布局核对失败改 fail-fast**：`pthread.lua` 的 `assert_layout`
  名不符实——布局漂移只 `print` 并返回 false，而两个调用方
  （sensor_daemon / daemon_probe）均忽略返回值，继续在错误偏移上读写
  共享槽，一旦「仓颉 SLOT_*_OFF vs Lua cdef」ABI 漂移即跨语言内存踩踏，
  且告警淹没在守护输出里。现失败直接 `error()`（守护 setup 的 pcall
  兜住并显式报 chunk setup error），守护拒绝在损坏布局上启动
- **ADS1115 destroy 语义反转**：原实现写 `config=0x0000`「清零停止转换」，
  但 MODE 位=0 是【连续转换模式】、COMP_QUE=00 为比较器使能（每次转换后
  拉 ALERT）——器件反而进入持续采样 + ALERT 翻转态（功耗、杂散中断）。
  现改 ADS1015 同款读改写：仅置 MODE=1（单次态不启动转换即断电）并清掉
  读回的 OS 位（防写回误启动一次多余转换），保留 PGA/DR/MUX 配置。
  新增 `test/test_ads1115.lua` 离线桩测试（此前该驱动零离线覆盖）：
  16 位符号扩展、差分 MUX、OS 轮询时序、超时与 destroy 读改写回归守护
- **重名前置判重（资源泄漏防线）**：`MtVars_Lua.Factory`（LuaJIT 状态机）
  与 `MtVars_Daemon.Factory`（共享槽 + mutex）都在实参求值时先申请 OS
  资源、`addHardwareVar` 后置查重，重名抛错后资源无人持有、无法回收
  （当前错误路径进程退出由 OS 兜底；长驻进程场景——如未来 TCP 模式——
  每次重试都累积泄漏）。`createLuaVars` / `createDaemonVars` 现于调
  Factory 前置判重（报错文案与 addHardwareVar 一致），资源只在名字
  确认可用后才创建；`addHardwareVar` 保留为权威兜底
- **RobotStruct 两阶段构造**：`init` 只做 SQL 纯解析（不申请 OS 资源，
  解析期抛错零清理负担），`createHardVars`（资源创建阶段）挪入
  `active()`，与 main 现有的 `finally deactivate` 天然配对——激活期
  抛错（重名 / controller 未定义等）时已创建硬件变量按 hwSeq 声明序
  倒序回收，为未来 TCP 长驻模式的「失败清理重试」铺路；main 报错
  前缀由「Action aborted」改为中性的「Aborted」
- **总线引用列 `$` 迁移补漏（真机回归发现）**：0.3.2 五个 test SQL
  同步加 `$` 时，漏掉了「设备表引用总线变量」的同类场景——PCA9685/
  ADS1115/ADXL345 等 28 个结构脚本的 `I2C`/`SPI`/`UART` 列仍写裸名
  （如 `'i2c-1'`），显式化后按字面值处理，总线 `call()` 返回的 `fd`
  不再合并进设备元数据，真机 build 报
  「Invalid Argument: fd or address」（纯 Lua 离线测试桩注入 fd、
  不经元数据管线，故未拦截此回归）。现全部补 `$`
  （robots/spider1.sql 与 test/ 下 28 个 .sql 共 31 处），
  README 结构脚本示例与说明同步
- **`_run.sh` 堆上限 64MB→512MB（真机回归发现）**：`cjHeapSize=64MB`
  小堆下 controller 转发（嵌套 play）路径高频 GC 触发悬空引用段错误
  （walk-fl.mt 稳定崩于 22~30 行，core 现场为 managed 对象字段读取
  野指针；判别实验：64MB 堆下转发链 5~6/6 崩、无转发同体积分配 0/4、
  改名模块/纯 dummy 链同样崩——与模块内容无关，纯属转发路径；默认堆
  与 512MB 下 14+ 次全稳）。疑似仓颉 1.1.3 运行时 GC/代码生成层问题，
  建议上游跟进；本仓先以 512MB 规避（树莓派 5 内存充裕），保留
  `cjHeapSize` 环境变量覆盖（强制 64MB 可复现崩溃）

## [0.3.1] - 2026-09-10

0.3.0 审计复审后的低危修复批（HAL 读接口静默失败显式化、UART 错误契约落地、
脚本工程加固），无新增功能。

### 修复

- **HAL 合并值读函数 n 越界改显式失败**：`i2c_read` / `i2c_read_multi` /
  `i2c_read_reg16` / `spi_read_multi` 的 n 非法（nil / <1 / 超上限）由
  「静默钳到 1 字节」改为返回 -1——静默钳制读出的 1 字节会经调用方的大端
  合并/拆字节数学放大成看似合法的错误数据（-1 本就是失败契约，MHZ19、
  SGP30、SHT3X 等全部调用方均已判读）；更长块读仍走字节表版
  `i2c_read_multi_bytes` / `i2c_read_bytes`（其钳制保留并已在注释注明）。
  配套 `test_hal_ffi` 断言更新并补 `i2c_read` n>8/n<1、`spi_read_multi` n>8 用例
- **HAL `uart_read` 落实错误契约**：read 失败时按 errno 分流——EAGAIN/EINTR
  继续轮询，其余错误（如 EIO）立即返回 nil（文档本就如此承诺，原实现
  把一切失败当「无数据」空转到超时，错误被吞）；新增 EIO 错误路径用例，
  `HAL.EAGAIN`/`HAL.EINTR` 常量化
- Servo 驱动逐次 `print` 收敛到 `BOT4CJ_DEBUG` 门控（与 Monitor 一致；
  大型动作脚本此前每帧每舵机刷一行；Relay 打印保留——README 承诺的
  relay-test 成功标识）

### 变更

- `_run.sh`：
  - 编译产物缺失时友好提示并退出 1（原 bash 报 No such file 或 confusing）
  - `src/` 源码新于 `target/release/bin/main` 时打印过期告警——
    「改完源码忘 build，旧二进制行为与文档不符」的常见坑前置暴露
  - 运行时 .so 路径收集由 GNU `find -printf` 改 POSIX 可移植写法

### 文档

- README「安全说明」明确定位：Lua 沙箱是**防误用护栏而非安全边界**
  （Lua 驱动可经 `require("ffi")` 直调任意 libc；.sql/.lua/.mt 均为可信
  输入），并注明未来接入 TCP 等外部输入时需另行建立信任边界

## [0.3.0] - 2026-09-04 ~ 2026-09-10（未打 tag）

围绕「守护采样 → 滤波 → 引用消费」数据闭环的大版本，同时落地 HV 子包重构、
硬件错误通道与一批新驱动（含 !33 feat/issue15-ux 合并）。

### 新增

- **@Daemon 守护线程机制**：
  - Cangjie `spawn` 绿色线程 + 每守护独立 lua_State（`jit.off`），规避 LuaJIT VM 非线程安全
  - 共享内存槽 IPC：守护侧 `publish` / 主线程 `play` 加锁读同一 `daemon_slot_t`（`lib/pthread.lua` 定义布局），防撕裂
  - 万能采样 chunk `lib/sensor_daemon.lua`：服务任意 build/call/destroy 型驱动，零 per-sensor 代码（fields 用 `|` 分隔，sign 做补码扩展）
  - 让出式节拍（拍间 Cangjie sleep 释放 schd-worker），实测 8 守护 + Timer 并发运行、干净退出
- **滤波器硬件变量**：Kalman（一维卡尔曼）与 Ema（指数移动平均），支持 守护→滤波→消费
  三级引用链（如 `imu0 → kf0 → mon0`）；`Monitor.lua` 示范消费/决策/转发闭环
- **新驱动**：SHT3X（SHT30/31/35 兼容）、INA3221（三通道高边电流/电压）、INA219（单通道
  高边电流/功率）、INA226（单通道高边电流/功率监测计，0.1% 精度，有真 ID 寄存器可甄别
  芯片）、MAX17043（单节锂电电量计）、TPL0501（SPI 256 抽头数字电位器，首个输出型
  器件）、LTC2944（多节电池库仑计，3.6~60V）、GPIO（HAL 扩展 gpiod uAPI，跨 SoC 通用）、
  MLX90393（Melexis 三轴磁力计/罗盘，命令式协议，build 自检链路 复位→Memory Recall→
  写 GAIN 回读校验→试测一帧）、AS3935（ams Franklin 闪电传感器，首个**事件型**驱动——
  call 返回最近锁存事件/雷暴距离/闪电能量，协议经 SparkFun 与 ESPHome 参考实现交叉核对）、
  VL53L4CD（ST ToF 激光测距，1mm~1.2m/18°，自 ST ULD 逐行移植 91 字节配置流与
  SetRangeTiming 预算编码，返回距离/状态/信号质量字段）、MS5611（TE 高分辨率
  气压计/高度计，0.012mbar，PROM CRC-4 自检 + 一阶/二阶温度补偿，含 double
  2^53 精度线上的 D1xSENS 拆位精确算法与压高公式海拔输出）、AS7262（ams 六通道
  可见光光谱 450~645nm，间接寄存器访问 TX/RX_VALID 握手时序同 SparkFun 实现，
  连续全通道模式 + 六通道原始计数 + 片上温度）、VL53L0X（ST 第一代 ToF 激光
  测距，30mm~2m/25°，Pololu/ESPHome/ST ULD 三方寄存器序列交叉核对：页式
getSpadInfo + SPAD 使能图原位重排 + 参考校准两轮 + 背靠背连续模式，
   标准/长距双 tuning 挡，结果块 12 字节与状态查表自 ST API 移植）
- **新驱动（补录）**：BMI270（六轴 IMU，配置 blob 双版本自 Bosch SensorAPI
  提取）、LSM6DSV16X（六轴 IMU，无 blob，位域/码表自 ST 官方头文件核实）、
  BME688（四合一环境：T/P/H 补偿 + 双变体气体路，Bosch API 交叉验证）、
  BMM150（三轴地磁，补偿管线黄金向量交叉验证 Bosch 原函数）、
  MT6826S（15 位磁编码器，SPI 模式 3 全双工帧 + CRC-8 校验 + 零位偏移）、
  LPS22HB（高分辨率气压计，单次模式 + BDU 防撕裂 + ST 家族自动增量位地址）、
  ADS1015（12 位高速 ADC，ADS1115 孪生，3300SPS + 左对齐解码）、
  MCP3421（18 位差分 ADC，配置回显自检 + RDY 轮询 + 四档分辨率）、
  APDS9900（环境光/近距，命令字节协议 + 内核同款 lux 算法）、
  BH1745（颜色传感器）、ADXL345（三轴加速度计，FULL_RES 恒定灵敏度，
  第四颗同构加速度计）、MAX30102（心率/血氧 PPG，Part ID 自检 + FIFO
  批量排空 + SAT 饱和标志）、AMG8833（8x8 红外热像阵列 Grid-EYE，
  聚合标量输出绕框架桥限制）、EMC2101（智能风扇控制器，温度/PWM/转速
  反馈 + 片上 LUT 双模式）、MAX17048（ModelGauge 换代电量计，VERSION
  寄存器甄别 17043/44/48/49 家族，支持 17049 双节变体）、CHT8305
  （申矽凌微温湿度，指针寄存器协议 + 双 ID 自检）、MCP4725（12 位 I2C
  DAC，EEPROM 断电保持 + 读回，首个模拟输出家族）、AD9833（DDS 波形
  发生器）、PCF8574（8 位准双向 IO 扩展器，无寄存器图单字节读写）、
  SDS011（激光 PM2.5/PM10 传感器，首个被动查询式 UART 器件）
- **UART 总线能力**（HAL termios 段 + UART 矩阵 + MH-Z19B CO2 参考驱动）：
  新增串行器件族；MH-Z19B 为首个 UART 参考实现，SDS011 同款节拍
- **HAL 新增 16 位寄存器指针读写** `i2c_write_reg16`/`i2c_read_reg16`：ST VL53Lx 族
  寄存器地址 16 位，「写 2 字节指针 + 读数据」合并为单次 I2C_RDWR 原子事务
  （避免拆两段事务重新引入指针竞态）
- **HAL 新增 `i2c_read_bytes`**：纯读逐字节返回（>6 字节的命令式应答超出 i2c_read
  大端合并的 2^53 double 精度线，如 MLX90393 的 9 字节 XYZT+status）
- **HAL 新增 `i2c_read_multi_bytes`**：字节表读（BH1745 颜色 8B / BMI270 六轴 12B
  等超 Lua number 2^53 精度的多字节单次读，配合 i2c_write_reg16 边沿位指针协议）
- **HAL 新增 `spi_write_multi`**：任意长帧 SPI 写（AD9833 DDS 波形发生器倒序字节帧）
- SGP30 驱动升级：build 自检（Measure_test）+ 基线持久化（免 12h 重学习）+
  绝对湿度补偿（引用 SHT3X 守护快照）
- **硬件错误通道（halt-on-error）**：`ensureOk` 统一 `{result}` 契约检查，任一硬件调用失败
  即中断剩余动作并以非零码退出
- **Lua 驱动沙箱**：加载后中和 `os.execute`/`io.popen`/`os.exit` 与任意文件读写（#5 清单）
- 环境变量 `BOT4CJ_DEBUG`：call/build/destroy 参数表/结果表转储开关，缺省静默
- Lua 离线测试统一入口 `test/run_all.sh`；test/ 套件全量入库（.sql/.mt 一一配对，
  真机用例自动跳过；含 ADS1115 单端/差分、多守护等）（#15）

### 变更

- **@Daemon chunk 按 `BOT4CJ_MODULE_PATH` 寻址**：与普通 Lua 硬件模块同一基准（绝对路径 / `modules/` 前缀原样，其余相对路径拼模块目录），编译产物可拷到任意位置运行（此前 chunk 按字面路径相对 CWD）
- 硬件变量类迁出为 `src/HV/` 子包（Alias/Daemon/Lua/MatrixVars），`r.cj` 只留
  RobotStruct 编排层（#15）
- HAL i2c 改用 `I2C_RDWR` 原子事务：读寄存器的「写指针 + 读」合并为单次内核传输，
  消除同器件并发访问的寄存器指针竞态
- DSL 语义收紧：`-` 整列缺省不再注入字段（可表达"用默认值"）；Lua 返回值保留
  number/bool/nil 类型不再字符串化；引用读快照去副作用（读不推进滤波器）
- 硬件变量生命周期改按声明序遍历
- 元数据收集改静态列缓存（P8/M2）：`MetaProvider` 一次冻结字面列、引用列每次
  实时快照，剔除每 call 两遍 O(cols·rows) 全表扫描；转发不再二次 read 引用
- 驱动样板收敛：`lib/drv_common.lua`（err/arg_table/i2c_pair）统一 **52 个**驱动的
  类型校验与 fd 构造（本版补收 PCA9685/Timer/MPU6050/Dummy，净删 ~100 行重复样板）；
  PCA9685 时基魔法数提为具名常量；删除 SPI/I2C 中从未使用的
  `SPI_SUCCESS`/`SPI_FAILED`/`I2C_SUCCESS`/`I2C_FAILED` 全局
- **Cangjie 内核瘦身**：
  - `Lua.cj` 的 `MtVars_Lua.Factory()` 巨型内联闭包拆为静态实现方法
    (callImpl/readImpl/riseImpl/freeImpl)，构造收紧为 private；Lua 状态机 init
    时即创建、rise/free 管理生命周期
  - `r.cj` `createHardVars()` 重构为分发器 + `createDaemonVars()`/`createLuaVars()`
  - `r.cj` Action 列校验收敛为单一 `parseActionColumn()`（run_drl_action 与
    parse_action_matrixObject 共用），消除重复的"<device>.<param>"拆分断言
  - `getEnv` 迁移至 `getVariable`（std.env），清理 main/Daemon/Lua 未用 import；
    过期/误导性 TODO/FIXME/INFO 注释收敛为未来规划条目，NN/DRL 注释块
    明确标注"保留待 nn4cj 接入"
- **共享测试库** `test/lib/tester.lua`：check/expect_ok/expect_err/expect_cmds/
  summary 一套断言原语（幂等 dofile），test_ema/test_bh1750/test_hal_ffi 三个
  代表用例已接入（其余渐进迁移）
- 依赖升级 luajit4cj 1.0.1（新增 Lua 类型常量与 Lua_tonumber/toboolean/tointeger/
  pushboolean 取值/压栈封装）：bot4cj 不再本地 foreign libluajit 符号，Lua C API
  一律经绑定包调用；compile-option 同步去掉 `-l luajit-5.1`（链接经 luajit4cj
  动态库传递，见其 README 约定）
- 接入 pthread4cj 1.0.2：守护槽互斥（stop 置位/采样读取）改经其
  `_mutex_init/destroy/lock/unlock` @C 封装，不再本地 foreign libc pthread 符号

### 修复

- `sensor_daemon` 字段提取对浮点静默向下取整：未配 sign 时原样透传（INA226 的
  V/I/P、MLX90393 的 µT 等浮点字段此前经守护发布会丢掉小数；sign=16 整型路径不变）
- 守护线程三连：stop 标志改经 mutex 临界区读写（消除数据竞争）；join 加超时
  （防 UAF 与 deactive 永久阻塞）；修复每守护 5 个 C 字符串泄漏
- `sandbox_lua` 栈索引错误（`setfield(-1)` 索引 nil 致 PANIC，#9 M1）
- PCA9685 channel 越界校验 + 离线测试（#9 M2）
- 两处裸 Exception 静默不抛
- MPU6050 六轴符号扩展：补码负值还原为有符号原始值 + 离线测试
- `test_pca9685_channel`/`float` 离线测试：桩改在驱动加载后注入（驱动加载
  期间 `dofile` 链路被 `os.getenv` 沙箱影响）
- 参数名误命中（Low）：@Action 参数名替换收窄为仅显式 @Alias 变量，普通硬件
  变量与参数重名时保留字面参数（如 `s1.t0` 不被误读为 Timer）
- INA219/INA3221 Lua 5.3 的 `<<` 位左移改为 `bitcompat.lshift`（LuaJIT 5.1 不识别，
  两个驱动此前加载即报 `unexpected symbol near '<'`，无法使用）
- 槽布局跨语言自检（Low）：`pthread.lua` ffi.offsetof/sizeof 逐项核对
  `daemon_slot_t` 与 `Daemon.cj` 的 SLOT_*_OFF，守护启动即校验、漂移即红
- 注入地址精度契约（Low）：`lua_pushinteger` 整数 tag 注入对任意 64 位值逐位
  保真；`slot_attach` 增前置守卫（损坏地址启动即红），≥2^53 仅告警不拦截；
  配套 `test/test_slot_addr.lua` 16 项断言
- 审计加固两轮（09-08 ~ 09-10，清单见 `.tmp/` 审计报告）：驱动返回值判读补齐
  （EMC2101/MS5611/ADS1256 的 set_addr/spi_w）、生产 print 噪音收敛到
  BOT4CJ_DEBUG 门控、`_run.sh` 退出码透传与 aarch64 运行时库路径、
  Daemon/Lua malloc 空指针检查、halt-on-error 契约补强（pcall 失败显式 ERR）、
  拼写与命名规范清理
- **ADS1x15 OS 位读语义反转**（datasheet 级）：轮询改等 1=转换完成——原实现等 0
  在启动转换后首轮即满足，所有采样滞后一拍读到陈旧值（首拍为复位默认值 0）；
  ADS1015 回显比对掩掉 OS 位（空闲恒读 1，原全 16 位比对在真机上必 mismatch、
  build 永远失败）、destroy 读改写清 OS 位防误启一次多余转换；
  `test_ads1015` 器件模型改按真实 OS 语义建模以守护修复
- **ADS1256 满量程换算少乘 2**：满量程为 ±2·VREF/PGA，原电压读数系统性低一半
- pthread `publish` 入锁前校验 vals 全为数值：防持锁区间 ffi 赋值抛错跳过
  unlock，主线程侧 `play()` 永久死锁
- MPU6050 六轴改 0x3B 起 14 字节 burst 单次原子读（消除 hi/lo 跨事务撕裂窗口
  与 12 次读失败值被静默合成伪读数），失败按 ERR 契约上报
- SPI setup 失败关闭泄漏 fd 并复位状态；I2C/SPI 的 `call` 校验 build 成败
  （未 build 不下发无效 fd）、返回契约补 `result="OK"`——三总线与 UART 对齐
- ADS1015/1115/1256 差分参数 `tonumber` 判 nil 按契约返回 ERR（占位符 `-`
  原触发裸 Lua error 被吞成通用错误）
- `sensor_daemon`：setup 各失败点关闭已开总线 fd（原长期泄漏）、补 spi_setup
  判读（错误码 -3）与 `P.assert_layout()` 槽布局断言、fail 增诊断日志
- `main.cj` 结构脚本解析异常路径经 finally 关闭文件流（原泄漏 fd）

## [0.2.0] - 2026-09-04

HAL 彻底甩掉宿主注册的 cfunc，进入 LuaJIT ffi 时代（**此后仅支持 LuaJIT**）。

### 新增

- LuaJIT 绑定抽取为独立包 [luajit4cj](https://gitcode.com/qingss0/luajit4cj)
- fft、kalman 由 C 移植为 Lua 库（`modules/lib/`），并清理全部 C/C++ 死代码

### 变更

- HAL 以 ffi 直调 glibc（open/ioctl/read/write/usleep），摆脱 shell-out 与 cfunc，
  原生性能零 fork；Lua 5.3+ 位运算经 `bitcompat.lua` 双兼容（LuaJIT bit 库 / 5.4 算术回退）
- SPI 传输延时改为逐次显式传入（`delay_us` 尾参，配置写在 @SPI 矩阵 delay 列）
- `i2c_set_addr` 失败诊断增强，支持 EBUSY 回退 FORCE

### 修复

- LuaJIT ffi 变参 ABI 陷阱：ioctl 地址参数显式 cast

## [0.1.1] - 2026-09-03 ~ 2026-09-04

### 新增

- 通用 **controller 引用机制**：硬件变量可在矩阵里把执行转发给其它硬件变量
  （`relay-test` 即 Relay→Timer 的无硬件演示链路）
- 动作脚本支持**内联 @Alias 定义**
- BME280 温湿度气压传感器驱动（Lua 模块）

### 变更

- 基层 I2C/SPI 驱动完全解耦为纯 Lua HAL（issue #14），驱动模块顶部 `dofile` 加载，
  函数名与原 cfunc 一致
- Servo 硬件变量迁移为 Lua 驱动
- 元数据中无法继承的 `-` 占位符归一为 `0`

## [0.0.6] - 2026-09-01 ~ 2026-09-02

### 新增

- **机器人结构脚本迁移到 SQL**：新版 `.sql` 结构文件由 SQL4cj 解析（支持 `--` 与
  块注释），旧版 `.mt` 结构矩阵废弃；动作脚本仍为 `.mt` 管道输入
- 新驱动：Timer（定时/延时）、SGP30（空气质量）、Dummy（Lua 硬件变量生命周期验证）

### 变更

- 动作与结构解耦；@Action 按标题行列序执行（修复列序错乱）
- 解析器独立为模块（Parser）

## [0.0.5] - 2026-08-31

### 新增

- Lua 桥新增 **burst 读写**函数，支持 16/24 位寄存器设备
- 重写 ADS1115 / ADS1256 驱动
- 新增深度审计报告（2026-08-30）

### 变更

- 全面改进错误处理与可配置性（fix/comprehensive-improvements）

### 修复

- 解析器（P.cj）逻辑错误与调试代码残留
- 硬件变量重复键检查（Issue #8）

## [0.0.4] - 2026-08-31

### 变更

- 更新外部依赖

## [0.0.3] - 2026-05-31（tag v0.0.3beta；6~8 月在其上持续演进）

项目最大转折点：**嵌入 Lua 状态机，硬件变量改由 Lua 脚本实现**——驱动开发从改仓颉
源码变为写独立 Lua 模块。

### 新增

- Lua 状态机嵌入与基于 Lua 的硬件变量（MtVars_Lua）：一次初始化、多次调用
- 6 月：ADS1256 / ADS1115 ADC 驱动（Lua）；SPI 应用层驱动；线程模块迁移至外部依赖
  pthread4cj；SPI 驱动沉淀为独立仓库 spi4cj
- 7 月：Lua 模块重构为单独仓库（luajit4cj 前身）
- 8 月：I2C 驱动重建；MPU6050 迁移调试跑通；删除遗留构建脚本
  （Makefile/compile.ps1/run.ps1）与杂项 test 目录（08-27）

## [v0.0.2beta] - 2026-05-03（tag；5 月上中旬持续）

硬件变量体系成型期。

### 新增

- 占位符支持与流式读取（脚本经管道输入）
- Servo 舵机硬件变量；RobotStruct 类封装机器人编排；硬件接口支持 JSON 输入
- PCA9685 重构并升级为硬件变量；矩阵储存
- @Alias 别名与 @Action 动作解析；手写 RAII，硬件变量倒序清除
- 原生 I2C（I2C_Orig）；pthread 线程与 I2C 锁；MPU6050 硬件变量
- kalman 滤波器；Cangjie 版神经网络（矩阵/张量/卷积/Adam 优化器）
- 动作集扩充：行走/转向/倾斜/举手/躺下等（`scripts/`）

### 修复

- 舵机逆向问题

### 移除

- 旧的神经网络与矩阵、声音（Speaker）部分及旧 I2C 接口（05-19 大瘦身，
  聚焦机器人控制主线）

## [v0.0.1beta] - 2026-04-21（tag；2026-01-28 项目起步）

探索期：从 C 直读传感器到强化学习驱动的机器人动作。

### 新增

- 项目创建：仓颉 + C 混合的 I2C / MPU6050 读取；Actor 动作脚本解析器与 `Actor.play()`
- PCA9685 舵机驱动初版（2 月）；蜘蛛机器人步态脚本调通（4 月）
- FFT 库；神经网络重构与储存/加载；CUDA 矩阵运算加速
- DRL 强化学习动作模式（奖励策略 + 陀螺仪输入）；CNN 卷积神经网络库（C 接口）
- Windows 平台支持（构建脚本/音频）；Speaker 扬声器
- 解析器独立为 Parser.cj；动作脚本命名行解析与动作类型枚举
