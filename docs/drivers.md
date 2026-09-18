# 硬件变量（驱动）清单

> **清单与计数以 `modules/` 目录为准**（本页是速查索引，可能滞后于代码；`ls modules/*.lua` 即权威清单）。结构脚本里每张 `CREATE TABLE <矩阵名>` 对应加载 `modules/<矩阵名>.lua`。各驱动的协议细节与其内嵌 `<矩阵名>_VIRT` 仿真模型见 [virtbus.md](virtbus.md)。

## 舵机 / 输出执行器

| 驱动 | 说明 |
| --- | --- |
| `PCA9685` | 16 路 PWM 舵机控制器（I2C），`Servo` 的 controller |
| `Servo` | 舵机抽象（角度 → 脉宽，max/min/offset 标定） |
| `SCSCL` | 飞特总线舵机（UART，FT-SCS 大端帧；位置/时间/速度/扭矩 + 反馈块读取 + 站号烧写） |
| `Timer` | 定时/延时（relay-test 链路的执行端） |
| `MCP4725` | 12 位 DAC（I2C） |
| `AD9833` | DDS 波形发生器（SPI，写-only） |
| `TPL0501` | 256 抽头数字电位器（SPI，写-only） |
| `PCF8574` | 8 位准双向 IO 扩展器（I2C） |
| `GPIO` | Linux GPIO |
| `PWM` | Linux PWM |
| `H03ARR` | H03A 电机驱动 |

## IMU / 姿态

| 驱动 | 说明 |
| --- | --- |
| `MPU6050` | 六轴 IMU（I2C） |
| `ADXL345` | 三轴加速度计（I2C） |
| `BMI270` | 六轴 IMU |
| `LSM6DSV` | 六轴 IMU |
| `MLX90393` | 三轴磁力计 |
| `BMM150` | 三轴磁力计 |

## 气压 / 环境 / 光

| 驱动 | 说明 |
| --- | --- |
| `MS5611` | 高精度气压计（I2C，PROM CRC-4） |
| `LPS22HB` | 气压/温度（I2C） |
| `BME280` | 温压湿三合一（I2C） |
| `BME688` | 温压湿 + 气体（I2C） |
| `SHT3X` | 温湿度（16 位命令字 + CRC-8） |
| `CHT8305` | 温湿度（SensyLink，双 ROM ID） |
| `AMG8833` | 8×8 红外热电堆阵列（Grid-EYE） |
| `AS7262` | 六通道可见光光谱 |
| `APDS9900` | 接近/环境光 |
| `BH1745` | 颜色传感器 |
| `BH1750` | 光照度（命令式单字节 opcode） |

## 测距 / 闪电

| 驱动 | 说明 |
| --- | --- |
| `VL53L0X` | ToF 激光测距（8 位指针 + 分页） |
| `VL53L4CD` | ToF 激光测距（16 位指针） |
| `AS3935` | 闪电检测 |

## ADC / 电源监测

| 驱动 | 说明 |
| --- | --- |
| `ADS1015` | 12 位 4 通道 ADC（I2C） |
| `ADS1115` | 16 位 4 通道 ADC（I2C） |
| `ADS1256` | 24 位 ΣΔ ADC（SPI 全双工） |
| `MCP3421` | 18 位 ΔΣ ADC（I2C） |
| `INA219` | 高边电流/功率监测（I2C） |
| `INA226` | 高精度电流/功率监测（真双 ROM ID） |
| `INA3221` | 三通道电流/电压监测 |
| `MAX17043` | 单节锂电电量计（ModelGauge） |
| `MAX17048` | 电量计（MAX17043 同族增强） |
| `LTC2944` | 多节电池电量计 |
| `EMC2101` | 风扇控制 + 温度 |

## 气体 / 医疗 / 其他传感器

| 驱动 | 说明 |
| --- | --- |
| `SGP30` | 空气质量 CO2eq/TVOC（I2C） |
| `SDS011` | 激光颗粒物（UART） |
| `MHZ19` | CO2 红外（UART） |
| `MAX30102` | 心率/血氧 PPG（环形 FIFO） |
| `MT6826S` | 磁编码器（SPI 全双工） |

## 框架 / 总线 / 工具

| 模块 | 说明 |
| --- | --- |
| `I2C` / `SPI` / `UART` | 总线变量，器件行以 `$` 引用挂载 |
| `Relay` | 参数转换转发（无硬件自检链路的一端） |
| `dummy` | 桩替身，打印参数即返回 |
| `Kalman` | 一维卡尔曼滤波（虚拟传感器，配 `Q`/`R`） |
| `Ema` | 指数移动平均（配 `alpha`∈(0,1]） |
| `Monitor` | 消费 `$` 引用快照的观察/决策变量 |
| `bitcompat` | Lua 5.1/5.3 位运算兼容库（非驱动） |

> 不依赖 HAL 的模块（如滤波器、virtbus 底座）可用 `luajit`/`lua5.4` 独立调试。

## 驱动契约

新增驱动照此实现，评审按此核对。公共样板在 `modules/lib/drv_common.lua`。

### 入口函数集

| 函数 | 调用时机 | 约定 |
| --- | --- | --- |
| `build(data)` | 装配期（`active()`，按声明序） | 打开/配置资源并做在位自检；**必须有** |
| `call(data)` | 每拍派发（或 `$` 引用读取） | 一次完整动作/读数；**必须有** |
| `destroy(data)` | 销毁期（倒序，`deactivate()`） | 尽力释放，见下 |
| `read`（经 `call`） | 无副作用读 | `$` 引用快照走 `read()`，缺省即 `play({})` |

### 返回值契约（halt-on-error）

每次 `build`/`call`/`destroy` 都必须返回**表**：

- 成功：`return { result = "OK" }`（可附带数据字段 `v0`/`out`/`value`…）
- 失败：`return err("<文案>")`（`drv_common.err` 构造 `{result="ERR", message=…}`）

宿主侧由 `MatrixVars_Hardware::ensureOk` 统一检查：非表、空表、`result="ERR"`、
`result` 非 `"OK"`/`"ERR"` 一律抛错。**漏写 `return` 会被判为失败**，不会静默当成功。

### 参数校验顺序

```lua
local ok, ek = arg_table(data)      -- ① 必须是表；并完成 proto=virt 前置
if not ok then return ek end

local fd, addr, ek2 = i2c_pair(data) -- ② fd/address 存在性 + i2c_set_addr
if fd == nil then return ek2 end
```

- `i2c_pair` = `i2c_check` + `i2c_set_addr`；报错口径固定为
  `Invalid Argument: fd or address` / `… (set_addr failed)`
- 若驱动的**额外**校验须先于 `set_addr`（约定「非法配置值不碰总线」，如
  `MS5611` 的 OSR 档位、`PCF8574` 的地址族），用 `i2c_check`（仅存在性）
  自行把 `set_addr` 留到校验通过之后
- 器件地址硬件固定、`address` 列可省者（`AMG8833` 0x69、`MCP4725` 0x62）
  保持内联校验——`i2c_pair` 视 `address` 为必填，语义不等价

### drv_common 公共原语

| 原语 | 语义 | 越界/失败语义 |
| --- | --- | --- |
| `err(msg)` | 构造 `{result="ERR", message=msg}` | — |
| `arg_table(data)` | 入参必须是表；附带 `proto=virt` 前置 | 返 `(false, err表)` |
| `virt_prepare(data)` | 独立入口：加载 virtbus → 发现并挂载 `*_VIRT` 模型 → inject（`arg_table` 只是它的调用方） | 返 `nil` 或原因串 |
| `i2c_check(data)` | `fd`/`address` 存在性（不碰总线） | 返 `(nil, -1, err表)` |
| `i2c_pair(data)` | `i2c_check` + `i2c_set_addr` | 同上 |
| `int_of(v, def, lo, hi)` | 数值列 → 取整并**钳到** `[lo, hi]`；非法/`-` 归 `def` | 钳制（非报错） |
| `num_or(v, def)` | 数值列 → `tonumber`；缺省/非法回落 `def` | 回落（不钳制、不取整） |
| `read_block_bytes(fd, reg, n, chunk)` | 从 `reg` 起读 `n` 字节，返 1 基大端字节表；`chunk` 缺省 6 | 任一读失败返 `nil` |
| `guard_build(rollback, fn, data)` | build 全程 pcall + 失败时回滚句柄 | 抛错折成 ERR 表 |

越界语义是**三选一**，由驱动按器件语义显式选择，不要互相「统一」：

- **报 ERR**：参数非法即拒绝（如 `APDS9900` 的 atime 范围、`MS5611` 的 OSR 档位）
- **钳制**：就近吸附到合法边界（`int_of`；`ADS1015`/`ADS1115` 的
  pga/dr/channel 亦为钳制——已复核保留，改报 ERR 属行为变更，需单独评审）
- **回落缺省**：占位符/非法值取默认（`num_or`）

`chunk` 必须 ≤6：HAL 的合并读把整块拼成一个 `double`，超过 6 字节即超
2^53 精度（见 `lib/HAL.lua` 的 `i2c_read_multi` 注释）。

> 未收 `poll_reg`（寄存器轮询）：唯一候选是 `VL53L0X` 的 `wait_reg`，
> `VL53L4CD` 的 `wait_ready` 轮询的是函数而非寄存器——单点抽象不值得。

### destroy 契约：资源不可用即静默成功

```lua
local fd, addr = tonumber(data.fd), tonumber(data.address)
if fd ~= nil and addr ~= nil and i2c_set_addr(fd, addr) then
    ...与器件状态有关的收尾动作...
end
return { result = "OK" }
```

收尾不是校验：器件可能因 build 失败或总线先行释放而拿不到 `fd`，此时报 ERR
会把正常收尾变成失败（`--tcp` 会话下即退出码非零）。
例外：`ADS1115` 的 destroy 有显式 ERR 契约回归用例（`test_ads1115.lua`），
属被测试固定的既有契约，不改范。

### 位运算与 bitcompat

- 位运算一律走 `modules/bitcompat.lua`（LuaJIT 5.1 无位运算符，
  且它提供 `s8`/`s12`/`s16`/`s16le`/`u16le` 符号扩展原语单点）：
  `local bitc = dofile(mpath.resolve("bitcompat.lua"))`
- 符号扩展**不要**再写 `if v >= 0x8000 then v = v - 0x10000 end`
  （除下述豁免）；调用点直接 `bitc.s16(v)`

### 内嵌虚拟模型（`<矩阵名>_VIRT`）

同文件内定义，`new()` 返回实例，可选 `BUS = "uart"|"spi"` 标记总线类型。
模型走 `lib/virtbus.lua`，与驱动共享同一套寄存器语义。

## 收敛豁免清单（刻意保留的重复实现）

这些重复是**设计**而非疏漏：两套独立实现互为对照，收敛会同时抹掉双方——
改动它们的算法/结构前请先确认互证价值。

| 豁免对象 | 位置 | 理由 |
| --- | --- | --- |
| 模型侧符号扩展与位运算 | 各驱动 `*_VIRT` 声明之后 | 模型刻意不共用驱动局部量（部分刻意「纯算术无 bitcompat」），与驱动实现互证寄存器语义 |
| CRC 计算双实现 | 驱动侧 vs 模型侧（如 `SHT3X` CRC-8、`MS5611` CRC-4、`SGP30` CRC-8） | 同上：一处算错时另一处会暴露，收敛后单点错误即失效 |
| 24 位符号扩展（`0x800000`/`0x1000000`） | `LPS22HB`、`ADS1256` 等 | `bitcompat` 无 `s24` 原语，保持内联 |
| 器件固定缺省地址的内联校验 | `AMG8833`、`MCP4725` | `address` 列可省，`i2c_pair` 语义不等价（见上） |
| `ADS1115` destroy 的显式 ERR | `modules/ADS1115.lua` | 有回归用例固定的既有契约 |
