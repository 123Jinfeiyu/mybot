-- MCP3421.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_mcp3421.lua（MCP3421 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_sgp30.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按「配置寄存器 + 转换状态机」建模：
--   - 写单字节配置：RDY=1 启动转换（busy 至 1/SPS 周期），RDY=0 仅改配置
--   - 读：未就绪回显 RDY=1 与上一次结果；就绪回显 RDY=0 与新结果；
--     消费后连续模式立即开始下一次转换（真实芯片时序）
--   - 各分辨率档编码用独立公式：18 位右对齐取低 2 位；14/12 位按数据手册
--     顶部重复符号位填充（int16 解释），与驱动解码互为交叉验证
--   - 转换周期用精确值 4.2/16.7/66.7/266.7ms（驱动用向上取整 + 余量，互不耦合）

dofile("modules/MCP3421.lua")

-- ==================== 器件模型 ====================
-- 配置位域（与数据手册一致，模型独立编码）
local B_RDY, B_CONT, M_RATE, M_GAIN = 0x80, 0x10, 0x0C, 0x03
local M_CONV_MS = { [12]=4.2, [14]=16.7, [16]=66.7, [18]=266.7 }	-- 精确 1/SPS

local m_cfg = 0x90			-- 上电默认：连续/240SPS/×1/RDY=1
local busy_until = nil		-- 转换完成时刻（fake 时钟 ms），nil = 空闲
local has_fresh = false		-- 有未被读走的新结果
local result_raw = 0		-- 模型测量的有符号码值（测试注入）
local now_ms = 0
local wr_log = {}
local wr_nak = false
local rd_nak = false
local corrupt_echo = false

local function model_res()
	local s = math.floor((m_cfg % 16) / 4) % 4	-- S1S0（低 4 位 = 速率+增益）
	return ({ [0]=12, [1]=14, [2]=16, [3]=18 })[s]
end

local function model_cont()
	return math.floor(m_cfg / 16) % 2 == 1						-- O/C
end

-- 有符号码值 -> 各分辨率档的原始字节（独立于驱动解码的编码公式）
local function encode_bytes(n)
	local res = model_res()
	local u, hi, lo
	if res == 18 then
		u = result_raw % 0x40000
		hi = math.floor(u / 65536) % 4			-- D[17:16] 在首字节低 2 位
		lo = { math.floor(u / 256) % 256, u % 256 }
	elseif res == 16 then
		u = result_raw % 0x10000
		hi = math.floor(u / 256) % 256
		lo = { u % 256 }
	elseif res == 14 then
		u = result_raw % 0x4000
		local w = (u >= 0x2000) and (u + 0xC000) or u	-- 顶部 2 位重复符号
		hi = math.floor(w / 256) % 256
		lo = { w % 256 }
	else	-- 12
		u = result_raw % 0x1000
		local w = (u >= 0x800) and (u + 0xF000) or u	-- 顶部 4 位重复符号
		hi = math.floor(w / 256) % 256
		lo = { w % 256 }
	end
	local bytes = { hi }
	for _, b in ipairs(lo) do bytes[#bytes + 1] = b end
	return bytes
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

function i2c_write_multi(fd, b)
	if wr_nak then return -1 end
	b = math.floor(b) % 256
	wr_log[#wr_log + 1] = b
	m_cfg = b
	if math.floor(b / 128) % 2 == 1 then			-- RDY=1：启动转换
		busy_until = now_ms + M_CONV_MS[model_res()]
		has_fresh = false
	end
	return 0
end

function i2c_read(fd, n)
	if rd_nak then return -1 end
	local res = model_res()
	local exp_n = (res == 18) and 4 or 3
	assert(n == exp_n, "read byte count " .. tostring(n) .. " ~= " .. tostring(exp_n))
	if busy_until ~= nil and now_ms >= busy_until then
		busy_until = nil
		has_fresh = true
	end
	local echo
	if has_fresh then
		echo = m_cfg % 128								-- RDY=0：新数据就绪
		if model_cont() then busy_until = now_ms + M_CONV_MS[res] end
		has_fresh = false								-- 被主设备读走
	else
		echo = m_cfg									-- RDY=1：转换中/已读走
	end
	if corrupt_echo then echo = (echo + 1) % 256 end
	local bytes = encode_bytes()
	bytes[#bytes + 1] = echo
	assert(#bytes == n, "model byte count mismatch")
	local v = 0
	for _, b in ipairs(bytes) do v = v * 256 + b end
	return v
end

function milli_sleep(ms) now_ms = now_ms + ms end

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

local function near(a, b, eps)
	return math.abs(a - b) <= (eps or 1e-9)
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

local FD, ADDR = 3, 0x68
local function reset()
	wr_log = {}
	busy_until = nil
	has_fresh = false
	result_raw = 0
	now_ms = 0
	m_cfg = 0x90
	wr_nak = false
	rd_nak = false
	corrupt_echo = false
end

local function last_wr() return wr_log[#wr_log] end

-- ==================== build：配置编码与回显校验 ====================
reset()
expect_ok("build 默认（18bit/×1/cont）", function() return build({ fd = FD, address = "0x68" }) end)
check(#wr_log == 1 and last_wr() == 0x9C, "配置字节 0x9C（RDY|cont|18bit|×1）",
	last_wr() and string.format("0x%02X", last_wr()))

reset()
expect_ok("build 16bit/×8/oneshot", function()
	return build({ fd = FD, address = "0x68", res = "16", pga = "8", opmode = "oneshot" })
end)
check(last_wr() == 0x8B, "配置字节 0x8B（RDY|16bit|×8）", string.format("0x%02X", last_wr()))

reset()
expect_ok("build 14bit 默认 cont", function() return build({ fd = FD, address = "0x68", res = "14" }) end)
check(last_wr() == 0x94, "配置字节 0x94（RDY|cont|14bit|×1）", string.format("0x%02X", last_wr()))

reset()
expect_ok("build 12bit/×2", function() return build({ fd = FD, address = "0x68", res = "12", pga = "2" }) end)
check(last_wr() == 0x91, "配置字节 0x91（RDY|cont|12bit|×2）", string.format("0x%02X", last_wr()))

-- ==================== build：错误路径 ====================
reset()
expect_err("build 非法 res", function() return build({ fd = FD, address = "0x68", res = "13" }) end, "res(bits)")
expect_err("build 非法 pga", function() return build({ fd = FD, address = "0x68", pga = "3" }) end, "pga(gain)")
expect_err("build 非法 opmode", function() return build({ fd = FD, address = "0x68", opmode = "x" }) end, "opmode")
expect_err("build 缺 fd", function() return build({ address = "0x68" }) end, "fd or address")
expect_err("build 非表参数", function() return build(7) end, "Invalid Arguments Type")

reset()
wr_nak = true
expect_err("build 写配置无应答", function() return build({ fd = FD, address = "0x68" }) end, "config write failed")
wr_nak = false

reset()
rd_nak = true
expect_err("build 回显读无应答", function() return build({ fd = FD, address = "0x68" }) end, "echo read failed")
rd_nak = false

reset()
corrupt_echo = true
expect_err("build 回显不匹配", function() return build({ fd = FD, address = "0x68" }) end, "echo mismatch")
corrupt_echo = false

-- ==================== call：连续模式与电压换算 ====================
reset()
expect_ok("call 前 build（18bit/×1）", function() return build({ fd = FD, address = "0x68" }) end)
result_raw = 1000
local res = expect_ok("call 等待首次转换完成", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == 1000, "raw=1000", tostring(res.raw))
	check(near(res.voltage, 1000 * 2.048 / 131072), "voltage = 15.625uV/LSB", tostring(res.voltage))
	check(now_ms >= 266.7, "轮询至转换完成（fake 时钟推进）", tostring(now_ms))
	check(#wr_log == 1, "连续模式 call 不重写配置", tostring(#wr_log))
end

res = expect_ok("call 二次取数（续转换）", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == 1000, "raw=1000（第二次）", tostring(res.raw))
end

reset()
expect_ok("call 前 build（×8 满量程）", function() return build({ fd = FD, address = "0x68", pga = "8" }) end)
result_raw = 131071
res = expect_ok("call ×8 正满码", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == 131071, "raw=131071", tostring(res.raw))
	check(near(res.voltage, 131071 * 2.048 / 131072 / 8), "voltage ≈ +0.256V", tostring(res.voltage))
end

result_raw = -131072
res = expect_ok("call ×8 负满码", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == -131072, "raw=-131072", tostring(res.raw))
	check(near(res.voltage, -0.256), "voltage = -0.256V", tostring(res.voltage))
end

reset()
expect_ok("call 前 build（18bit/×1）", function() return build({ fd = FD, address = "0x68" }) end)
result_raw = -1000
res = expect_ok("call 18 位负码值", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == -1000 and near(res.voltage, -1000 * 2.048 / 131072), "raw=-1000 符号扩展正确",
		string.format("%s/%s", tostring(res.raw), tostring(res.voltage)))
end

-- ==================== call：低分辨率档（符号位重复填充） ====================
reset()
expect_ok("call 前 build（16bit）", function() return build({ fd = FD, address = "0x68", res = "16" }) end)
result_raw = -20000
res = expect_ok("call 16 位负码值", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == -20000 and near(res.voltage, -20000 * 2.048 / 32768), "raw=-20000，voltage=-1.25V",
		string.format("%s/%s", tostring(res.raw), tostring(res.voltage)))
end

reset()
expect_ok("call 前 build（14bit）", function() return build({ fd = FD, address = "0x68", res = "14" }) end)
result_raw = 5000
res = expect_ok("call 14 位正码值", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == 5000 and near(res.voltage, 5000 * 2.048 / 8192), "raw=5000，voltage=1.25V",
		string.format("%s/%s", tostring(res.raw), tostring(res.voltage)))
end
result_raw = -4000
res = expect_ok("call 14 位负码值（顶部重复符号）", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == -4000, "raw=-4000", tostring(res.raw))
end

reset()
expect_ok("call 前 build（12bit/×2）", function() return build({ fd = FD, address = "0x68", res = "12", pga = "2" }) end)
result_raw = -100
res = expect_ok("call 12 位负码值（×2 增益换算）", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == -100, "raw=-100", tostring(res.raw))
	check(near(res.voltage, -100 * 2.048 / 2048 / 2), "voltage = -0.05V", tostring(res.voltage))
end

-- ==================== call：单次模式与未就绪 ====================
reset()
expect_ok("build oneshot", function() return build({ fd = FD, address = "0x68", opmode = "oneshot" }) end)
local wr_before = #wr_log
result_raw = 42
res = expect_ok("call 单次模式触发转换", function() return call({ fd = FD, address = "0x68" }) end)
if res then
	check(res.raw == 42, "raw=42", tostring(res.raw))
	check(#wr_log == wr_before + 1, "oneshot 每次 call 重写触发", string.format("%d -> %d", wr_before, #wr_log))
	check(last_wr() == 0x8C, "触发字节 0x8C（RDY|18bit|×1）", string.format("0x%02X", last_wr()))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x68" }) end)
busy_until = now_ms + 1e9		-- 模拟器件卡死在转换中
expect_err("call 未就绪超时（默认 wait）", function() return call({ fd = FD, address = "0x68" }) end, "not ready")
check(now_ms >= 287, "默认等待 267+20ms", tostring(now_ms))

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x68" }) end)
busy_until = now_ms + 1e9
expect_err("call 未就绪超时（wait=50 覆盖）", function()
	return call({ fd = FD, address = "0x68", wait = "50" })
end, "not ready")

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x68" }) end)
rd_nak = true
expect_err("call 读无应答", function() return call({ fd = FD, address = "0x68" }) end, "read failed")
rd_nak = false

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x68" }) end)
expect_err("call 非表参数", function() return call("x") end, "Invalid Arguments")
expect_err("call 缺 address", function() return call({ fd = FD }) end, "fd or address")

-- ==================== destroy ====================
reset()
expect_ok("destroy 前 build", function() return build({ fd = FD, address = "0x68" }) end)
reset()		-- 清写记录，destroy 的写单独看
expect_ok("destroy 置待机", function() return destroy({ fd = FD, address = "0x68" }) end)
check(#wr_log == 1 and last_wr() == 0x0C, "destroy 写 0x0C（RDY=0/O/C=0，保留速率增益）",
	last_wr() and string.format("0x%02X", last_wr()))
check(math.floor(last_wr() / 128) % 2 == 0 and math.floor(last_wr() / 16) % 2 == 0,
	"RDY 与 O/C 位均为 0")

reset()
expect_ok("destroy 缺 fd 安全返回", function() return destroy({}) end)
check(#wr_log == 0, "缺 fd 不发任何写", tostring(#wr_log))

reset()
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
