-- ADS1115.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_ads1115.lua（ADS1115 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ads1015.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按 16 位寄存器文件 + 转换状态机建模：
--   - POR 默认配置 0x8583（±2.048V / 单次 / 128SPS，数据手册值）
--   - 单次写 OS=1 启动转换：busy 至精确 1/SPS 周期（独立于驱动 1ms 轮询），
--     完成后转换寄存器装载结果
--   - OS 位读语义按真实硬件建模（datasheet 9.6.3）：转换中读 0、空闲读 1
--     （写入的 OS=1 是启动命令，不落存储；读取时按 busy 状态动态合成）
--   - 转换寄存器为【16 位直接编码】（区别 ADS1015 的 12 位左对齐）
--
-- 与 ADS1015 驱动的行为差异（测试同步覆盖）：
--   - build 读回不做比对（仅判读失败），无 echo mismatch 用例
--   - destroy 缺 fd/addr 返回 ERR（严格契约；ADS1015 为宽松 OK）
--   - destroy 为读改写置 MODE_SINGLE + 清 OS 位（0.3.2 修复：原实现写
--     0x0000 = 连续转换 + 比较器使能，与「停止」意图相反）

dofile("modules/ADS1115.lua")

local bitc = dofile("modules/bitcompat.lua")

-- ==================== 器件模型 ====================
-- regs[0x01] 存储不含 OS 位（低 15 位）；读取时空闲合成 OS=1（POR 读值 0x8583）
local regs = { [0x00] = 0, [0x01] = 0x0583, [0x02] = 0x8000, [0x03] = 0x7FFF }
local conv_raw16 = 0			-- 16 位有符号测量值（测试注入）
local busy_until = nil			-- 转换完成时刻（fake 时钟 ms），nil = 空闲
local stuck = false				-- 模拟器件卡死（超时用例）
local now_ms = 0
local wr_log = {}
local wr_nak = false
local rd_nak = false
local sa_fail = false			-- 模拟 set_addr 失败（destroy 严格契约用例）

-- 精确转换周期 1000/SPS（ADS1115 数据率表 {8,16,32,64,128,250,475,860}），
-- 与驱动的 1ms 轮询不耦合。
-- 注意：dr=0（8SPS）转换需 125ms，超过驱动 100ms 轮询预算——真实硬件上
-- 该档会误报 Conversion timeout（已知驱动限制，暂不入用例以免固化缺陷）。
local conv_ms = { [0]=125, [1]=62.5, [2]=31.25, [3]=15.625,
                  [4]=7.8125, [5]=4, [6]=2.1053, [7]=1.1628 }

local function dr_of(cfg) return math.floor(cfg / 32) % 8 end

-- 16 位有符号码值 -> 16 位无符号存储值（独立编码，检验驱动符号扩展解码）
local function encode16(v)
	return v % 65536
end

local function advance()
	if busy_until ~= nil and not stuck and now_ms >= busy_until then
		busy_until = nil						-- 转空闲（OS 读值由读函数合成为 1）
		regs[0x00] = encode16(conv_raw16)
	end
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr)
	if sa_fail then return false end
	return true
end

function i2c_write_multi(fd, reg, ...)
	if wr_nak then return -1 end
	local bytes = { ... }
	local v = 0
	for _, b in ipairs(bytes) do v = v * 256 + (math.floor(b) % 256) end
	wr_log[#wr_log + 1] = { reg = reg, val = v, n = #bytes }
	if reg == 0x01 then
		regs[0x01] = v % 0x8000						-- OS 是启动命令位，不落存储
		if math.floor(v / 0x8000) % 2 == 1 then		-- OS=1：启动转换
			if math.floor(v / 0x0100) % 2 == 1 then	-- 单次模式
				busy_until = now_ms + conv_ms[dr_of(v)]
			else										-- 连续模式：立即出数
				busy_until = nil
				regs[0x00] = encode16(conv_raw16)
			end
		end
	else
		regs[reg] = v
	end
	return 0
end

function i2c_read_multi(fd, reg, n)
	if rd_nak then return -1 end
	assert(n == 2, "only 16-bit reads expected")
	assert(reg ~= nil and reg >= 0 and reg <= 3, "unexpected register " .. tostring(reg))
	advance()
	local v = regs[reg] or 0
	if reg == 0x01 and busy_until == nil then
		v = v % 0x8000 + 0x8000		-- 空闲：OS 读 1（转换中保持 0）
	end
	return math.floor(v / 256) % 256 * 256 + v % 256		-- 经大端字节路径合并
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

local FD, ADDR = 3, 0x48
local function reset()
	regs = { [0x00] = 0, [0x01] = 0x0583, [0x02] = 0x8000, [0x03] = 0x7FFF }
	conv_raw16 = 0
	busy_until = nil
	stuck = false
	now_ms = 0
	wr_log = {}
	wr_nak = false
	rd_nak = false
	sa_fail = false
end

local function last_wr() return wr_log[#wr_log] end

-- ==================== build：配置编码 ====================
reset()
expect_ok("build 默认（AIN0/±2.048V/128SPS）", function() return build({ fd = FD, address = "0x48" }) end)
check(#wr_log == 1 and last_wr().reg == 0x01 and last_wr().val == 0x4583,
	"配置字 0x4583（单端AIN0|±2.048V|单次|128SPS|禁比较器）",
	last_wr() and string.format("0x%04X", last_wr().val))

reset()
expect_ok("build 自定义（AIN3/±0.256V/860SPS）", function()
	return build({ fd = FD, address = "0x48", pga = "5", dr = "7", channel = "3" })
end)
check(last_wr().val == 0x7BE3, "配置字 0x7BE3（AIN3|±0.256V|单次|860SPS|禁比较器）",
	last_wr() and string.format("0x%04X", last_wr().val))

-- ==================== build：错误路径 ====================
reset()
wr_nak = true
expect_err("build 写配置无应答", function() return build({ fd = FD, address = "0x48" }) end, "Failed to write")
wr_nak = false

reset()
rd_nak = true
expect_err("build 回读无应答", function() return build({ fd = FD, address = "0x48" }) end, "Failed to read back")
rd_nak = false

reset()
sa_fail = true
expect_err("build set_addr 失败", function() return build({ fd = FD, address = "0x48" }) end, "set_addr failed")
sa_fail = false

reset()
expect_err("build 缺 fd", function() return build({ address = "0x48" }) end, "Invalid Argument: fd")
expect_err("build 非表参数", function() return build("x") end, "Invalid Arguments Type")

-- ==================== call：单端读取与 16 位符号扩展 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
conv_raw16 = 32767
local res = expect_ok("call 单端正满码", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(res.raw == 32767, "raw=32767（16 位直接解码）", tostring(res.raw))
	check(near(res.voltage, 32767 * 2.048 / 32768), "voltage = 2.0479V", tostring(res.voltage))
	check(last_wr().val == 0xC583, "call 重写配置启动转换（OS=1|AIN0）",
		last_wr() and string.format("0x%04X", last_wr().val))
	check(now_ms >= 7.8125, "轮询至转换完成", tostring(now_ms))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
conv_raw16 = -32768
res = expect_ok("call 负满码", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(res.raw == -32768, "raw=-32768（sign16 符号扩展）", tostring(res.raw))
	check(near(res.voltage, -2.048), "voltage = -2.048V", tostring(res.voltage))
end

reset()
expect_ok("call 前 build（channel=2）", function() return build({ fd = FD, address = "0x48", channel = "2" }) end)
conv_raw16 = 100
res = expect_ok("call 通道 2（call 参数切换 MUX）", function() return call({ fd = FD, address = "0x48", channel = "2" }) end)
if res then
	check(res.raw == 100, "raw=100", tostring(res.raw))
	check(last_wr().val == 0xE583, "MUX 切到 AIN2（0xE583）",
		last_wr() and string.format("0x%04X", last_wr().val))
end

-- ==================== call：PGA 缺省回读芯片配置 ====================
reset()
expect_ok("call 前 build（±4.096V）", function() return build({ fd = FD, address = "0x48", pga = "1" }) end)
conv_raw16 = 16000
res = expect_ok("call 无 pga 参 -> 回读芯片配置换算", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(near(res.voltage, 16000 * 4.096 / 32768), "voltage = 2.0V（fs=±4.096V）", tostring(res.voltage))
end

-- ==================== call：差分 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
conv_raw16 = -300
res = expect_ok("call 差分 0-1", function() return call({ fd = FD, address = "0x48", pos = 0, neg = 1 }) end)
if res then
	check(res.raw == -300, "raw=-300", tostring(res.raw))
	check(last_wr().val == 0x8583, "MUX=差分0-1（0x8583）",
		last_wr() and string.format("0x%04X", last_wr().val))
end

res = expect_ok("call 差分 2-3", function() return call({ fd = FD, address = "0x48", pos = 2, neg = 3 }) end)
if res then
	check(last_wr().val == 0xB583, "MUX=差分2-3（0xB583）",
		last_wr() and string.format("0x%04X", last_wr().val))
end

expect_err("call 不支持的差分对 1-2", function()
	return call({ fd = FD, address = "0x48", pos = 1, neg = 2 })
end, "Unsupported diff")
expect_err("call 差分参数非数字", function()
	return call({ fd = FD, address = "0x48", pos = "-", neg = 1 })
end, "not a number")
expect_err("call 非法通道", function() return call({ fd = FD, address = "0x48", channel = "4" }) end, "Invalid channel")

-- ==================== call：慢速档与超时 ====================
reset()
expect_ok("call 前 build（16SPS 慢档）", function() return build({ fd = FD, address = "0x48", dr = "1" }) end)
conv_raw16 = 5
res = expect_ok("call 16SPS 档等待转换", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(res.raw == 5, "raw=5", tostring(res.raw))
	check(now_ms >= 62.5, "轮询至 62.5ms 转换完成", tostring(now_ms))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
stuck = true
expect_err("call 转换超时", function() return call({ fd = FD, address = "0x48" }) end, "Conversion timeout")
check(now_ms >= 100, "超时前轮询 100ms", tostring(now_ms))
stuck = false

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
rd_nak = true
expect_err("call 读配置无应答", function() return call({ fd = FD, address = "0x48" }) end, "Failed to read config")
rd_nak = false
expect_err("call 非表参数", function() return call(11) end, "Invalid Arguments")
expect_err("call 缺 address", function() return call({ fd = FD }) end, "Invalid Argument: addr")

-- ==================== destroy（0.3.2 修复回归守护） ====================
reset()
regs[0x01] = 0x4483			-- 模拟连续模式配置（MODE=0）
expect_ok("destroy 置单次断电态", function() return destroy({ fd = FD, address = "0x48" }) end)
check(last_wr() ~= nil and last_wr().reg == 0x01 and last_wr().val == 0x4583,
	"读改写置 MODE=1，保留 PGA/DR/MUX（0x4483 -> 0x4583）",
	last_wr() and string.format("0x%04X", last_wr().val))
check(busy_until == nil, "OS 位已清（写回不误启动转换）", tostring(busy_until))

reset()
rd_nak = true
expect_ok("destroy 读配置失败安全返回", function() return destroy({ fd = FD, address = "0x48" }) end)
check(#wr_log == 0, "读失败不发任何写", tostring(#wr_log))
rd_nak = false

-- ADS1115 destroy 保持严格契约（区别 ADS1015 的宽松 OK）
reset()
expect_err("destroy 缺 fd 报 ERR", function() return destroy({ address = "0x48" }) end, "Invalid Arguments: fd")
expect_err("destroy 缺 addr 报 ERR", function() return destroy({ fd = FD }) end, "Invalid Arguments: addr")
check(#wr_log == 0, "缺参不发任何写", tostring(#wr_log))

reset()
sa_fail = true
expect_err("destroy set_addr 失败报 ERR", function() return destroy({ fd = FD, address = "0x48" }) end, "set_addr failed")
sa_fail = false

reset()
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
