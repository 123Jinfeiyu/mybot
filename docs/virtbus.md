# 虚拟总线（virtbus）技术参考

> 对应 issue #18。本文是无真机验证驱动链路的完整参考手册；[README](../README.md) 中只保留摘要。

## 概述

`lib/virtbus.lua` 提供纯内存的虚拟总线：`inject()` 后替换 HAL 的全部全局 `i2c_*`/`spi_*` 函数，真实驱动（如 BH1750）无需真机与 `/dev/i2c-*`、`/dev/spidev*` 即可完整跑通 build/call/destroy。I2C 器件行为由 `attach(name, addr, dev)` 挂载的模型回调决定（`write(bytes)` 收命令/寄存器写、`read(n)` 返回 n 字节），每次事务记入 `vbus.txlog` 供断言；未挂载的地址按 NACK 处理（写 -1、读 -1/nil），模型自身也可 `write` 返回 `false`/`read` 返回 `nil` 注入 NACK（模拟掉线），负路径与真机"设备不在总线"行为一致。指针式读族（`read_reg/multi/reg16`）严格复刻 HAL 的 I2C_RDWR「写指针 + 读」事务，模型能知道当前读的是哪个寄存器；`read/read_bytes` 为无指针纯读。

virtbus.lua 本身是**纯 Lua、无 ffi 依赖**（lua5.4/luajit 都能离线跑总线契约）；只有经 `load_driver` 加载真实驱动时才需要 LuaJIT（HAL.lua 依赖 ffi）。

## 快速上手

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

标准流程五步：`vbus.reset()`（用例隔离）→ `attach(addr, 模型)` → `inject()` 或 `load_driver(驱动路径)`（挂真实驱动时用后者，自动重注入）→ 驱动 build/call/destroy + 断言 `txlog`/返回值 → `restore()` 或 `reset()`。

## API

| API | 说明 |
| --- | --- |
| `vbus.attach(name, addr, dev)` | 在 7 位 I2C 地址挂模型；`name` 仅日志用。保存模型引用并**动态委托**——attach 后替换 `dev.write/read` 或改模型字段（如 `dev.nack=true`）即时生效，无需重挂；同一模型可挂多个地址（主/备地址测试） |
| `vbus.attach_spi(name, path, dev)` | 按 spidev 路径（`/dev/spidev<bus>.<cs>`，路径即片选）挂 SPI 模型；动态委托 `dev.xfer`，同路径多 fd 命中同一模型 |
| `vbus.inject()` | 替换全部 20 个全局 `i2c_*`（12）/`spi_*`（8）为虚拟实现；幂等（已注入直接返回）；I2C/SPI 在同一注入态下并存 |
| `vbus.restore()` | 还原注入前捕获的原始全局函数（真实 HAL 函数） |
| `vbus.reset()` | `restore()` + 清空器件表、txlog、fd 线索（含 SPI）；每个用例开头调用保证隔离 |
| `vbus.load_driver(path)` | `dofile` 真实驱动后强制重注入（驱动首次 dofile HAL 会用真实 ffi 函数覆盖全局 `i2c_*/spi_*`）；并保证 `restore()` 能还原到真实 HAL 而非置空 |
| `vbus.txlog` | 事务序列数组（见下），驱动行为断言的唯一凭据；I2C/SPI 共用一条时间线 |
| `vbus.devs` / `vbus.spi_devs` | 当前 I2C 地址 / SPI 路径 → 挂载项的表 |

`i2c_open` 走与 HAL 相同的路径白名单（仅匹配 `/dev/i2c-%d+`，非法路径返回 -1），虚拟 fd 从 3 起分配；`i2c_set_addr` 后该 fd 的事务才路由到对应模型，未 set_addr 或地址无模型一律按 NACK 处理。

## 模型回调契约

`attach` 的 dev 表，函数为点定义：

- `write(bytes)`：收到 1 基字节表（写族为 `{reg, val...}`，指针式读前还会收到单字节/双字节的寄存器指针）。**显式 `return false` = NACK**；无返回值/nil = 成功。
- `read(n)`：返回 **1 基字节表**（长度可少于 n，按实际组装）；**返回 nil = NACK**；不提供 `read` 时缺省返回 n 个 0。n 已按 HAL 契约钳制（字节表族 1~16）。

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

## txlog 条目格式

按时间序追加；指针式读产生相邻的 `w` + `r` 两条：

| dir | 含义 | addr | bytes |
| --- | --- | --- | --- |
| `"open"` | 打开虚拟总线 | nil | `{ raw = 路径 }` |
| `"set"` | `i2c_set_addr` 绑定地址 | 地址 | `{}` |
| `"w"` | 写事务（含读操作的寄存器指针写） | 地址 | 1 基字节表，如 `{0x3B}`、`{0x6B, 0x00}` |
| `"r"` | 读事务 | 地址 | `{ n = 请求字节数 }`（实际数据由模型 read 返回，不入日志） |

## SPI 契约

`attach_spi(name, path, dev)`，路径即片选身份：

- 挂载身份是 spidev 路径（`^/dev/spidev%d+%.%d+$` 白名单同 HAL），虚拟 fd 与 I2C 共用编号空间；同一路径的多个 fd 命中同一模型，`spi_close` 只关自己的 fd。
- SPI 物理层**全双工、无 ACK**：模型只有一个回调 `xfer(tx)`——收本帧 MOSI 1 基表、返回等长 MISO 表（短补 0/长截断；返回 nil/false = 传输失败）。一次 HAL 传输（`spi_write/write3/write_multi/read/read_multi`）= 一个 CS 帧 = txlog 一条 `x` 记录；**纯写也会移入 MISO 字节**（真机如此，调用方丢弃但 txlog 照记），模型可借此实现「发命令的同帧回状态」协议（如 ADS1256 的命令态/DATA 轮询）；`spi_read*` 的 MOSI 为全 0（等价真机 tx_buf=NULL）。
- `spi_setup` 复刻 HAL 校验（mode 0~3、bits 7~16、speed 正整数）并留痕；它面向**控制器**，真机上总线无器件也成功——虚拟层同此语义（与 I2C「无地址 NACK」刻意不同）。传输族的字节/帧长校验同 HAL（非字节不发帧直接 -1；`write_multi` 1~64 字节；`read_multi` n 1~8 越界显式 -1），`delay_us` 钳到 65535 随帧留痕。无挂载路径/未打开 fd 的传输一律 -1（虚拟层约定的「设备不在」负路径）。
- SPI txlog 条目：`{dir="sopen", path}`、`{dir="setup", path, mode, bits, speed}`、`{dir="x", path, tx={...}, rx={...}, delay=us, ok=bool}`（失败帧 `rx=nil, ok=false` 同样记录）。
- 现状边界：`spi_*` 全局族可注入（覆盖 TPL0501/AD9833/ADS1256 用法）；MT6826S 驱动绕过全局直调 `HAL.sys.ioctl_p` 打全双工帧，本层拦截不到，迁移它需先在 HAL 增加全局全双工传输原语。

## 契约对齐

虚拟总线逐函数复刻 HAL 返回值——`i2c_write_reg` 为**布尔**（驱动用 `if not` 判定，若错返 -1 会因 Lua 真值规则造成 NACK 假成功）、`write_multi/reg16` 为 0/-1、读族长度约束（合并值 1~6/1~8 越界显式失败，字节表钳 1~16）一致。

## 器件模型库 `lib/virtbus/`

协议层模型，只复现寄存器/字节序/burst 等事实，测量值由用例显式注入，避免模型与驱动按同一错误理解互证而假绿：

- `virtbus/regmap.lua`：通用「寄存器指针 + burst」模型，覆盖 MPU6050/BME280 一类大多数 I2C 传感器（8 位指针；构造传 `{ptr16=true}` 支持 VL53Lx 类 16 位指针）；提供 `set/set_word/get_sword` 预置寄存器、`nack`（读写全断）与 `nack_read/nack_write`（定向故障）、`write_log` 事务留痕、`reset()`。
- `virtbus/mpu6050.lua`：MPU6050 六轴 IMU 模型（regmap 之上固化寄存器事实）——WHO_AM_I 身份（可 `who(0x71)` 模拟错挂）、`set_accel/set_gyro/set_temp_raw` 注入有符号 LSB、`sample()` 一次填满 14 字节 burst、`set_accel_g/set_gyro_dps/set_temp_c` 按 datasheet 灵敏度以物理量造样本（供守护/E2E）、`virt_tick(n)` 造确定性合成波形（供 proto=virt 守护逐拍驱动）、`config_writes()` 取 build 配置序列。
- `virtbus/bme280.lua`：BME280 温压湿环境传感器模型（regmap 包装层）——0xD0 身份（`who(0x58)` 模拟错挂 BMP280）、datasheet 示例校准 NVM 字节布局（0x88 起 T/P 小端系数、0xA1 H1、0xE1 起 7 字节 H 块含 H4/H5 的 s12 nibble 拼装）、0xF7 起 P/T 20bit + H 16bit 数据区、合成 0xF3 STATUS 与 forced 转换状态机（`advance(ms)` 外部虚拟时钟驱动 6ms 转换/`nvm_stuck`/`meas_stuck` 卡死注入；`virt_tick` 走预武装路径无需时钟）、`set_raw(t,p,h)` 注入无符号 ADC 码（0x80000/0x8000 无效读数可测）。
- `virtbus/bh1750.lua`：BH1750 命令式器件模型（opcode 序列、大端计数读回、上电/模式状态解释、`set_count()`、`nack`）。
- `virtbus/ads1256.lua`：**首个 SPI 输入器件模型**，ADS1256 24bit ΣΔ ADC 的全双工命令状态机（`xfer` 点定义）——RESET/WAKEUP/SYNC/SELFCAL/RDATAC/SDATAC 命令、WREG/RREG 寄存器访问（RREG 读窗口优先于命令识别，dummy 字节 0x00 不被误判 WAKEUP）、RDATAC 模式 24bit 大端样本帧（驱动丢弃第 4 字节）；固化 MUX 单端 `ch<<4|0x08`/差分 `pos<<4|neg`、ADCON PGA 1~64、DRATE 30k~5SPS 事实；`set_raw`（补码 floor 量化 + clamp）、`set_voltage(v,pga_idx)` 反解、`virt_tick` 32 拍确定性正弦（1.25±1.0V）、`fail_when/fail_data` 帧级故障注入（RESET 不清故障钩子，防负路径假绿）。模块表 `BUS="spi"` 供守护自动分流；除 `xfer` 外公开 API 一律冒号定义。
- `virtbus/ad9833.lua`：**首个写-only SPI 输出器件模型**，AD9833 DDS 的 16 位字解码状态机——器件无 SDO，模型只解 MOSI：每 2 字节 CS 帧一个大端字，顶 2 位寻址控制/FREQ0/FREQ1/PHASE；B28 两段加载按 per-register 低半字锁存拼 28 位调谐字（低 14 位在前，顺序反了立刻读到错误频率）、B28=0/HLB 单半字写；反向读取 `freq_hz/phase_deg/wave/muted` 与 FSELECT/PSELECT 活动寄存器；帧级 `fail_when`。写-only 器件无 `virt_tick`、不做守护采样 E2E。
- `virtbus/tpl0501.lua`：**最小写-only SPI 输出器件模型**，TPL0501 256 抽头数字电位器——协议即一个 SPI 字节（CS 帧移入 8 位、CS 上升沿锁存，无命令字/地址/SDO）；按移位寄存器事实处理多字节帧（只锁存最后 8 位），抽头易失上电默认中点 0x7F（build 零发字）；`wiper/percent/ohms(rtotal)` 反向读取（R=level/255·rtotal，构造可指定 100k/10k 型号）、帧级 `fail_when`。
- `virtbus/vl53l0x.lua`：**首个 8 位指针 + 0xFF 分页 I2C 器件模型**（不能用 regmap ptr16——16 位指针的是 VL53L4CD）：`regs[page][reg]` 双键分页存储，页选寄存器 0xFF 写即切页、同号不同页互不覆盖。复刻 DataInit 舞步（page1 0x91 stop_variable 读/回写）、page6/7 SPAD 握手（0x83 RMW 保留、page7 就绪轮询、0x92 数量/光圈编码、0xB0 六字节使能图）、SYSRANGE 模式机（0x41/0x01 参考校准、0x02 连续）与 0x13 data-ready/0x0B 清中断时序；`advance(ms)` 虚拟时钟驱动 5ms 转换倒计时，`inject_result` 按 datasheet 定点布局写 0x14~0x1F 十二字节结果块（SPAD 8.8/率 9.7/距离大端）；故障钩子含全断 nack、定向 `fail_read(reg,page)`/`fail_write(pred)`、`set_spad_stuck/set_refcal_stuck/set_never_ready`；`virt_tick` 造 32 拍确定性正弦距离（100~1100mm）。
- `virtbus/mcp4725.lua`：**首个 I2C 命令位嵌入式输出器件模型**，12 位 DAC——无寄存器指针，命令类型在写流首字节高 3 位（C2..C0）、读为无指针纯读（区别于 BH1750 单字节 opcode 与寄存器指针器件）：解码快速写（C=000，PD 在 bits5:4 + 12 位码，只更新易失 DAC）与写 DAC+EEPROM（C=011，命令帧 PD 在 bits2:1，双码独立存储），EEP 写进入 25ms 写周期、读回 bit7 RDY 忙/闲，`advance(ms)` 驱动；5 字节读回拼 RDY/PD 与 DAC/EEPROM 两个跨字节 12 位码（命令帧与读回帧 PD 位布局不同是硅片事实）；短帧 NACK、未建模命令 ACK 留痕 unknown；`fail_read(按读长度谓词)` 可只断 1 字节 RDY 轮询、`fail_write(按帧谓词)`、`set_eep_stuck`。输出器件无 virt_tick、阶段 C 豁免。
- `virtbus/vl53l4cd.lua`：**首个 16 位寄存器指针（regmap ptr16）真实器件模型**（对照 VL53L0X 的 8 位指针+分页）：ptr16 底座上包装（链接原 write/read 覆盖同表）加状态机——软复位 0x0000 保持/释放（释放原地 reseed 回上电种子，不能换 regs 表：包装表与底座共享引用）；91 字节配置流是 0x002D 起地址自增的一笔流写；data-ready 极性链 0x0030 bit4→有效极性、0x0031 bit0 动态合成（翻转自动跟随）；0x0087=0x40/0x21 启动预武装、0x0086=0x01 清中断进 5ms 倒计时（`advance(ms)`），停止态不武装；结果块六寄存器固定地址（状态/SPAD/信号/环境/sigma/距离）字节宽度注入；`fail_read(16位地址,长度)`/`fail_write(帧谓词)`/`set_never_ready/set_boot_stuck`；三个 nack 由包装层自持自判（浅拷贝后 d.nack 不被底座 old_write 看见）；`virt_tick` 32 拍正弦 100~1100mm。
- `virtbus/sht3x.lua`：**首个 16 位命令字 + 每数据字 CRC-8 的命令式 I2C 器件模型**（SHT30/31/35，对照 BH1750 单字节 opcode）：无寄存器指针，2 字节命令大端字借 i2c_write_reg 两字节原子写发送、状态/测量无指针纯读；模型按 poly 0x31/init 0xFF **真算 CRC-8**（向量 0xBEEF→0x92）；软复位/清状态/读状态三命令与状态寄存器四标志位（bit0 写校验、bit1 命令状态读后清、bit4 复位检测、bit12 加热器）；非法命令 NACK 置 bit1；单次轮询三档重复性转换门 16/7/5ms 由 `advance(ms)` 驱动，未完成纯读 NACK、结果读后失效；周期 15 启动字 + break/fetch（可重复读）；`fail_cmd/fail_read(按 pending/长度谓词)`、`corrupt_crc`（只坏 CRC 字节）、`set_meas_stuck`、`set_status_word`；virt_tick 温湿度双正弦 + 预武装。
- `virtbus/mcp3421.lua`：**首个「单字节配置写 + 数据流纯读、读回帧自带配置回显/RDY 就绪位」的命令式 I2C 器件模型**（MCP3421 18 位 ΔΣ ADC）：无寄存器指针，写恒 1 字节配置（i2c_write_multi：RDY/O/C/分辨率/PGA 位域），读恒无指针纯读，末字节是配置回显——bit7 RDY 即就绪握手（0=新数据）、低 6 位供 build 比对兼在位探测（回显即状态，区别于 BH1750 opcode 与 SHT3X 命令字+CRC）；四档转换门 5/17/67/267ms 由 `advance(ms)` 驱动，连续模式读走自动续转、oneshot 读完待机；18bit 4 字节右对齐补码、12/14bit 3 字节顶部重复符号位；字节流读长度由主设备决定（短读截取/长读补 0）；`fail_write/fail_read(按 n,fresh 谓词)`、`set_never_ready`（轮询超时路径）、`corrupt_echo`（只翻配置位不动 RDY）；set_raw 按位宽钳制、set_voltage 物理量注入；virt_tick ±1.5V 正弦 + 预武装。
- `virtbus/ms5611.lua`：**首个「单字节命令编码通道+OSR 档位」+「指针读 PROM/ADC」+ PROM 出厂校准 CRC-4 的命令式 I2C 器件模型**（TE MS5611-01BA03 气压计，不用 regmap 的纯闭包模型）：无寄存器地图——写恒为恰好 1 字节命令：0x1E 软复位、D1 压力 0x40+2i / D2 温度 0x50+2i 启动转换（两区间分别判定、仅偶数收，跨区间保留码 0x4A 与奇数码 NACK）；读必须先发指针——0xA0+2i 选 PROM 字读 2B 大端、0x00 选 ADC 读 3B 大端 24 位码，指针读后失效、无指针纯读 NACK；五档转换门按 datasheet T_MAX 0.60/1.17/2.28/4.54/9.04ms 由 `advance(ms)` 驱动（区别于驱动取整等待表 1/2/3/5/10ms），门限前/未启动 ADC 读 NACK、oneshot 结果保持到下条转换命令、PROM 转换中可读。8 字 PROM 含真实量级 C1~C6 与 word7 序列号，CRC-4 按 TE/PX4 布局**真算**（poly 0x3000、word7 band 0xFF00、CRC 位域参与自身运算，`crc4_ok` 公开；AN520 word0 布局不适用本芯），nibble 试探填出自洽种子；补偿解算（一阶/二阶/极低温追加项、d1*sens~2^56 拆位、压高公式）全归驱动，模型另给 `set_pt` 暖机一阶逆模型供守护造数（闭环误差仅 24 位量化粒度）；`fail_cmd/fail_read(target,n,idx 谓词)`、`corrupt_prom`（只翻 word7 CRC 位域、系数不动，专打 build CRC 兼在位甄别）；24 位码 clamp；virt_tick 气压 1013.25±5hPa/温度 22±2°C 正弦 + 预武装。

使用示例：

```lua
local mpmod = dofile("modules/lib/virtbus/mpu6050.lua")
local imu = mpmod.new()                      -- WHO_AM_I 默认 0x68
imu:sample({ ax = 1, ay = 2, az = 3,         -- 有符号原始 LSB
             gx = -4, gy = -3, gz = -2, temp = 340 })
vbus.attach("MPU6050@0x68", 0x68, imu)
```

## 守护无硬件端到端（`proto=virt`）

`@Daemon` 的 `proto` 列填 `virt`，sensor_daemon 不碰真机，而是挂 `lib/virtbus/<driver 小写>.lua` 器件模型——真实驱动 build/call、共享槽 publish、`@Ema/@Kalman`、`@Monitor` 引用消费整条链路全部走内存仿真。挂载族按模型模块表 `BUS` 标记自动分流：I2C 模型（默认）走 `attach(addr)` + `i2c_open`（必须配 `addr`）；**SPI 模型（`BUS="spi"`）走 `attach_spi(name,path,dev)` + `spi_open/spi_setup`**（`path` 缺省 `/dev/spidev0.0`、免 addr，`mode/bits/speed` 从 Daemon 列读取，如 ADS1256 要 Mode1/8bit/1.92MHz，SPI 器件 params 不透传 address）。模型若提供 `virt_tick(n)`，守护每拍先驱动它造**确定性合成波形**再采样（无该方法则输出静态初值）；驱动无对应模型/缺必填总线身份时 setup 显式 fail(-8)，守护空转不崩。守护启动打印醒目 `VIRTUAL HARDWARE` 横幅（仿真数据禁止用于生产环境）。

现有八例：

- `mpu_virt`：七字段 IMU 守护 + 七路 EMA + 两 Monitor
- `bme280_virt`：三字段温压湿守护 + 三路 EMA + 两 Monitor，真实驱动跑附录 8.3 补偿、模型只给 NVM/位场
- `ads1256_virt`：首个 SPI E2E——raw+voltage:f 两字段守护，真实驱动跑 8 帧 build/6 帧 call 与 24bit 补码电压换算、模型只给命令状态机与合成正弦样本
- `vl53l0x_virt`：首个分页 I2C E2E——DIST_MM:f/STATUS/VALID/SIGNAL_MCPS:f 四字段守护，真实驱动跑完整 build（DataInit 舞步/SPAD 握手/约 90 条 tuning/两轮参考校准）与 12 字节结果块解析，模型给分页寄存器与 data-ready 状态机及合成距离正弦
- `vl53l4cd_virt`：**首个 16 位指针 ptr16 I2C E2E**——DIST_MM:f/STATUS/VALID/SIGNAL_KCPS:f 四字段守护，真实驱动跑 91 字节配置流/SetRangeTiming 预算编码/六结果寄存器独立原子读，模型给 ptr16 寄存器窗、动态合成的极性 data-ready 链与合成距离正弦
- `sht3x_virt`：**首个 16 位命令字+CRC-8 命令式 I2C E2E**——T:f/RH:f/CRC_OK 三字段守护，真实驱动跑软复位+状态 CRC 校验的 build 与「命令→16ms 等待→6 字节双 CRC 帧」的 call，模型真算 CRC-8、给转换门状态机与温湿度双正弦；interval=50ms 容纳真实转换睡眠
- `mcp3421_virt`：**首个配置回显/RDY 握手型命令式 I2C E2E**——12bit/×1/连续档 voltage:f 单字段守护（差分电压有负值），真实驱动跑「写配置→纯读回显比对」的 build 与「轮询 RDY 5ms 步进→补码解码→电压换算」的 call，模型给四档转换门/连续续转状态机与 ±1.5V 正弦、virt_tick 预武装使 call 零真实睡眠
- `ms5611_virt`：**首个通道+OSR 命令字/指针读 PROM+ADC/出厂 CRC-4 命令式 I2C E2E**——P_HPA:f/T_C:f/ALT_M:f 三字段守护（Daemon 额外列 osr/p0 自动透传），真实驱动跑「软复位睡 3ms→八字 PROM 指针读→CRC-4 校验兼在位甄别」的 build 与「D1/D2 双段转换 0x48/0x58→各 10ms 真实等待→0x00 指针读 3B→一阶/二阶补偿与压高公式」的 call，模型给五档 datasheet 转换门/指针读状态机/TE 布局真算 CRC-4 与 1013.25±5hPa、22±2°C 正弦、virt_tick 预武装；interval=50ms 容纳双段 20ms 真实转换睡眠，实测 16 拍气压 1008~1018hPa

```bash
cat scripts/test/mpu_virt.mt      | bash _run.sh --robot=robots/test/mpu_virt.sql
cat scripts/test/bme280_virt.mt   | bash _run.sh --robot=robots/test/bme280_virt.sql
cat scripts/test/ads1256_virt.mt  | bash _run.sh --robot=robots/test/ads1256_virt.sql
cat scripts/test/vl53l0x_virt.mt  | bash _run.sh --robot=robots/test/vl53l0x_virt.sql
cat scripts/test/vl53l4cd_virt.mt | bash _run.sh --robot=robots/test/vl53l4cd_virt.sql
cat scripts/test/sht3x_virt.mt    | bash _run.sh --robot=robots/test/sht3x_virt.sql
cat scripts/test/mcp3421_virt.mt  | bash _run.sh --robot=robots/test/mcp3421_virt.sql
cat scripts/test/ms5611_virt.mt   | bash _run.sh --robot=robots/test/ms5611_virt.sql
```

`run_all.sh` 在检测到编译产物 `target/release/bin/main` 与仓颉 runtime 时，按用例表（用例名｜`:f` 浮点字段断言｜count 下限）自动执行仓颉级 E2E：通用断言横幅、无错误、守护持续发布 count 增长、EMA 至少一拍 `out ≠ raw`（键序无关的逐行数值比对），另加每用例的浮点字段未被取整断言；环境缺失则逐例跳过，不阻断纯 Lua 回归。

## 回归测试

均由 `run_all.sh` 自动纳入：

- `test/test_virtbus.lua`：纯 Lua，43 断言——功能/指针事务/长度约束/两级 NACK/还原
- `test/test_virtbus_regmap.lua`：纯 Lua，37 断言——regmap + ptr16 + 定向故障 + 总线集成
- `test/test_virtbus_spi.lua`：74 断言——SPI 路径白名单/setup 校验/全双工 CS 帧/delay 钳制/字节与帧长边界/无挂载与模型故障负路径/动态委托/fd 生命周期/I2C+SPI 并存，LuaJIT 段另挂虚拟 SPI 跑真实 `modules/SPI.lua` chunk 的 build/call/destroy
- `test/test_virtbus_bh1750.lua`：LuaJIT——真实 BH1750 build/call/destroy + 动态改读数 + 掉线 + 无器件负路径
- `test/test_mpu6050.lua`：LuaJIT，50 断言——真实 MPU6050 全链路：build 配置序列/WHO_AM_I 甄别/0x69 备地址/14 字节 burst/六轴补码/温度换算/读写两个方向 NACK/virt_tick 仿真波形（原手写桩 test_mpu6050_sign 为其严格子集已合并删除）
- `test/test_bme280.lua`：LuaJIT，51 断言——真实 BME280 全链路：复位/校准块 6×4B+7B 长读/CTRL 回读复核/forced 状态机/20bit 数据位场/0x80000 与 0x8000 无效读数/超时/主备地址/附录 8.3 独立参考交叉验证（迁移时模型按字节宽度编码戳破旧桩 H1=260 的超宽假值，datasheet 真值 H1=75 对应 45.3%RH 档）
- `test/test_ads1256.lua`：LuaJIT，96 断言，**首个挂真实驱动的 SPI 全链路**——模型协议事实/24bit 样本帧与 clamp/WREG-RREG/virt_tick 周期波形、build 8 帧字节序列与 5+5+50ms 等待/Mode1 setup 留痕/自定义 drate-pga-channel、call 6 帧序列与转换等待/单端差分 MUX/±满量程补码/PGA 换算/第 4 字节丢弃、8 类 build + 5 类 call 帧级失败负路径/MUX 失败补发 RDATAC 恢复/8 拍连读（旧 mock ioctl_p cdata 层桩整体替换）
- `test/test_ad9833.lua`：LuaJIT，110 断言，**首个写-only SPI 器件**——16 位字顶 2 位寻址解码/B28 低-高半字拼接与反序保护/HLB 单半字/FREQ1+FSELECT/PHASE0-1+PSELECT/波形位/非整字帧、真实 build 五字序列与 440 方波 90° 解码、参数校验零发字、5+6 逐帧失败负路径与故障后重建、12.5MHz 满量程（旧手写字桩整体替换）
- `test/test_tpl0501.lua`：LuaJIT，95 断言，**最小写-only SPI 器件**——单字节 CS 帧锁存/上电中点 127/多字节帧移位寄存器语义/build 零发字/level-percent-ohms 三路换算与优先级/delay 透传钳制/故障帧与未打开 fd 双负路径（旧替换全局 spi_write 的桩整体替换）
- `test/test_vl53l0x.lua`：LuaJIT，116 断言，**首个分页 I2C 器件**——分页双键隔离/DataInit 舞步/page6-7 SPAD 握手/SYSRANGE 模式机/advance 虚拟时钟/定向故障钩子/定点结果块注入/virt_tick 九组模型事实，真实 build 全序列（VHV RMW/Q9.7 门限四档/SEQ_CONFIG/光圈与非光圈 SPAD 图重排/tuning 档位/stop_variable 回写）与九路 build 负路径，真实 call 五字段解析/ST 状态码六例/8190 越界/四路 call 负路径/destroy 停测（旧 410 行手写桩整体替换）
- `test/test_mcp4725.lua`：LuaJIT，108 断言，**首个 I2C 命令位嵌入式输出器件**——快速写/EEP 写命令位解码与 PD 位布局差异、DAC/EEPROM 双码独立、5 字节无指针读回、RDY 25ms 写周期 advance 时钟、短帧 NACK/未用命令留痕、按读长度与按帧谓词定向钩子，真实 build 纯读在位零写、快速写三参优先级与 vref 换算、EEP 写一次轮询/500ms 超时/轮询总线失败/写帧失败四路径（旧 188 行手写帧桩整体替换）
- `test/test_vl53l4cd.lua`：LuaJIT，108 断言，**首个 16 位指针 ptr16 器件**——ptr16 指针事务/上电种子/软复位原地 reseed/极性链动态合成/5ms 倒计时/六结果寄存器宽度注入/六类钩子/virt_tick，真实 build 软复位帧/91 字节配置流/启动序列/SetRangeTiming 四档预算编码常量/word 与 dword 帧长/预武装零轮询睡眠/七路负路径，真实 call 1234mm 五字段/六结果读按 16 位地址过滤轮询/连续两拍倒计时/ST 状态码与 SPAD=0 守卫/超时-轮询失败-结果读失败-清中断失败四路径/destroy 停测（旧 300 行手写 16 位寄存器桩整体替换）
- `test/test_sht3x.lua`：LuaJIT，114 断言，**首个 16 位命令字+CRC-8 命令式器件**——CRC-8 真算向量/状态四标志位与读后清/非法命令置 bit1/非 2 字节帧 NACK/三档重复性转换门/结果读后失效/长度不符 NACK/周期 fetch/六类钩子/virt_tick 双正弦，真实 build 命令序列与 3ms 睡眠/bit4 缺失仅告警/旧错位映射守卫（bit0 不致命、bit1 致命）/七路负路径，真实 call 16ms 等待与 6 字节双 CRC 帧/双量程端点/坏 CRC 降级不中断/连续两拍/六路负路径/destroy 零事务（旧 206 行手写全局桩整体替换）
- `test/test_mcp3421.lua`：LuaJIT，143 断言，**首个配置回显/RDY 握手型命令式器件**——单字节配置位域/非单字节帧 NACK/四档转换门边界/18bit 右对齐与 12/14bit 顶部重复符号位独立编码交叉验证/连续读走续转/oneshot 待机/字节流长短读/位宽钳制/电压注入/六类钩子/virt_tick 节拍，真实 build 四档配置字节 0x9C/0x8B/0x94/0x91 与「写→纯读回显」序列/八路负路径（含回显翻转与未打开 fd），真实 call 5ms 步进轮询睡眠节拍 270/70/20/5ms/×8 双满码/oneshot 触发写/wait 超时/五路负路径/virt_tick 零等待正负两拍，destroy 待机写 0x0C/0x0B（旧 335 行手写全局桩整体替换）
- `test/test_ms5611.lua`：LuaJIT，169 断言，**首个通道+OSR 命令字/指针读 PROM+ADC/出厂 CRC-4 命令式器件**——CRC-4 模型/测试独立算法/存储位域三方一致/八字 PROM 指针读与读后失效/读长度约束/转换命令两区间偶数编码（0x4A 保留码 NACK）/五档 datasheet 转换门 0.60~9.04ms 边界/转换中与未启动 ADC NACK/oneshot 结果保持/D1D2 码区分/24 位钳制/set_pt 四物理点逆算闭环/armed 连转/六类钩子（corrupt 只翻 word7 CRC 位域）/virt_tick 峰谷节拍，真实 build「0x1E→3ms→八字 0xA0+2i 指针写+2B 读」序列与 CRC mismatch/复位失败/PROM 读失败/指针写失败/fd=99 八路负路径，真实 call 双段 0x48,00,58,00+双 3B 读与 20ms 节拍、ffi int64 独立参考复算暖机/低温二阶/极低温追加项三分支逐位对照/OSR 降档 0x40/0x50 与 2ms/p0=1000 海拔/virt_tick 两拍/八路负路径，destroy 零事务（旧 312 行手写全局桩整体替换）
