-- test_pca9685.lua —— PCA9685 真实驱动 + 虚拟 I2C 总线 + regmap 包装模型
-- 用法（项目根目录）: luajit test/test_pca9685.lua（驱动依赖 HAL ffi，需 LuaJIT）
--   GC 压力节奏：PCA_GC=1 luajit test/test_pca9685.lua
--
-- 从 3 个手写桩文件（test_pca9685_channel/float/writefail.lua）整体迁移到
-- 「真实驱动 + virtbus 注入层 + virtbus/pca9685.lua 模型」。本器件是 virtbus
-- 库【8 位字节指针 regmap 包装族 · 纯输出器件】样板（NXP PCA9685 16 路
-- 12 位 PWM/舵机驱动，0x40 缺省/0x7F 地址上限，引脚可配 0x40~0x7F）：
--   MODE1 RESTART(bit7) 写 1 永不落盘 + SLEEP 0→1 挂起重启请求、读回动态
--   合成 bit7；EXTCLK(bit6) 粘滞，仅成功 RESTART 能清；PRE_SCALE 仅 SLEEP
--   可写、最小钳 3；burst 自增完全由 AI(bit5) 闸门（AI=0 全部落同一寄存器/
--   连读重复），0xFF 算术回绕 0x00；LED 块小端 12 位 + FULL(bit12)。
--   无测量值/无 virt_tick，守护采样 E2E（阶段 C）豁免，只做 A+B+D。
--
-- 覆盖：模型侧 POR 六颗种子字/重启状态机（POR 无挂起 0x80 忽略、SLEEP
-- 挂起、RESTART 自清计数、build 尾态读回 0xA0、EXTCLK 粘滞与重启清）/
-- PRE_SCALE 醒着写丢弃 + 计数 + 最小钳/AI 闸门多字节写与连读/回绕撞
-- MODE1/LED 小端 12 位编解码独立公式交叉 + FULL 位 + 越界通道 nil/
-- ALL_LED 同构/坏帧与 1B 纯指针/六类钩子/reset 回 POR 保钩子；真实
-- build 1 set+4w（{00,80}{00,10}{FE,7A}{00,20}，10ms 睡眠）与备地址
-- 及四笔写逐一 NACK 短路 + 睡眠账 + 参数负路径；真实 call 1 set+1w
-- 5B 原子帧（ON 双零 + OFF 小端）、角度码表/字符串与浮点/限幅/16 通道
-- 寄存器步进/相邻块不踩/channel·angle 参数零写短路/写失败；destroy
-- {00,80} 重启 + 负路径。

if os.getenv("PCA_GC") == "1" then
	collectgarbage("setpause", 1)
	collectgarbage("setstepmul", 100000)
end

local tester = dofile("test/lib/tester.lua")
local check, expect_ok, expect_err = tester.check, tester.expect_ok, tester.expect_err

local vbus = dofile("modules/lib/virtbus.lua")
local pmod = dofile("modules/lib/virtbus/pca9685.lua")
vbus.reset()

local dev = pmod.new()
vbus.attach("PCA9685@0x40", 0x40, dev)
vbus.attach("PCA9685@0x7F", 0x7F, dev)
vbus.load_driver("modules/PCA9685.lua")

-- 输出器件无转换门：milli_sleep 桩仅累加留痕（build 两段各 5ms）
local sleep_total = 0
function milli_sleep(ms) sleep_total = sleep_total + (tonumber(ms) or 0) end

local fd = i2c_open("/dev/i2c-1")	-- 虚拟 fd 从 3 起
check("i2c_open 虚拟 fd=3", fd == 3, tostring(fd))

-- 每个 section：器件回 POR + 故障解除（挂载/fd 不动）
local function section()
	dev:reset()
	dev:clear_faults()
	sleep_total = 0
	return #vbus.txlog
end
-- 只开事务窗口、不复位器件（build 之后用，保留 MODE1/PRESCALE 配置）
local function window()
	sleep_total = 0
	return #vbus.txlog
end

local function entries(mark)
	local t = {}
	for i = (mark or 0) + 1, #vbus.txlog do t[#t + 1] = vbus.txlog[i] end
	return t
end
local function writes(mark)
	local t = {}
	for _, e in ipairs(entries(mark)) do
		if e.dir == "w" then t[#t + 1] = e.bytes end
	end
	return t
end
local function nset(mark)
	local n = 0
	for _, e in ipairs(entries(mark)) do if e.dir == "set" then n = n + 1 end end
	return n
end
local function nwr(mark) return #writes(mark) end
local function nrd(mark)
	local n = 0
	for _, e in ipairs(entries(mark)) do if e.dir == "r" then n = n + 1 end end
	return n
end
local function frame_eq(f, ...)
	local want = { ... }
	if type(f) ~= "table" or #f ~= #want then return false end
	for i = 1, #want do
		if (f[i] or -1) ~= want[i] then return false end
	end
	return true
end
local function frames_eq(mark, ...)
	local got = writes(mark)
	local args = { ... }
	if #got ~= #args then return false end
	for i = 1, #args do
		if not frame_eq(got[i], table.unpack(args[i])) then return false end
	end
	return true
end
local function hx(t)
	local s = {}
	for _, b in ipairs(t) do s[#s + 1] = string.format("%02X", b) end
	return table.concat(s, " ")
end

-- 测试侧独立角度→OFF 码参考（不看驱动/模型实现，datasheet 20ms@50Hz）
local function off_ref(angle)
	local a = tonumber(angle)
	if a < 0 then a = 0 end
	if a > 180 then a = 180 end
	local width = 0.5 + 2.0 * (a / 180.0)
	return math.floor(width * 4096 / 20)
end

-- 第 n 次命中指定帧【前缀】时 NACK 的谓词工厂（n=1 即首次；按前缀匹配，
-- 如 sig={0xFE} 命中驱动的 2B 帧 {0xFE,val}）
local function nth_frame_nack(...)
	local sig = { ... }
	local seen = 0
	return function(b)
		if #b < #sig then return false end
		for i = 1, #sig do
			if sig[i] ~= b[i] then return false end
		end
		seen = seen + 1
		return seen == 1
	end
end

-- =====================================================================
-- 组 1：模型协议事实（脱驱动，直接点模型 API）
-- =====================================================================
local m = section()

check("POR MODE1=0x01（ALLCALL）", dev:mode1() == 0x01, string.format("%02X", dev:mode1()))
check("POR MODE2=0x04（OUTDRV）", dev:mode2() == 0x04, string.format("%02X", dev:mode2()))
check("POR SUBADR1=0xE2", dev:subadr(1) == 0xE2, string.format("%02X", dev:subadr(1)))
check("POR SUBADR2=0xE4", dev:subadr(2) == 0xE4)
check("POR SUBADR3=0xE8", dev:subadr(3) == 0xE8)
check("POR ALLCALLADR=0xE0（0x70<<1）", dev:allcall_adr() == 0xE0)
check("POR PRE_SCALE=0x1E(30)", dev:prescale() == 0x1E, tostring(dev:prescale()))
check("POR 频率 196.9Hz", math.abs(dev:freq_hz() - 196.8876) < 0.01,
	tostring(dev:freq_hz()))
check("POR 位观察口 sleep/ai/extclk=0 allcall=1",
	(not dev:sleep()) and (not dev:ai()) and (dev:extclk() == 0) and dev:allcall() == 1)
check("POR 无重启挂起/计数 0/丢弃 0",
	dev:restart_pending() == false and dev:restart_count() == 0 and dev:prescale_drop() == 0)
for ch = 0, 15 do
	local on, onf, off, offf = dev:get_led(ch)
	check(string.format("POR LED%d 计数块全 0", ch),
		on == 0 and onf == 0 and off == 0 and offf == 0)
end
local aon, aonf, aoff, aofff = dev:get_all_led()
check("POR ALL_LED 块全 0", aon == 0 and aonf == 0 and aoff == 0 and aofff == 0)
check("get_led 越界 -1/16 返 nil", dev:get_led(-1) == nil and dev:get_led(16) == nil)
check("subadr 越界返 nil", dev:subadr(0) == nil and dev:subadr(4) == nil)

-- RESTART 状态机：POR 无挂起，写 0x80 整位忽略、不落盘、不计数
dev.write({ 0x00, 0x80 })
check("POR 写 0x80：MODE1 读 0x00（RESTART 不落盘）",
	dev:mode1() == 0x00, string.format("%02X", dev:mode1()))
check("POR 写 0x80：重启计数 0（无挂起请求）", dev:restart_count() == 0)
check("POR 写 0x80：ALLCALL 随低 7 位被清 0", dev:allcall() == 0)
check("POR 写 0x80：仍无挂起", dev:restart_pending() == false)

-- SLEEP 0→1 挂起重启请求（MODE1 读 bit7 动态合成）
dev.write({ 0x00, 0x10 })
check("SLEEP 后读回 0x90（bit7 挂起 + bit4）",
	dev:mode1() == 0x90, string.format("%02X", dev:mode1()))
check("SLEEP 位=1", dev:sleep() == true)
check("重启请求已挂起", dev:restart_pending() == true)
check("挂起不增加重启计数", dev:restart_count() == 0)

-- 醒着写 PRE_SCALE 丢弃（必须先 SLEEP）——当前在 SLEEP，先验证睡眠态可写
dev.write({ 0xFE, 0x7A })
check("SLEEP 态 PRE_SCALE=0x7A(122) 落盘",
	dev:prescale() == 0x7A, string.format("%02X", dev:prescale()))
check("PRE_SCALE=122 对应 49.6Hz", math.abs(dev:freq_hz() - 49.6221) < 0.01,
	tostring(dev:freq_hz()))
-- build 尾笔：AI 唤醒（未用 RESTART），挂起请求保留
dev.write({ 0x00, 0x20 })
check("AI 唤醒后读回 0xA0（bit7 挂起仍在 + AI）",
	dev:mode1() == 0xA0, string.format("%02X", dev:mode1()))
check("唤醒后 sleep=0 ai=1", dev:sleep() == false and dev:ai() == true)
check("未走 RESTART：计数仍 0、挂起仍在",
	dev:restart_count() == 0 and dev:restart_pending() == true)
-- 醒着改 PRE_SCALE：ACK 但丢弃
dev.write({ 0xFE, 0x40 })
check("醒着写 PRE_SCALE 丢弃保持 0x7A", dev:prescale() == 0x7A)
check("丢弃计数=1", dev:prescale_drop() == 1)
-- destroy 路径：有挂起时写 0x80 执行重启
dev.write({ 0x00, 0x80 })
check("挂起态写 0x80：RESTART 执行、读回 0x00",
	dev:mode1() == 0x00, string.format("%02X", dev:mode1()))
check("重启计数=1、挂起清除",
	dev:restart_count() == 1 and dev:restart_pending() == false)
check("重启写低 7 位全 0：AI 也被清", dev:ai() == false)

-- PRE_SCALE 最小值钳 3（datasheet）
m = section()
dev.write({ 0x00, 0x10 })	-- 进 SLEEP
dev.write({ 0xFE, 0x00 })
check("PRE_SCALE 写 0 钳到 3", dev:prescale() == 3, tostring(dev:prescale()))
dev.write({ 0xFE, 0x02 })
check("PRE_SCALE 写 2 钳到 3", dev:prescale() == 3)
dev.write({ 0xFE, 0x03 })
check("PRE_SCALE 写 3 落 3", dev:prescale() == 3)
dev.write({ 0xFE, 0xFF })
check("PRE_SCALE 写 255 落 255", dev:prescale() == 255)
dev:reset()
dev.write({ 0xFE, 0x03 })
check("POR 醒着写 3 也丢弃（保持 0x1E）",
	dev:prescale() == 0x1E and dev:prescale_drop() == 1)

-- EXTCLK 粘滞：置 1 后普通写 0 清不掉，RESTART 才清
m = section()
dev.write({ 0x00, 0x10 })	-- SLEEP（EXTCLK 须在睡眠态切换）
dev.write({ 0x00, 0x50 })	-- SLEEP+EXTCLK
check("置 EXTCLK：extclk=1、sleep=1", dev:extclk() == 1 and dev:sleep() == true)
dev.write({ 0x00, 0x60 })	-- EXTCLK+AI 唤醒
check("EXTCLK 唤醒读回 0xE0（挂起 bit7+EXTCLK+AI）",
	dev:mode1() == 0xE0, string.format("%02X", dev:mode1()))
dev.write({ 0x00, 0x20 })	-- 试图清 EXTCLK
check("普通写清 EXTCLK 无效（保位，仍 0xE0）",
	dev:mode1() == 0xE0, string.format("%02X", dev:mode1()))
check("EXTCLK 观察口仍 1", dev:extclk() == 1)
dev.write({ 0x00, 0x80 })	-- 成功 RESTART 清 EXTCLK
check("RESTART 清 EXTCLK：读回 0x00",
	dev:mode1() == 0x00, string.format("%02X", dev:mode1()))
check("EXTCLK=0 重启计数=1", dev:extclk() == 0 and dev:restart_count() == 1)

-- AI 闸门：AI=0 多字节写全部落同一寄存器（末字节赢）
m = section()
dev.write({ 0x06, 0x11, 0x22 })
check("AI=0 多字节写：0x06=0x22（末字节赢）",
	dev.regs[0x06] == 0x22, string.format("%02X", dev.regs[0x06]))
check("AI=0 多字节写：0x07 未触碰（nil 读 0）", (dev.regs[0x07] or 0) == 0)
check("AI=0 留痕仍为一帧 3B",
	#dev.write_log[#dev.write_log] == 3 and dev:write_count() == 1)
dev.write({ 0x00, 0x20 })	-- 开 AI
dev.write({ 0x06, 0x11, 0x22, 0x33 })
check("AI=1 burst：0x06/07/08 = 11/22/33",
	dev.regs[0x06] == 0x11 and dev.regs[0x07] == 0x22 and dev.regs[0x08] == 0x33,
	string.format("%X %X %X", dev.regs[0x06], dev.regs[0x07], dev.regs[0x08]))
-- AI=0 连读重复同一寄存器
dev:reset()
dev.write({ 0x00 })			-- 指针指 MODE1
local rep = dev.read(3)
check("AI=0 连读 3 次重复 MODE1=0x01",
	rep[1] == 0x01 and rep[2] == 0x01 and rep[3] == 0x01,
	hx(rep))
check("AI=0 连读指针不前进", dev:pointer() == 0x00)
-- AI=1 连读逐字节推进
dev.write({ 0x00, 0x20 })
dev.write({ 0xFD })			-- 指针指 0xFD（MODE2 前一区，避开动态位）
dev.write({ 0x02 })			-- SUBADR1
local inc = dev.read(4)
check("AI=1 连读 02/03/04/05 = E2/E4/E8/E0",
	inc[1] == 0xE2 and inc[2] == 0xE4 and inc[3] == 0xE8 and inc[4] == 0xE0,
	hx(inc))
check("AI=1 连读指针停 0x06", dev:pointer() == 0x06,
	string.format("%02X", dev:pointer()))
-- 0xFF 回绕 0x00（回绕撞 MODE1 走 MODE1 写解码）
dev.write({ 0xFF, 0x55, 0x00 })
check("burst 回绕：0xFF=0x55", dev.regs[0xFF] == 0x55)
check("burst 回绕撞 MODE1：0x00 数据走模式位解码（AI 被清零）",
	dev:mode1() == 0x00 and dev:ai() == false,
	string.format("%02X", dev:mode1()))
-- 读回绕
dev:reset()
dev.write({ 0x00, 0x20 })
dev.write({ 0xFE })
local wrapr = dev.read(3)
check("读回绕 FE/FF/00 = PRESCALE/00/MODE1",
	wrapr[1] == 0x1E and wrapr[2] == 0x00 and wrapr[3] == 0x20,
	hx(wrapr))

-- LED 小端 12 位编解码（独立公式交叉）+ FULL 位
m = section()
local function enc_ref(count, full)
	count = math.floor(count)
	local lo = count % 256
	local hi = math.floor(count / 256) % 16
	if full then hi = hi + 16 end
	return lo, hi
end
local led_cases = {
	{ 0,    0,    0x06 },
	{ 0xABC, 0xDEF, 0x06 },
	{ 4095, 4095, 0x06 },
	{ 102,  512,  0x06 + 4 * 15 },
}
for _, c in ipairs(led_cases) do
	local ch = math.floor((c[3] - 0x06) / 4)
	dev:set_led(ch, c[1], c[2])
	local elo, ehi = enc_ref(c[1], false)
	local flo, fhi = enc_ref(c[2], false)
	check(string.format("LED%d ON 小端编码 %04X", ch, c[1]),
		dev.regs[c[3]] == elo and dev.regs[c[3] + 1] == ehi,
		string.format("%02X %02X want %02X %02X",
			dev.regs[c[3]], dev.regs[c[3] + 1], elo, ehi))
	check(string.format("LED%d OFF 小端编码 %04X", ch, c[2]),
		dev.regs[c[3] + 2] == flo and dev.regs[c[3] + 3] == fhi)
	local on, onf, off, offf = dev:get_led(ch)
	check(string.format("LED%d 解码回读 on=%d off=%d full=0", ch, c[1], c[2]),
		on == c[1] and onf == 0 and off == c[2] and offf == 0,
		string.format("%d %d %d %d", on, onf, off, offf))
end
-- FULL 位（高字节 bit4=第 13 位）
dev:set_led(6, 4095, 0, true, false)
check("LED6 ON_FULL：ON_H=0x1F",
	dev.regs[0x1E] == 0xFF and dev.regs[0x1F] == 0x1F,
	string.format("%02X %02X", dev.regs[0x1E], dev.regs[0x1F]))
local f6on, f6onf, f6off, f6offf = dev:get_led(6)
check("LED6 解码 count=4095 on_full=1", f6on == 4095 and f6onf == 1 and f6off == 0)
dev:set_led(6, 0, 4095, false, true)
check("LED6 OFF_FULL：OFF_H=0x1F", dev.regs[0x20] == 0xFF and dev.regs[0x21] == 0x1F)
local _, _, o6, of6 = dev:get_led(6)
check("LED6 解码 off=4095 off_full=1", o6 == 4095 and of6 == 1)
-- 注入钳制
dev:set_led(0, 99999, -5)
local z0, _, z2 = dev:get_led(0)
check("set_led 超程钳 4095/0", z0 == 4095 and z2 == 0, tostring(z0) .. "/" .. tostring(z2))
-- ALL_LED 同构
dev:set_all_led(0x123, 0x456)
local al1, alf1, al2, alf2 = dev:get_all_led()
check("ALL_LED 编解码同构 0xFA 起",
	dev.regs[0xFA] == 0x23 and dev.regs[0xFB] == 0x01
	and dev.regs[0xFC] == 0x56 and dev.regs[0xFD] == 0x04
	and al1 == 0x123 and al2 == 0x456 and alf1 == 0 and alf2 == 0,
	string.format("%X %X %X %X", dev.regs[0xFA], dev.regs[0xFB],
		dev.regs[0xFC], dev.regs[0xFD]))

-- 坏帧 NACK / 1B 纯指针 ACK / 计数留痕
m = section()
check("write(nil) NACK", dev.write(nil) == false)
check("write('x') NACK", dev.write("x") == false)
check("write({}) NACK", dev.write({}) == false)
check("坏帧不进 write_log", dev:write_count() == 0, tostring(dev:write_count()))
check("1B 纯指针任意地址 ACK", dev.write({ 0xFE }) == true)
check("1B 纯指针设 ptr=0xFE", dev:pointer() == 0xFE)
check("1B 纯指针不计 write_count", dev:write_count() == 0)

-- 六类故障钩子
m = section()
dev.nack = true
check("nack=true 写失败", dev.write({ 0x06, 0, 0, 0, 0 }) == false)
check("nack=true 读失败", dev.read(1) == nil)
dev.nack = false
dev.nack_write = true
check("nack_write 写失败", dev.write({ 0x00, 0x20 }) == false)
check("nack_write 读正常", dev.read(1) ~= nil and dev.read(1)[1] == 0x01)
dev.nack_write = false
dev.nack_read = true
check("nack_read 读失败", dev.read(1) == nil)
check("nack_read 写正常", dev.write({ 0x01, 0x04 }) == true)
dev.nack_read = false
dev:fail_write(function(b) return b[1] == 0xFE end)
check("fail_write 谓词拦 PRE_SCALE 帧", dev.write({ 0xFE, 0x7A }) == false)
check("fail_write 谓词放行 MODE1 帧", dev.write({ 0x00, 0x20 }) == true)
dev:fail_read(function(ptr) return ptr == 0x01 end)
dev.write({ 0x01 })
check("fail_read 谓词拦 MODE2 读", dev.read(1) == nil)
dev.write({ 0x02 })
local fr = dev.read(1)	-- 此时前面的 {00,20} 已开 AI，存局部勿连读两次
check("fail_read 谓词放行 SUBADR1 读", fr ~= nil and fr[1] == 0xE2,
	fr and string.format("%02X", fr[1]) or "nil")

-- reset：回 POR、清挂起/计数/留痕、保留故障钩子
m = section()
dev:fail_write(function(b) return b[1] == 0x00 and b[2] == 0x80 end)
dev.write({ 0x00, 0x10 })
dev.write({ 0x00, 0x80 })
dev:reset()
check("reset 后 MODE1=0x01/PRE=0x1E", dev:mode1() == 0x01 and dev:prescale() == 0x1E)
check("reset 后挂起/重启计数/丢弃计数清零",
	dev:restart_pending() == false and dev:restart_count() == 0 and dev:prescale_drop() == 0)
check("reset 后 write_log 清空", #dev.write_log == 0)
check("reset 保留 fail_write 钩子（0x80 仍被拦）",
	dev.write({ 0x00, 0x80 }) == false)
dev:clear_faults()

-- =====================================================================
-- 组 2：真实 build
-- =====================================================================
local base = { fd = fd, address = "0x40" }

m = section()
expect_ok("build 正常返回 OK", function() return build(base) end)
check("build 事务=1 set + 4 写", nset(m) == 1 and nwr(m) == 4 and nrd(m) == 0,
	string.format("set=%d w=%d r=%d", nset(m), nwr(m), nrd(m)))
-- 驱动 prescale = floor(25e6/(4096*50) - 1 + 0.5) = floor(121.5703) = 121
check("build 四笔写帧序列",
	frames_eq(m, { 0x00, 0x80 }, { 0x00, 0x10 }, { 0xFE, 0x79 }, { 0x00, 0x20 }),
	hx(writes(m)[1] or {}) .. " | " .. hx(writes(m)[4] or {}))
check("build 总睡眠 10ms（5+5）", sleep_total == 10, tostring(sleep_total))
check("build 后 PRE_SCALE=121(0x79)", dev:prescale() == 121, tostring(dev:prescale()))
check("build 后 MODE1 读 0xA0（AI+挂起）",
	dev:mode1() == 0xA0, string.format("%02X", dev:mode1()))
check("build 后 sleep=0 ai=1", dev:sleep() == false and dev:ai() == true)
check("build 未触发 RESTART（计数 0、挂起保留）",
	dev:restart_count() == 0 and dev:restart_pending() == true)
check("build 后频率约 50.03Hz", math.abs(dev:freq_hz() - 50.0303) < 0.01,
	tostring(dev:freq_hz()))

-- 备地址（地址上限 0x7F）
m = section()
expect_ok("build 备地址 0x7F 成功",
	function() return build({ fd = fd, address = "0x7F" }) end)
check("备地址 1 set+4w", nset(m) == 1 and nwr(m) == 4)
check("备地址写帧 addr 均 0x7F",
	entries(m)[2].addr == 0x7F and entries(m)[5].addr == 0x7F,
	string.format("%s/%s", tostring(entries(m)[2].addr), tostring(entries(m)[5].addr)))

-- 首笔 RESTART 写失败 → 短路，不睡
m = section()
dev:fail_write(nth_frame_nack(0x00, 0x80))
expect_err("build 首笔写失败报 MODE1",
	function() return build(base) end, "MODE1 write failed")
check("首笔失败：1 set+1w 0r", nset(m) == 1 and nwr(m) == 1 and nrd(m) == 0,
	string.format("set=%d w=%d", nset(m), nwr(m)))
check("首笔失败：睡眠 0", sleep_total == 0, tostring(sleep_total))

-- SLEEP 写失败（第 2 笔）→ 已睡 5ms，短路
m = section()
dev:fail_write(nth_frame_nack(0x00, 0x10))
expect_err("build SLEEP 写失败",
	function() return build(base) end, "MODE1 (SLEEP)")
check("SLEEP 失败：1 set+2w", nset(m) == 1 and nwr(m) == 2)
check("SLEEP 失败：已睡 5ms", sleep_total == 5, tostring(sleep_total))

-- PRE_SCALE 写失败（第 3 笔）
m = section()
dev:fail_write(nth_frame_nack(0xFE))
expect_err("build PRE_SCALE 写失败",
	function() return build(base) end, "PRE_SCALE")
check("PRE_SCALE 失败：1 set+3w", nset(m) == 1 and nwr(m) == 3)
check("PRE_SCALE 失败：已睡 5ms", sleep_total == 5)

-- AI 写失败（第 4 笔；第二段 5ms 在写成功之后，失败时不发生）
m = section()
dev:fail_write(nth_frame_nack(0x00, 0x20))
expect_err("build AI 写失败",
	function() return build(base) end, "MODE1 (AI)")
check("AI 失败：1 set+4w", nset(m) == 1 and nwr(m) == 4)
check("AI 失败：仍只睡 5ms（尾睡未发生）", sleep_total == 5, tostring(sleep_total))

-- 参数负路径（arg_table 先于 i2c_pair → 零总线事务）
m = section()
expect_err("build 非 table", function() return build("x") end, "Invalid Arguments Type: table")
check("非 table 零事务", nset(m) == 0 and nwr(m) == 0)
expect_err("build 缺 fd/address",
	function() return build({}) end, "fd or address")
check("缺 fd/address 零事务", nset(m) == 0 and nwr(m) == 0)
expect_err("build fd 非数字",
	function() return build({ fd = "x", address = "0x40" }) end, "fd or address")
check("fd 非数字零事务", nset(m) == 0 and nwr(m) == 0)
expect_err("build 未打开 fd=99 set_addr 失败",
	function() return build({ fd = 99, address = "0x40" }) end, "set_addr failed")
check("fd=99 零总线事务（set 校验在留痕前）", nset(m) == 0 and nwr(m) == 0)
-- 地址 >0x7F：set_addr 直接拒绝（零留痕）
m = section()
expect_err("build 地址 0x80 set_addr 拒绝",
	function() return build({ fd = fd, address = "0x80" }) end, "set_addr failed")
check("地址 0x80 零总线事务", nset(m) == 0 and nwr(m) == 0)
-- 合法但未挂载地址 0x60：set_addr 成功留痕（vbus 不检查器件存在），
-- 首笔写 NACK → 驱动报 not responding
m = section()
expect_err("build 未挂载地址 0x60 首写失败",
	function() return build({ fd = fd, address = "0x60" }) end, "MODE1 write failed")
check("未挂载地址：1 set 留痕 + 1w（失败写也留痕）",
	nset(m) == 1 and nwr(m) == 1 and nrd(m) == 0,
	string.format("set=%d w=%d", nset(m), nwr(m)))

-- =====================================================================
-- 组 3：真实 call（build 后开窗，保留配置）
-- =====================================================================
m = section()
expect_ok("前置 build", function() return build(base) end)
local bm = window()

-- 角度码表：独立参考公式交叉，帧精确到 5 字节
local angle_pts = { 0, 1, 45, 44.7, 90, 179, 180 }
for ch, angle in ipairs({ 0, 6, 15 }) do
	local c = ({ 0, 6, 15 })[ch]
	for _, ang in ipairs(angle_pts) do
		local mk = window()
		expect_ok(string.format("call ch%d angle=%s", c, tostring(ang)),
			function() return call({ fd = fd, address = "0x40", channel = c, angle = ang }) end)
		local o = off_ref(ang)
		check(string.format("ch%d angle=%s 1 set+1w 5B 原子帧", c, tostring(ang)),
			nset(mk) == 1 and nwr(mk) == 1 and #writes(mk)[1] == 5 and nrd(mk) == 0,
			string.format("set=%d w=%d", nset(mk), nwr(mk)))
		check(string.format("ch%d angle=%s 帧 {0x%02X,00,00,%02X,%02X}", c, tostring(ang),
			0x06 + 4 * c, o % 256, math.floor(o / 256)),
			frame_eq(writes(mk)[1], 0x06 + 4 * c, 0, 0, o % 256, math.floor(o / 256)),
			hx(writes(mk)[1]))
		local on, onf, off, offf = dev:get_led(c)
		check(string.format("ch%d angle=%s 模型解码 OFF=%d ON=0 full=0", c, tostring(ang), o),
			on == 0 and onf == 0 and off == o and offf == 0,
			string.format("off=%d want=%d", off, o))
	end
end

-- 字符串/浮点角度同路
for _, v in ipairs({ "45", "44.7", "90", "0", "180" }) do
	local mk = window()
	expect_ok("call 字符串角度 '" .. v .. "'",
		function() return call({ fd = fd, address = "0x40", channel = "6", angle = v }) end)
	check("字符串角度帧 OFF 与数值一致",
		frame_eq(writes(mk)[1], 0x1E, 0, 0, off_ref(v) % 256, math.floor(off_ref(v) / 256)),
		hx(writes(mk)[1]))
end
do
	local mk = window()
	expect_ok("call 浮点 angle=44.7",
		function() return call({ fd = fd, address = "0x40", channel = 6, angle = 44.7 }) end)
	check("浮点 44.7 OFF=204（floor 量化）",
		frame_eq(writes(mk)[1], 0x1E, 0, 0, 0xCC, 0x00), hx(writes(mk)[1]))
end

-- 角度限幅：越界钳到 0/180（旧 test_pca9685_float 覆盖点）
for _, v in ipairs({ -30, "-30", 250, "250", 999, -999 }) do
	local mk = window()
	expect_ok("call 越界角度 '" .. tostring(v) .. "' 限幅不报错",
		function() return call({ fd = fd, address = "0x40", channel = 6, angle = v }) end)
	local o = off_ref(v)
	check(string.format("越界角度 %s 限幅 OFF=%d（102 或 512）", tostring(v), o),
		o == 102 or o == 512,
		hx(writes(mk)[1]))
end

-- 16 通道寄存器步进：reg = 0x06 + 4*ch，全部在 LED 区内
for c = 0, 15 do
	local mk = window()
	expect_ok(string.format("call 全通道 ch%d", c),
		function() return call({ fd = fd, address = "0x40", channel = c, angle = 90 }) end)
	check(string.format("ch%d 基址 0x%02X OFF=307", c, 0x06 + 4 * c),
		frame_eq(writes(mk)[1], 0x06 + 4 * c, 0, 0, 0x33, 0x01),
		hx(writes(mk)[1]))
end
-- 相邻通道块不被踩：ch8 写完后 ch7/ch9 保持各自值
do
	local _, _, off7 = dev:get_led(7)
	local _, _, off8 = dev:get_led(8)
	local _, _, off9 = dev:get_led(9)
	check("相邻块隔离（7/8/9 各 307）", off7 == 307 and off8 == 307 and off9 == 307,
		string.format("%d/%d/%d", off7, off8, off9))
	local mk = window()
	expect_ok("重写 ch8=0",
		function() return call({ fd = fd, address = "0x40", channel = 8, angle = 0 }) end)
	local _, _, n7 = dev:get_led(7)
	local _, _, n8 = dev:get_led(8)
	local _, _, n9 = dev:get_led(9)
	check("重写 ch8 仅改自身（ch7/ch9 仍 307，ch8=102）",
		n7 == 307 and n8 == 102 and n9 == 307,
		string.format("%d/%d/%d", n7, n8, n9))
	check("重写写帧恰 5B 无越界字节", #writes(mk)[1] == 5)
	check("ch8 末地址 0x29 不踩 0x2A", dev.regs[0x2A] == 0)
end
-- 字符串通道
do
	local mk = window()
	expect_ok("字符串通道 '7'",
		function() return call({ fd = fd, address = "0x40", channel = "7", angle = 90 }) end)
	check("字符串通道基址 0x22",
		frame_eq(writes(mk)[1], 0x22, 0, 0, 0x33, 0x01), hx(writes(mk)[1]))
end

-- channel 参数负路径（i2c_pair 在参数校验前 → 1 set 0w）
local function call_err(label, data, msgpat, sets)
	local mk = window()
	expect_err(label, function() return call(data) end, msgpat)
	check(label .. " 1 set 0w",
		nset(mk) == (sets or 1) and nwr(mk) == 0 and nrd(mk) == 0,
		string.format("set=%d w=%d", nset(mk), nwr(mk)))
end
call_err("call 缺 channel", { fd = fd, address = "0x40", angle = 90 }, "Invalid Argument: channel")
call_err("call channel 非数字",
	{ fd = fd, address = "0x40", channel = "abc", angle = 90 }, "channel (not a number)")
call_err("call channel=16 越界",
	{ fd = fd, address = "0x40", channel = 16, angle = 90 }, "out of range 0-15")
call_err("call channel=-1 越界",
	{ fd = fd, address = "0x40", channel = -1, angle = 90 }, "out of range 0-15")
call_err("call channel=255 越界",
	{ fd = fd, address = "0x40", channel = 255, angle = 90 }, "out of range 0-15")
call_err("call 字符串 channel='20' 越界",
	{ fd = fd, address = "0x40", channel = "20", angle = 90 }, "out of range 0-15")
-- angle 参数负路径
call_err("call 缺 angle",
	{ fd = fd, address = "0x40", channel = 6 }, "Invalid Argument: angle")
call_err("call angle 非数字",
	{ fd = fd, address = "0x40", channel = 6, angle = "x" }, "angle (not a number)")
-- 非 table：arg_table 先于一切
do
	local mk = window()
	expect_err("call 非 table", function() return call("z") end, "Invalid Arguments Type: table")
	check("call 非 table 零事务", nset(mk) == 0 and nwr(mk) == 0)
end
-- fd=99 / 错地址
do
	local mk = window()
	expect_err("call fd=99 set_addr 失败",
		function() return call({ fd = 99, address = "0x40", channel = 6, angle = 90 }) end,
		"set_addr failed")
	check("call fd=99 零事务", nset(mk) == 0 and nwr(mk) == 0)
end
do
	local mk = window()
	expect_err("call 地址 0x80 set_addr 拒绝",
		function() return call({ fd = fd, address = "0x80", channel = 6, angle = 90 }) end,
		"set_addr failed")
	check("call 地址 0x80 零事务", nset(mk) == 0 and nwr(mk) == 0)
end
do
	local mk = window()
	expect_err("call 未挂载地址 0x60 PWM 写失败",
		function() return call({ fd = fd, address = "0x60", channel = 6, angle = 90 }) end,
		"PWM registers")
	check("call 未挂载地址：1 set + 1w", nset(mk) == 1 and nwr(mk) == 1)
end

-- PWM 写失败（5B 帧 NACK；vbus 对失败写仍留 w 痕）
do
	local mk = window()
	dev:fail_write(function(b) return #b == 5 end)
	expect_err("call PWM 帧写失败",
		function() return call({ fd = fd, address = "0x40", channel = 6, angle = 45 }) end,
		"PWM registers")
	check("PWM 失败：1 set+1w", nset(mk) == 1 and nwr(mk) == 1)
	dev:clear_faults()
end

-- 整个 build+call+destroy 零读事务
check("build→call 全程无读事务", nrd(bm) == 0, tostring(nrd(bm)))

-- =====================================================================
-- 组 4：destroy
-- =====================================================================
-- build 后 destroy：有挂起请求，{00,80} 执行 RESTART
m = section()
expect_ok("前置 build（destroy 组）", function() return build(base) end)
local dm = window()
expect_ok("destroy build 后返回 OK", function() return destroy(base) end)
check("destroy 1 set+1w 0r 零睡眠",
	nset(dm) == 1 and nwr(dm) == 1 and nrd(dm) == 0 and sleep_total == 0,
	string.format("set=%d w=%d sleep=%d", nset(dm), nwr(dm), sleep_total))
check("destroy 帧 {00,80}", frame_eq(writes(dm)[1], 0x00, 0x80), hx(writes(dm)[1]))
check("destroy 触发 RESTART（计数 1、挂起清除、MODE1=0）",
	dev:restart_count() == 1 and dev:restart_pending() == false and dev:mode1() == 0x00,
	string.format("rc=%d mode1=%02X", dev:restart_count(), dev:mode1()))

-- destroy 写失败
m = section()
expect_ok("前置 build（destroy 失败组）", function() return build(base) end)
local d2 = window()
dev:fail_write(nth_frame_nack(0x00, 0x80))
expect_err("destroy 写失败报 restart",
	function() return destroy(base) end, "MODE1 (restart)")
check("destroy 失败：1 set+1w", nset(d2) == 1 and nwr(d2) == 1)
dev:clear_faults()

-- POR 态直接 destroy：无挂起，0x80 不重启但帧照发、ALLCALL 被清
m = section()
expect_ok("POR 态 destroy 仍 OK", function() return destroy(base) end)
check("POR destroy 1 set+1w", nset(m) == 1 and nwr(m) == 1)
check("POR destroy 帧 {00,80}", frame_eq(writes(m)[1], 0x00, 0x80))
check("POR destroy 不重启（计数 0）但低 7 位清零 MODE1=0",
	dev:restart_count() == 0 and dev:mode1() == 0x00,
	string.format("rc=%d mode1=%02X", dev:restart_count(), dev:mode1()))

-- destroy 参数负路径
m = section()
expect_err("destroy 非 table",
	function() return destroy(7) end, "Invalid Arguments Type: table")
check("destroy 非 table 零事务", nset(m) == 0 and nwr(m) == 0)
m = section()
expect_err("destroy 缺 fd",
	function() return destroy({}) end, "fd or address")
check("destroy 缺 fd 零事务", nset(m) == 0 and nwr(m) == 0)
m = section()
expect_err("destroy fd=99",
	function() return destroy({ fd = 99, address = "0x40" }) end, "set_addr failed")
check("destroy fd=99 零事务", nset(m) == 0 and nwr(m) == 0)

tester.summary("test_pca9685")
