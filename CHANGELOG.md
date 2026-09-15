# 更新日志

本文件记录 bot4cj 的所有重要变更，依据 git 提交历史整理（仅覆盖 main 主线）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与 `cjpm.toml` 的 `version` 字段对应。

## [未发布]

virtbus 虚拟硬件测试基座、2026-09 审计修复批次、ActionSource 工厂、NN 前向推理引擎。

### 新增

- **virtbus 虚拟硬件测试基座**（`modules/lib/virtbus/`，issue #18）：真实 HAL 下注入虚拟 I2C/SPI 总线与器件模型，无需真机即可用真实驱动跑通全链路。
  - `regmap.lua` 通用寄存器模型（8/16 位指针 + burst 自增回绕）；`inject()` 替换全局 I2C/SPI 函数，支持 NACK/故障注入、txlog 留痕。
  - **42 个虚拟器件模型及测试迁移**（原手写桩整体重写）：I2C 侧 MPU6050、BME280、VL53L0X、MCP4725、VL53L4CD、SHT3X、MCP3421、MS5611、SGP30、INA219、ADS1115、AS7262、MAX30102、AMG8833、PCF8574、LTC2944、CHT8305、ADS1015、INA226、INA3221、MAX17043、MAX17048、ADXL345、LPS22HB、PCA9685、BMM150、BH1745、APDS9900、MLX90393、AS3935、EMC2101、BME688、BMI270、LSM6DSV；SPI 侧 ADS1256、AD9833、TPL0501、MT6826S；UART 侧 MHZ19、SDS011。迁移即抓出旧桩假绿（BME280 H1 溢出、MPU6050 NACK 误判等）。其中 PCA9685 为纯输出器件，只做模型 + 真实驱动单测（旧 3 个手写桩合并为 1 个 338 断言链路测试），守护采样 E2E 豁免；BMM150 为首个「POR 上电挂起态电源门 + 三段工厂 trim NVM + XY/Z 异构 13/15 位数据块」模型（240 断言，旧 247 行手写桩整体重写）；MAX17048 为 MAX17043 同族换代模型（八字稀疏地图、VERSION 真判值家族甄别、CRATE 第四路、STATUS 横幅读，275 断言，旧 220 行手写桩整体重写）；BH1745NUC 为首个双 ROM ID + VALID 只读动态合成 + 小端四通道 RGBC 数据块的颜色传感器模型（六档积分门、SW/INT 双命令位自清，186 断言，旧手写大端桩整体重写）；APDS-9900 为首个命令字节 COMMAND+TYPE 四态寻址模型（TAOS TSL2771 架构环境光+近距传感器，小端三字对、全软件 lux 两线 IR 补偿，202 断言，驱动零改动）；MLX90393 为首个「命令字 + status-first 应答 + 易失内存 RR/WR 读写」命令式模型（Melexis Triaxis 三轴磁力计，非法命令恒 ACK+ERROR 锁存、SM 单次转换门与 DRDY 低读锁错、Melexis 区写保护、µT/°C 全软件换算，211 断言，迁移抓出驱动七处协议错误整体重写）；AS3935 为首个事件型器件模型（Franklin 闪电传感器，INT 事件锁存读后清、0x3C 软复位/0x3D RCO 校准两条直接命令、21 位能量三字节小端只读测量区、PWD 停摆与 MASK_DIST 干扰屏蔽硬件门，virt_tick 8 拍离散事件脚本替代连续正弦，202 断言，旧 267 行手写桩整体重写、驱动零改动；守护侧 21 位 ENERGY 字段须逐字段 `:f` 透传，跟全局 sign=16 会被截成 16 位）；EMC2101 为首个「PWM 输出 + 测量反馈混合」模型（Microchip/SMSC 智能风扇控制器，手动占空比 FAN_SETTING 与 8 槽 LUT 片上温控自主两模式受 PROG 位硬件门——PROG=0 时写 ACK 丢弃；外温 11 位补码 1/8°C 双字节 interlock 读序 MSB 先、TACH 16 位小端 interlock 方向相反 LSB 先，模型影子锁存可观测撕裂防护；双 Part ID 0x16/0x28+MFG 0x5D；PWM_F POR 0x17 现算 full_lsb=46；二极管开路 0x3F8/短路 0x3FF 特征码；RPM=5,400,000/count，212 断言，旧 261 行手写桩整体重写并抓出驱动 TACH 读序反接）；BME688 为首个「3 块 42 字节 NVM + 17 字节防撕裂数据场双气体量程路 + forced TPHG 门限状态机」模型（Bosch 四合一温压湿气，0x8A×23/0xE1×14/0x00×5 三块校准 burst、0x1D 起 20 位 P/T+16 位 H+低/高两路 10 位气体，0xF0 变体选路；门限 = TPH OSR 时长 + RUN_GAS/heat_off 使能的 GAS_WAIT，virt_tick 预武装零睡眠；CHIP_ID/VARIANT 按 ROM 语义软复位不变以保住误挂甄别；32 拍离线码钉经独立 Bosch float 公式反解固化，154 断言，旧 364 行手写桩整体重写并抓出 par_p8 漏 s16）；BMI270 为首个「配置 blob 上传状态机 + INIT_DATA 地址陷阱」模型（Bosch 六轴 IMU，CHIP_ID=0x24；blob 三段握手 INIT_CTRL 清零→每 8 字节 INIT_ADDR(0x5B) 低 4 位/整字双字节寻址 + INIT_DATA(0x5E) 8 字节突发，后者 burst 全部字节落 blob 窗口、不普通自增不进 regs 影子→INIT_CTRL=1 启动 150ms 加载门，fifo 328B/full 8192B 合法长度、不符即 INTERNAL_STATUS=0x02；0x0C..0x17 六轴 12 字节小端 LSB 在前连续测量；blob 数据引用 modules/lib/bmi270_config.lua 不转录进模型，32 拍六轴离线码钉独立换算反解，171 断言，旧 271 行手写桩整体重写并抓出驱动 INIT_ADDR 写 NACK 误判，同批订正旧桩三处错误 POR/期望：ACC_CONF=0xA8/GYR_CONF=0xA9/PWR_CONF=0x03、build 后 ACC_CONF 应回读 0xA8 而非 0x38、旧桩未模拟 0x5E 地址陷阱）；LSM6DSV16X 为首个「CTRL3 软复位/IF_INC 突发门 + 双 6 字节小端连续窗、无 blob 无转换门」模型（ST 六轴 IMU，I2C 0x6A/0x6B，WHO_AM_I=0x70；CTRL3 bit0 sw_reset 写即自清并立即 reseed、bit7 BOOT 自清、bit2 IF_INC=0 时多字节读重复同址；fs_xl 在 CTRL8(0x17)[1:0]、fs_g 在 CTRL6(0x15)[3:0] 且 ±4000dps 码为不连续的 0xC；陀螺窗 0x22 在前、加计窗 0x28，各 6 字节小端常驻；STATUS(0x1E) XLDA/GDA 按 ODR 动态合成；set_reset_stuck 喂 10 拍轮询超时；32 拍六轴离线码钉，178 断言，旧 226 行手写桩整体重写并抓出驱动 REG_CTRL8 地址写错 0x18/应为 0x17 的真机 bug）；MT6826S 为首个「全双工 24 位帧 + CSN 四寄存器快照 + 无 ID 靠固定位在位甄别 + CRC-8 快照」模型（MagnTek 15 位磁编码器，SPI Mode3/1MHz；24 位帧 cmd4+addr12+data8，MOSI 带命令地址、MISO 同帧第 3 字节回数据前两字节 Hi-Z；0x003~0x006 角度 h/l<<1/STATUS/CRC-8(poly0x07) 一组四帧共享快照模拟 CSN 下降沿锁存；l bit0 与 st 高 5 位固定位兼在位甄别；持续坏 CRC 与 corrupt_next_groups 半开区间分组注入、fail_transfer/fail_when 传输钩子；32 拍线性爬坡 virt_tick，90 断言，旧 ioctl_p 手写 cdata 桩整体替换；同批在 HAL/virtbus 新增首个全双工全局原语 spi_xfer_full 并把驱动从本地 ffi 自组 spi_ioc_transfer 改为调全局，仓库内再无绕过全局直调 ioctl 的 SPI 驱动）；MHZ19 为首个「UART 字节流请求→应答环」模型（Winsen MH-Z19B NDIR CO₂，9600 8N1，无地址无 ACK；模型唯一回调 `on_write(bytes)` 三态——返回字节表=8 字节应答 `FF 86 HI LO TEMP S 00 00 CS` 逐字节压入输入环 / nil=不应答即驱动超时 / false=本笔写失败 `uart_write` 返 -1；virtbus 同批新增 `uart_*` 5 函数注入族共 26 全局，逐项镜像 HAL 校验——`uart_open` 先 baud 白名单（1200~921600 十一档）后路径白名单、非法写零留痕、校验通过即使无挂载 TX 恒成功、`ur` 条目记实际读回字节表使聚合读协议可逐轮断言、总线级 `uart_fail_read` 读故障返 nil 记 err；校验和模型/驱动/测试三方独立实现交叉；set_co2/set_temp_c/set_status 注入与 32 拍爬坡 virt_tick，113 断言，旧手写 uart_* 桩测试整体替换；sensor_daemon 同批加 `BUS="uart"` 器件派发——path 缺省 `/dev/serial0`、baud 为 Daemon 表框架列不进 params）；SDS011 为第二个「UART 字节流」模型（Nova Fitness 激光颗粒物，9600 8N1；19 字节请求 `AA B4 CMD D0×12 FF FF CS AB` → 10 字节应答 `AA C5 b3..b6 ID1 ID2 CS AB`（CS=Σb3..b8、设备 ID 计入），命令分流四态——0x02 切被动/0x04 查询（PM 小端 u16、模型只存协议原始 16 位量，/10 换算只在驱动）/0x06 工作睡眠/0x07 固件版本，ACK 回显命令字节供驱动逐字节校验；故障钩子六件含 stale_push（每条应答前垫 10 字节残留主动流帧——CS 故意填 0 使驱动扫描必落空、每命令多一轮 ur，真机开机主动流残留语义）与 swallow_next 定向吞一次应答（版本应答丢失 build 仍降级 OK）；非法请求先留痕再解析、5 拍扬尘 virt_tick 使 PM 恒带一位小数（E2E 浮点断言可直接锚守护行，与 MHZ19 整数 ppm 相反），164 断言，旧手写 uart_* 桩测试整体替换）。
  - **守护 `proto=virt` 无硬件端到端**：按驱动名挂模型，build/call → 共享槽 → 滤波 → Monitor 全链路纯内存跑通。全套 96 项 / 1 真机跳过 / E2E 三十七组。其中 BH1750 补齐最后一个缺口（`bh1750_virt`）——模型新增 `virt_tick(n)` 造 32 拍正弦照度（count 15002±3000 → H 档 LUX 10001.7~15001.7，只改测量值不产生命令写），对应单测 14 → 23 断言；命令式器件的 `:f` 浮点断言不锚守护 raw 某一拍（少数拍 count 恰为 6 的倍数成整数、且 20ms 守护拍与 50ms pump 混叠），改锚 Ema 混合输出 `out`。
- @Daemon 共享槽采样通道 16 → 32，支撑单守护聚合多路传感器。
- **MPU6050 补全片上温度输出**：call() 返回 `TEMP/TEMP_RAW`；`sensor_daemon` 字段新增 `:f`/`:s16` 逐字段模式后缀。
- **ActionSource 工厂 `src/ActionSource.cj`**（stdin / TCP / 文件三源可替换动作输入）：`main.cj` 新增 `--tcp` / `--input` 参数；`TcpStreamReader` 字节流适配字符流；含回环 E2E 与 7 用例。
- **NN 前向推理引擎 `modules/lib/nn.lua`**（纯 Lua，无 nn4cj 依赖）：全连接 MLP，默认 `{41,80,80,40,17}` 架构；`forward`/`forwardOne`/`zeroInit`/`randInit`/`saveMt`/`loadMt`；16 用例。
- 离线桩测试：SHT3X、LTC2944、MAX17043、TPL0501、INA219、INA3221；新增 `test_spi.lua`、`test_ads1256.lua`。

### 变更

- **PCA9685 虚拟器件模型内嵌驱动脚本（单文件驱动+模型实验）**：`modules/lib/virtbus/pca9685.lua` 整体迁入 `modules/PCA9685.lua` 尾部，经全局 `PCA9685_VIRT.new()` 工厂暴露。三条约定：① 生产路径零执行——模型仅在虚拟总线测试中实例化，regmap 底座延迟到 `new()` 内加载，真机 build/call/destroy 不触碰 `lib/virtbus/`；② 工厂经全局表而非文件 return 暴露——生产侧 `Lua.cj` build_lua 以 `LUA_MULTRET` pcall 加载驱动文件且成功路径无 settop，文件返回值会永久滞留 Lua 栈；③ 模型与驱动保持独立实现（模型自带寄存器表/位定义/换算常量），互证防假绿性质不变。`test_pca9685.lua` 接线改为从驱动加载后的全局取工厂（338 断言全绿，PCA_GC 压力档同绿）；模型库余 41 个文件，守护 `proto=virt` 按 `<driver 小写>.lua` 找模型的约定暂不感知内嵌形态（PCA9685 守护 E2E 豁免，无实际影响）。
- **`test_pca9685.lua` LuaJIT 下 `table.unpack` 崩溃**：补 `unpack or table.unpack` 兼容 shim（同 `sgp30.lua` 等 6 处既有做法，fix-plan-20260915 P0-1）；修复前 111 断言后崩溃退出码 1，修复后 338/338。
- **BH1750 虚拟器件模型内嵌驱动脚本（单文件第二个，打通守护 E2E 回退路径）**：`modules/lib/virtbus/bh1750.lua` 整体迁入 `modules/BH1750.lua` 尾部 `BH1750_VIRT` 全局工厂（纯闭包无外部依赖，三条约定同 PCA9685_VIRT）；`sensor_daemon.lua` virt 模式新增回退装载——`lib/virtbus/<driver 小写>.lua` 缺失时 dofile 驱动文件取内嵌 `<驱动名>_VIRT` 工厂、跳过二次 load_driver 仅重注入，模型文件在则维持原路径，两形态失败报错各自明示；模型库 41 → 40。`test_virtbus_bh1750.lua` 接线改从全局取工厂（23 断言全绿）；`bh1750_virt` 守护 E2E 经真实二进制 + 回退路径实跑通过，全量 97 通过 / 0 失败。至此带守护 E2E 的器件迁移条件全部就绪（TP-推广待定：剩余 35 组 E2E 器件 + 2 个 write-only 同档器件均按此模式可迁）。
- **MPU6050 虚拟器件模型内嵌驱动脚本（单文件第三个，regmap×守护回退组合首次验证）**：`modules/lib/virtbus/mpu6050.lua` 整体迁入 `modules/MPU6050.lua` 尾部 `MPU6050_VIRT` 全局工厂（REG 表/灵敏度常量随工厂暴露，regmap 底座延迟到 new() 内加载）；模型库 40 → 39。`test_mpu6050.lua` 接线改从全局取工厂（50 断言全绿，`mpmod.REG`/`mpmod.new()` 引用经本地绑定零改动沿用）；`mpu_virt` 守护 E2E 经真实二进制 + 回退路径实跑通过（publishes=79），全量 97 通过 / 0 失败。
- **MHZ19 虚拟器件模型内嵌驱动脚本（单文件第四个，UART 族样板）**：`modules/lib/virtbus/mhz19.lua` 整体迁入 `modules/MHZ19.lua` 尾部 `MHZ19_VIRT` 全局工厂（工厂表带 `BUS="uart"`，`sensor_daemon` BUS 分派据此走 attach_uart/tty 路径——回退装载的 UART 分支首次实跑验证）；模型库 39 → 38。`test_mhz19.lua` 接线改从全局取工厂（113 断言全绿）；`mhz19_virt` 守护 E2E 经真实二进制 + 回退路径实跑通过（publishes=50），全量 97 通过 / 0 失败。I2C/SPI/UART 三总线形态中 I2C 与 UART 的内嵌×回退组合均已验证，SPI 仅缺 E2E 实跑（ads1256/mt6826s 迁移时补齐）。
- **ADS1256 虚拟器件模型内嵌驱动脚本（单文件第五个，SPI 族样板，三总线全验证）**：`modules/lib/virtbus/ads1256.lua` 整体迁入 `modules/ADS1256.lua` 尾部 `ADS1256_VIRT` 全局工厂（工厂表带 `BUS="spi"`，`sensor_daemon` BUS 分派据此走 attach_spi/spidev 路径——回退装载的 SPI 分支首次实跑，三总线内嵌×回退组合全部验证）；模型库 38 → 37。模型常量/命令表/DRATE 映射移入 new() 内（避免与驱动文件级同名局部量遮蔽），bitcompat 经驱动 mpath 上值独立 dofile、失败退化纯算术（不共用驱动 bitc，保互证独立）。`test_ads1256.lua` 接线改从全局取工厂（96 断言全绿）；`ads1256_virt` 守护 E2E 经真实二进制 + 回退路径实跑通过（publishes=35），全量 97 通过 / 0 失败。
- **pre-push 门禁确认与文档化（P0-4）**：`.git/hooks/pre-push`（本机 2026-09-10 安装，钩子不入库）推送前跑全量 `test/run_all.sh`，非零拒推，`git push --no-verify` 可跳过；已实测「失败拒推」与「全绿放行」两路径。不采用 `core.hooksPath=scripts/git-hooks` 方案——会旁路本机已有的 post-checkout/post-commit 钩子；GitCode CI 流水线就绪后同一脚本可作唯一门禁上管线。
- **离线测试「零断言/早退假绿」缺口（P0-2）**：`run_all.sh` 原判定仅认「退出码非零或 [FAIL] 行」，零断言安静退出的文件会被记通过（探针实测：`if true then return end` 早退文件判「1 通过」）。三处收紧：`test/lib/tester.lua` 的 summary() 末尾打 `[DONE]` 完成标记（先于 os.exit）；`run_all.sh` 判定升级为三条——未见 `[DONE]` 或 `[PASS]` 计数为 0 一律计失败；9 个自带框架的用例（action_source/fft/h07_fd/jit_warmup/kalman/nn/pthread/slot_addr/uart）补完成标记，其中 test_action_source/test_nn 的静默计数 check 改为逐断言打印 `[PASS]`/`[FAIL]` 与全库对齐。修复后两种假绿探针均翻转为失败，全量回归无误报。

### 修复

- **cjpm.lock 恢复入库（P0-3）**：`aaffb0a`（2026-09-14）误删锁文件后 `_set.sh:8` 每次构建都触发 `cjpm update`，依赖版本漂移、历史构建不可复现（与 `.gitignore`/CHANGELOG 0.3.2 的入库声明矛盾）。按当前 cjpm.toml 重建，五依赖（SQL4cj 1.0.3 / ctype4cj 1.0.4 / mt2 1.0.0 / luajit4cj 1.0.1 / pthread4cj 1.0.2）与 0.3.2 锁定版本一致。
- **守护 play 返回值契约统一**（`src/HV/Daemon.cj` playImpl）：返回键 `status` → `result`，使 `ensureOk`（MatrixVars）对守护链路的 halt-on-error 校验真正生效——此前全仓 54 个驱动均返回 `result`，唯守护发 `status`，按「无 result 字段视为成功」条款守护失败会被静默放行（评估报告 P6 / fix-plan-20260915 P1-6）。改后 37 组守护 E2E 实跑全绿、无新报错。
- **5 个测试结构脚本 INSERT 缺列**：按各自 `CREATE TABLE` 补全 `'-'` 占位符，用例由解析失败变为正常走到硬件层。
- **`h03_arr.sql` 引用列漏写 `$` 前缀**：补为 `'$arr_src'` 后恢复正常。
- **守护 `interval ≤ 0` CPU 忙循环**：解析失败与 `≤ 0` 一律抛 `IllegalArgumentException`，报错带上下文。
- **`sgp30.lua` LuaJIT 下 `table.unpack` 崩溃**：补兼容 shim。
- **`pthread.lua` stop 偏移注释错误**（248 应为 328）。
- **长动作脚本 controller 转发路径概率性崩溃/挂死**：仓颉 1.1.3 并发 GC 缺陷，`_run.sh` 新增 `BOT4CJ_NO_GC=1` 规避；热路径 `logD` 改条件求值降低 GC 压力。
- **ActionSource 编译失败 8 错**：按 Cangjie std.io 实际接口修正，补导入与类型适配。
- **test_pwm 概率性段错误**：`ffi.copy` 越界 1 字节写坏 GC 对象头，改 `char[?](#s+1)`。
- **BH1750 MTreg 编码错误**：改 `0x60|(mt&0x1F)`；测试断言一并修正。
- **多路径模块查找对守护 chunk 与滤波驱动不生效**（issue #19 收尾）。
- **守护消费端过滤 CRC 失败样本**。
- **2026-09 审计修复批次**：严重四项（LTC2944 控制字写偏、BMM150 三轴多乘 0.3、INA219 SADC 档表错位、MHZ19 帧头扫描死代码）；中低项含 SHT3X 状态位映射、MPU6050 WHO_AM_I 甄别、call-before-build 守卫等。
- **MAX17048 build VALRT 阈值字节反接**（virtbus 迁移抓出）：datasheet Rev.7 规定地址 0x14=VALRT.MIN（16 位字高字节）、0x15=VALRT.MAX（低字节），旧驱动把 valrt_max 编码进高字节，致过压/欠压阈值互换；修正后 valrt_max=4.2V/valrt_min=3.3V 正确落字 0xA5D2（MIN=165、MAX=210）。
- **BH1745 颜色数据字节序反接**（virtbus 迁移抓出）：datasheet Rev.002 Command Set 逐行明确 0x50=RED_DATA_LSBs/0x51=RED_DATA_MSBs（G/B/Clear 同构，偶地址为 LSB 的小端布局），旧驱动误按大端 `b[1]*256+b[2]` 解码，四通道高低字节互换（旧手写桩以大端字节喂数互证假绿）；修正为 `R=b[2]*256+b[1]`（G/B/C 同理），与 ROHM/Arduino 例程一致。
- **EMC2101 TACH 读序反接**（virtbus 迁移抓出）：datasheet Rev.2.54 §6.1 Data Read Interlock 规定 TACH 必须【先读低字节 0x46 锁存高字节 0x47】再读 MSB（外温恰好相反，先读 MSB 0x01 锁存 LSB 0x10，两路方向相反；Adafruit Arduino/CircuitPython 两库 TACH 均明确 LSB first），旧驱动两路都按先 MSB 读、旧手写桩以同序断言互证假绿，修后读序 0x46→0x47、RPM 计数拼字正确。同批按 datasheet 订正旧桩两处错误 POR：FAN_CONFIG(0x4A)=0x20（PROG=1 手动/可编程态，旧桩误给 0x00）、PWM_F(0x4D)=0x17=23（full_lsb=46，旧桩误给 0x0F=15/full_lsb=30）。
- **BMI270 blob 地址相写失败未中止上传**（virtbus 迁移抓出）：HAL `i2c_write_multi` 契约为 0=成功/-1=NACK，而 `upload_blob` 对 INIT_ADDR(0x5B) 写用 `if not i2c_write_multi(...)` 判错——Lua 中 -1 为真，真机上地址相写 NACK 会被当成功继续发 0x5E 数据（数据在器件侧落到错误字地址，blob 加载结果不可预期）；改为 `~= 0` 显式判定，与随后 INIT_DATA 写的判定一致。
- **LSM6DSV16X 加计量程寄存器地址写错**（virtbus 迁移抓出）：ST 官方 lsm6dsv16x_reg.h 明确加计 fs_xl 在 CTRL8(0x17)[1:0]，0x18 是 CTRL9（usr_off/lpf2/hp 等使能位）；旧驱动 `REG_CTRL8 = 0x18`，build 的量程读改写全落到 CTRL9，真机加计永远 POR ±2g（横幅却照印配置量程），陀螺 CTRL6(0x15) 不受影响。改为 0x17 后 ±2/4/8/16g 档正确落盘，新模型以「build 后 0x17=fs 码而 0x18 恒 0」回归钉死。
- **BME688 `par_p8` 漏按 int16_t 解释**（virtbus 迁移抓出）：Bosch BME68x-Sensor-API v4.4.8 `get_calib_data` 实证 `par_p8` 为有符号 16 位，旧驱动 `parse_calib` 与旧手写桩都按无符号拼字，负 `p8`（种子 -4000）时气压二阶修正错出十几 hPa，桩以同错互证假绿；改为 `s16` 后与独立转录公式全量程闭合（模型 NVM 种子专含 `p8=-4000` 回归此点）。
- **MLX90393 驱动协议全面错位**（virtbus 迁移抓出，按 datasheet 3901090393 Rev012 重写）：应答帧漏读首字节 status 致全部偏移错位（status-first TXYZ 9B，OFF_T=2/OFF_X=4/OFF_Y=6/OFF_Z=8）；单次完成位误查为他位，实为 status bit5 SM_MODE；WR 写寄存器必须 4B 帧 `[0x60,Dhi,Dlo,A<<2]`（旧发 3B）；增益/霍尔配置在 reg0x00（旧写错址，POR=0x007C，RMW 只改 GAIN_SEL）；温度码按无符号 u16 解码 `25+(TR−46244)/45.2`（旧按有符号）；增益表 {5,4,3,2.5,2,5/3,4/3,1} 正比换算 `sens=0.751·tab/5`（旧表方向反）。配套模型修正三处：SB/SM zyxt=0 时缺失 reg0x01 的 nil 算术兜底、负码四舍五入改按幅度取整（二进制浮点致 −100 码误杀）、RT 不清测试桩预武装态（守护 build 首命令即 RT，撤了预武装会致试测转换门在真实 milli_sleep 下永不完成、setup fail -2）。
- `Lua.cj` 沙箱补中和 `io.input`/`io.output`；Lua 表转 JSON 混合键转对象；守护退出清理顺序修正。

### 重构

- **`logD` 惰性求值**：签名改为接收 `() -> String`，非 debug 模式下插值代码不再执行——从类型上根除"热路径日志插值产生 GC 压力"问题（0.3.3 曾修 7 处、仍漏 10 处的调用点差异随之消除）。
- **FFI C 字符串作用域辅助 `withCString`**（`Lua.cj`）：`mallocCString`/`free` 手工配对约 18 处收敛为 try/finally 统一释放，malloc 失败一律显式抛错。
- **`luaGlobalType` 探测辅助**：`readImpl`/`freeImpl` 重复的"getglobal + type + pop"探针合并。
- **`jsonMergeInto` 浅合并辅助**（`MatrixVars.cj`）：`callImpl` 元数据拷贝/参数合并/转发 payload 与 `MetaProvider.collect` 引用列合并共 5 处重复循环收敛。
- **`MtVars_Daemon.Factory` 拆分**：play/rise/free 三个内联大闭包提取为具名静态实现 `playImpl`/`riseImpl`/`freeImpl`（与 `MtVars_Lua` 既有同类重构对齐）；配置串与 interval 两遍列扫描合并为一遍；slot 释放两分支去重为 `releaseSlot`。
- `sandbox_lua` os/io 两段孪生中和逻辑合并为数据驱动循环；ActionSource 端口校验双报错路径合一。
- **命名改进**：`src/r.cj` → `src/RobotStruct.cj`（单字母文件名自描述化）；`RobotStruct` 单字母字段 `C`/`M` → `matrices`/`hardwareVars`（含 `MetaProvider` 同名字段）；`run_action`/`parse_action_matrixObject` → `runAction`/`parseActionCell`；局部 `MtName`/`AC`/`AliasDefs` 等统一 camelCase。各文件注释中对 r.cj 的引用同步更新。

### 文档

- `BME280.lua` / `SHT3X.lua` 头注释更新为现 HAL 语义，移除过时 cfunc 描述。

## [0.3.3] - 2026-09-11

新增硬件 PWM 驱动，修复 BME280 恒 ERR 与构建/配置一致性问题。

### 新增

- **硬件 PWM 驱动 `PWM`**（`modules/PWM.lua` + `HAL.lua` PWM 段）：走 Linux sysfs 接口，SoC 控制器产生波形。走 ffi 而非 io 库（沙箱兼容）；写序固定 duty→period→duty；`chip`/`channel` 限定非负整数；duty 未配置时 build 不使能输出。离线测试 2 层 mock + 示例。

### 修复

- **`pwm_exported` 存在性探测改 `O_RDONLY`**：非 root 用户 EACCES 误判未导出。
- **越界通道号前置校验 + `pwm_npwm`**：export 前读 `npwm` 校验合法范围。
- **sysfs 写失败带 errno 人话提示**。
- **`.gitignore` 移除 `cjpm.lock`**：与 0.3.2 锁文件入库矛盾。
- **`_run.sh` 堆大小注释与默认值不符**（128MB vs 注释 512MB）。
- **BME280 湿度校准块读取超 HAL 上限，build 恒 ERR**：改走 `i2c_read_multi_bytes`，新增 33 用例离线桩测试。

### 文档

- README 硬件变量矩阵表补 `PWM`，新增「硬件 PWM」小节（设备树绑定、权限等）。

## [0.3.2] - 2026-09-10

引用列显式化：`$` 前缀替代隐式按值判定（行为变更，存量结构脚本需同步加 `$`）。

### 变更

- **引用列须以 `$` 前缀显式标记**：裸名一律按字面值；`$` 指向未定义变量 build 期显式报错。五个 test 用例与 README 同步加 `$`，新增 `test_ref_prefix` 回归用例。

### 修复

- **INSERT 目标表名拼错改解析期显式报错**：原静默跳过致硬件实例消失。
- **ADS1115 轮询预算扩至 200ms**：覆盖最慢 8SPS 档（125ms 转换周期）。
- **ADS1115/ADS1015 PGA 冗余编码换算错 8 倍**：6/7 映射到 5。
- **HAL `i2c_read_bytes` n<1 反向钳制**：n<1 钳 1、n>16 钳 16。
- **`test/run_all.sh` 空匹配假绿与死循环**：无匹配报错退出 1；新增单测超时。
- **构建可复现性：`cjpm.lock` 入库**。
- **README 舵机表与 spider1.sql 对齐**（10 路→14 路）。
- **`.gitignore` 裸名模式锚定**；删除过时 `robots/spider.mt`。
- **`_run.sh` cjc 缺失显式报错** + LD_LIBRARY_PATH 空项安全隐患修复。
- **真机跳过改文件内标记** `-- requires: i2c`。
- **ADS1115/ADS1015 call 补 `i2c_set_addr`**。
- **Kalman.new 校验 Q≥0、R>0**。
- **sensor_daemon tick 期错误限频日志**。
- **共享槽布局核对失败改 fail-fast**（`error()` 而非 print）。
- **ADS1115 destroy 语义反转修复**：改读改写仅置 MODE=1，新增 16 用例离线桩测试。
- **重名前置判重**（资源泄漏防线）；**RobotStruct 两阶段构造**（init 纯解析 / active 资源创建）。
- **总线引用列 `$` 迁移补漏**：28 个结构脚本共 31 处补 `$`。
- **`_run.sh` 堆上限 64MB→512MB**（仓颉 GC 并发缺陷规避）。

## [0.3.1] - 2026-09-10

0.3.0 审计复审后的低危修复批，无新增功能。

### 修复

- **HAL 合并值读函数 n 越界改显式失败**（返回 -1 替代静默钳制）。
- **HAL `uart_read` 落实错误契约**：EAGAIN/EINTR 轮询，其余立即返 nil。
- Servo 驱动 print 收敛到 `BOT4CJ_DEBUG` 门控。

### 变更

- `_run.sh`：编译产物缺失友好提示、源码过期告警、.so 路径收集改 POSIX。

### 文档

- README「安全说明」：Lua 沙箱是防误用护栏而非安全边界。

## [0.3.0] - 2026-09-04 ~ 2026-09-10（未打 tag）

围绕「守护采样 → 滤波 → 引用消费」数据闭环的大版本，同时落地 HV 子包重构、硬件错误通道与一批新驱动。

### 新增

- **@Daemon 守护线程机制**：Cangjie spawn + 每守护独立 lua_State；共享内存槽 IPC；万能采样 chunk `sensor_daemon.lua`；让出式节拍。
- **滤波器硬件变量**：Kalman 与 Ema，支持三级引用链。
- **新驱动**：SHT3X、INA3221、INA219、INA226、MAX17043、TPL0501、LTC2944、GPIO、MLX90393、AS3935、VL53L4CD、MS5611、AS7262、VL53L0X、BMI270、LSM6DSV16X、BME688、BMM150、MT6826S、LPS22HB、ADS1015、MCP3421、APDS9900、BH1745、ADXL345、MAX30102、AMG8833、EMC2101、MAX17048、CHT8305、MCP4725、AD9833、PCF8574、SDS011。
- **UART 总线能力**（HAL termios + MH-Z19B CO2 参考驱动）。
- **HAL 新增**：`i2c_write_reg16`/`i2c_read_reg16`、`i2c_read_bytes`、`i2c_read_multi_bytes`、`spi_write_multi`。
- SGP30 驱动升级（build 自检 + 基线持久化 + 湿度补偿）。
- **硬件错误通道**（halt-on-error）；**Lua 驱动沙箱**；`BOT4CJ_DEBUG` 开关。
- Lua 离线测试统一入口 `test/run_all.sh`，test/ 套件全量入库。

### 变更

- @Daemon chunk 按 `BOT4CJ_MODULE_PATH` 寻址；硬件变量类迁出 `src/HV/` 子包。
- HAL i2c 改用 `I2C_RDWR` 原子事务；DSL 语义收紧。
- 元数据收集改静态列缓存；驱动样板收敛 `drv_common.lua`（统一 52 个驱动）。
- Cangjie 内核瘦身：`Lua.cj` 闭包拆分、`r.cj` 重构分发器。
- 依赖升级 luajit4cj 1.0.1、pthread4cj 1.0.2。

### 修复

- `sensor_daemon` 浮点静默向下取整修复；守护线程 stop/join/泄漏三连修复。
- `sandbox_lua` 栈索引错误；PCA9685 channel 越界校验。
- MPU6050 六轴符号扩展；INA219/INA3221 位左移兼容性修复。
- **ADS1x15 OS 位读语义反转**（datasheet 级）；**ADS1256 满量程换算少乘 2**。
- pthread `publish` 入锁前数值校验；SPI setup 失败关闭泄漏 fd。
- 审计加固两轮（驱动返回值判读、print 噪音收敛、halt-on-error 补强等）。

## [0.2.0] - 2026-09-04

HAL 彻底甩掉宿主注册的 cfunc，进入 LuaJIT ffi 时代（此后仅支持 LuaJIT）。

### 新增

- LuaJIT 绑定抽取为独立包 luajit4cj；fft、kalman 由 C 移植为 Lua 库。

### 变更

- HAL 以 ffi 直调 glibc（open/ioctl/read/write/usleep）；SPI 延时改为逐次显式传入；`i2c_set_addr` 支持 EBUSY 回退 FORCE。

### 修复

- LuaJIT ffi 变参 ABI 陷阱：ioctl 地址参数显式 cast。

## [0.1.1] - 2026-09-03 ~ 2026-09-04

### 新增

- 通用 **controller 引用机制**；动作脚本支持内联 @Alias 定义；BME280 驱动。

### 变更

- 基层 I2C/SPI 驱动完全解耦为纯 Lua HAL（issue #14）；Servo 迁移为 Lua 驱动；`-` 占位符归一为 `0`。

## [0.0.6] - 2026-09-01 ~ 2026-09-02

### 新增

- **机器人结构脚本迁移到 SQL**：新版 `.sql` 结构文件由 SQL4cj 解析，旧版 `.mt` 结构矩阵废弃。
- 新驱动：Timer、SGP30、Dummy。

### 变更

- 动作与结构解耦；@Action 按标题行列序执行；解析器独立为模块。

## [0.0.5] - 2026-08-31

### 新增

- Lua 桥新增 burst 读写函数；重写 ADS1115 / ADS1256 驱动。

### 变更

- 全面改进错误处理与可配置性。

### 修复

- 解析器逻辑错误与调试代码残留；硬件变量重复键检查（Issue #8）。

## [0.0.4] - 2026-08-31

### 变更

- 更新外部依赖。

## [0.0.3] - 2026-05-31（tag v0.0.3beta；6~8 月在其上持续演进）

项目最大转折点：嵌入 Lua 状态机，硬件变量改由 Lua 脚本实现。

### 新增

- Lua 状态机嵌入与 MtVars_Lua；ADS1256/ADS1115 ADC、SPI 驱动；pthread4cj；spi4cj 独立仓库。
- I2C 驱动重建；MPU6050 迁移跑通；删除遗留构建脚本与杂项 test 目录。

## [v0.0.2beta] - 2026-05-03（tag；5 月上中旬持续）

硬件变量体系成型期。

### 新增

- 占位符支持与流式读取；Servo/PCA9685 硬件变量；RobotStruct 类封装。
- @Alias/@Action 解析；原生 I2C；pthread 线程与 I2C 锁；MPU6050。
- kalman 滤波器；Cangjie 版神经网络（矩阵/卷积/Adam）。
- 动作集扩充：行走/转向/倾斜/举手/躺下等。

### 移除

- 旧的神经网络与矩阵、声音（Speaker）部分及旧 I2C 接口。

## [v0.0.1beta] - 2026-04-21（tag；2026-01-28 项目起步）

探索期：从 C 直读传感器到强化学习驱动的机器人动作。

### 新增

- 项目创建：仓颉 + C 混合 I2C/MPU6050 读取；Actor 动作脚本解析器与 `Actor.play()`。
- PCA9685 舵机驱动；蜘蛛机器人步态脚本；FFT 库；神经网络重构；CUDA 矩阵加速。
- DRL 强化学习；CNN 卷积神经网络库；Windows 平台支持；Parser.cj 独立。
