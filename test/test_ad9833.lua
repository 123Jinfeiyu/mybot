-- AD9833.lua 离线测试（桩替身，不触碰真实 SPI）
-- 用法: luajit test_ad9833.lua（AD9833 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ina226.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。spi_write_multi 桩把每帧两字节拼成 16 位字记录，
-- 逐字断言控制字/频率字/相位字序列与波形位组合。

dofile("modules/AD9833.lua")

local bitc = dofile("modules/bitcompat.lua")

-- ==================== SPI 桩 ====================
local words = {}			-- 每帧 16 位字（data[1]<<8 | data[2]）
local nak = false
local read_fail = false

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function spi_write_multi(fd, data, delay_us)
	if nak then return -1 end
	words[#words + 1] = data[1] * 256 + data[2]
	return 0
end

function spi_write(fd, val, delay_us) return nak and -1 or 0 end

-- ==================== 断言辅助 ====================
local fails = 0
local function check(cond, label, extra)
	if cond then
		print(string.format("[PASS] %s", label))
	else
		fails = fails + 1
		print(string.format("[FAIL] %s%s", label, extra and (" (" .. tostring(extra) .. ")") or ""))
	end
end

local function result_of(ok, res)
	if ok and type(res) == "table" then return tostring(res.result) end
	return "EXC:" .. tostring(res)
end

local function expect_ok(label, fn)
	local ok, res = pcall(fn)
	check(result_of(ok, res) == "OK", label, result_of(ok, res))
	return ok and res or nil
end

local function expect_err(label, fn, msgpat)
	local ok, res = pcall(fn)
	local msg = type(res) == "table" and tostring(res.message) or tostring(res)
	check(result_of(ok, res) == "ERR" and msg:find(msgpat, 1, true) ~= nil,
		label, msg)
end

local function words_hex()
	local t = {}
	for i, w in ipairs(words) do t[i] = string.format("0x%04X", w) end
	return table.concat(t, ",")
end

local function seq_is(label, want)
	local same = #words == #want
	for i = 1, math.min(#words, #want) do
		if words[i] ~= want[i] then same = false; break end
	end
	check(same, label, words_hex())
end

local FD = 7

-- ==================== build：默认参数（1kHz/sine/0°） ====================
words = {}
expect_ok("build 默认（1kHz/sine/0°）", function()
	return build({ fd = FD })
end)
-- RESET|B28 -> FREQ0 低14位 -> FREQ0 高14位 -> 相位 -> B28 放行（sine 位全 0）
seq_is("初始化五字序列", { 0x2100, bitc.bor(0x4000, 10737 % 0x4000), 0x4000, 0xC000, 0x2000 })

-- ==================== build：全列配置 ====================
words = {}
expect_ok("build 440Hz/square/90°", function()
	return build({ fd = FD, freq = "440", wave = "square", phase = "90" })
end)
-- tw(440)=4724 -> 0x5274/0x4000；相位 90° = 1024 -> 0xC400；square = OPBITEN|DIV2 (0x28)
seq_is("方波初始化序列", { 0x2100, bitc.bor(0x4000, 4724 % 0x4000), 0x4000, 0xC400, bitc.bor(0x2000, 0x0028) })

words = {}
expect_ok("build triangle", function()
	return build({ fd = FD, wave = "Triangle" })
end)
seq_is("三角波初始化序列（大小写不敏感）", { 0x2100, bitc.bor(0x4000, 10737 % 0x4000), 0x4000, 0xC000, bitc.bor(0x2000, 0x0002) })

-- ==================== build：参数校验 ====================
words = {}
expect_err("build freq=0", function() return build({ fd = FD, freq = "0" }) end, "Invalid freq")
expect_err("build freq 超上限", function() return build({ fd = FD, freq = "20000000" }) end, "Invalid freq")
expect_err("build wave 非法", function() return build({ fd = FD, wave = "bogus" }) end, "Invalid wave")
expect_err("build phase=450", function() return build({ fd = FD, phase = "450" }) end, "Invalid phase")
check(#words == 0, "参数校验失败不写任何字", words_hex())

expect_err("build 缺 fd", function() return build({ address = "0x38" }) end, "Invalid Argument: fd")

-- ==================== call：频率运行中变更 ====================
words = {}
expect_ok("call freq='44000'", function() return call({ fd = FD, freq = "44000" }) end)
-- tw(44000)=472446=0x7357E：控制字 B28 -> 0x757E -> 0x401C（低 14 位在前）；
-- 前面 build triangle 已切模块状态，控制字保持三角波位
seq_is("频率变更三字序列（低 14 位在前）", { bitc.bor(0x2000, 0x0002), bitc.bor(0x4000, 0x357E), bitc.bor(0x4000, 0x001C) })

words = {}
local res = expect_ok("call freq 回显", function() return call({ fd = FD }) end)
if res then
	check(res.freq == 44000 and res.wave == "triangle" and res.phase == 0 and res.mute == 0,
		"状态回显", string.format("%s/%s/%s/%s", tostring(res.freq), tostring(res.wave),
		tostring(res.phase), tostring(res.mute)))
	check(#words == 0, "无键 call 不发字", words_hex())
end

-- ==================== call：波形/相位/静音 ====================
words = {}
expect_ok("call wave='triangle'", function() return call({ fd = FD, wave = "triangle" }) end)
seq_is("波形切换单字（三角 = 0x2002）", { bitc.bor(0x2000, 0x0002) })

words = {}
expect_ok("call mute='1'", function() return call({ fd = FD, mute = "1" }) end)
seq_is("静音 = RESET 置位（三角波形保持）", { 0x2102 })

words = {}
expect_ok("call freq（静音态）", function() return call({ fd = FD, freq = "2000" }) end)
-- tw(2000)=21474=0x53E2；静音态控制字 = B28|wave|RESET
seq_is("静音态频率变更保持 RESET", { bitc.bor(0x2000, 0x0002, 0x0100), bitc.bor(0x4000, 0x13E3), bitc.bor(0x4000, 0x0001) })

words = {}
expect_ok("call mute='0' 放行", function() return call({ fd = FD, mute = "0" }) end)
seq_is("放行清 RESET（三角波形保持）", { 0x2002 })

words = {}
expect_ok("call phase='180'", function() return call({ fd = FD, phase = "180" }) end)
seq_is("相位字 180° = 0xC800", { bitc.bor(0xC000, 2048) })

expect_err("call freq 超上限", function() return call({ fd = FD, freq = "-1" }) end, "Invalid freq")
expect_err("call wave 非法", function() return call({ fd = FD, wave = "saw" }) end, "Invalid wave")
expect_err("call 缺 fd", function() return call({ freq = "1000" }) end, "Invalid Argument: fd")

-- ==================== call：传输失败 ====================
nak = true
words = {}
expect_err("call SPI 传输失败", function() return call({ fd = FD, wave = "sine" }) end, "Failed to send")
nak = false

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
