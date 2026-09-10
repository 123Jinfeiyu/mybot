-- ADS1015.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_ads1015.lua（ADS1015 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_sgp30.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按 16 位寄存器文件 + 转换状态机建模：
--   - POR 默认配置 0x8583（±0.256V / 连续 / 1600SPS，数据手册值）
--   - 单次写 OS=1 启动转换：busy 至精确 1/SPS 周期（独立于驱动 1ms 轮询），
--     完成后转换寄存器装载结果
--   - OS 位读语义按真实硬件建模（datasheet 8.6.3）：转换中读 0、空闲读 1
--     （写入的 OS=1 是启动命令，不落存储；读取时按 busy 状态动态合成）
--   - 转换寄存器按【12 位左对齐】编码（码值 << 4），独立于驱动右移解码
--   - 连续模式（MODE=0）写后立即出数

dofile("modules/ADS1015.lua")

local bitc = dofile("modules/bitcompat.lua")

-- ==================== 器件模型 ====================
-- regs[0x01] 存储不含 OS 位（低 15 位）；读取时空闲合成 OS=1（POR 读值 0x8583）
local regs = { [0x00] = 0, [0x01] = 0x0583, [0x02] = 0x8000, [0x03] = 0x7FFF }
local conv_raw12 = 0			-- 12 位有符号测量值（测试注入）
local busy_until = nil			-- 转换完成时刻（fake 时钟 ms），nil = 空闲
local stuck = false				-- 模拟器件卡死（超时用例）
local now_ms = 0
local wr_log = {}
local wr_nak = false
local rd_nak = false
local corrupt = false

-- 精确转换周期 1000/SPS（内核 ti-ads1015 数据率表），与驱动的 1ms 轮询不耦合
local conv_ms = { [0]=7.8125, [1]=4, [2]=2.0408, [3]=1.0870,
                  [4]=0.625, [5]=0.4167, [6]=0.3030, [7]=0.3030 }

local function dr_of(cfg) return math.floor(cfg / 32) % 8 end

-- 12 位有符号码值 -> 16 位左对齐存储值（独立编码，检验驱动右移解码）
local function encode12(v)
	return (v % 4096) * 16
end

local function advance()
	if busy_until ~= nil and not stuck and now_ms >= busy_until then
		busy_until = nil						-- 转空闲（OS 读值由读函数合成为 1）
		regs[0x00] = encode12(conv_raw12)
	end
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

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
				regs[0x00] = encode12(conv_raw12)
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
	if corrupt and reg == 0x01 then v = bitc.bxor(v, 0x0080) end
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
	conv_raw12 = 0
	busy_until = nil
	stuck = false
	now_ms = 0
	wr_log = {}
	wr_nak = false
	rd_nak = false
	corrupt = false
end

local function last_wr() return wr_log[#wr_log] end

-- ==================== build：配置编码与回显自检 ====================
reset()
expect_ok("build 默认（AIN0/±2.048V/1600SPS）", function() return build({ fd = FD, address = "0x48" }) end)
check(#wr_log == 1 and last_wr().reg == 0x01 and last_wr().val == 0x4583,
	"配置字 0x4583（单端AIN0|±2.048V|单次|1600SPS|禁比较器）",
	last_wr() and string.format("0x%04X", last_wr().val))

reset()
expect_ok("build 自定义（AIN3/±0.256V/3300SPS）", function()
	return build({ fd = FD, address = "0x48", pga = "5", dr = "7", channel = "3" })
end)
check(last_wr().val == 0x7BE3, "配置字 0x7BE3（AIN3|±0.256V|单次|3300SPS|禁比较器）",
	last_wr() and string.format("0x%04X", last_wr().val))

-- ==================== build：错误路径 ====================
reset()
wr_nak = true
expect_err("build 写配置无应答", function() return build({ fd = FD, address = "0x48" }) end, "Failed to write")
wr_nak = false

reset()
rd_nak = true
expect_err("build 回显读无应答", function() return build({ fd = FD, address = "0x48" }) end, "Failed to read back")
rd_nak = false

reset()
corrupt = true
expect_err("build 回显不匹配", function() return build({ fd = FD, address = "0x48" }) end, "echo mismatch")
corrupt = false

reset()
expect_err("build 缺 fd", function() return build({ address = "0x48" }) end, "Invalid Argument: fd")
expect_err("build 非表参数", function() return build("x") end, "Invalid Arguments Type")

-- ==================== call：单端读取与 12 位左对齐解码 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
conv_raw12 = 2047
local res = expect_ok("call 单端正满码", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(res.raw == 2047, "raw=2047（右移 4 位解码）", tostring(res.raw))
	check(near(res.voltage, 2047 * 2.048 / 2048), "voltage = 2.046V", tostring(res.voltage))
	check(last_wr().val == 0xC583, "call 重写配置启动转换（OS=1|AIN0）",
		last_wr() and string.format("0x%04X", last_wr().val))
	check(now_ms >= 0.625, "轮询至转换完成", tostring(now_ms))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
conv_raw12 = -2048
res = expect_ok("call 负满码", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(res.raw == -2048, "raw=-2048（符号经左对齐位保留）", tostring(res.raw))
	check(near(res.voltage, -2.048), "voltage = -2.048V", tostring(res.voltage))
end

reset()
expect_ok("call 前 build（channel=2）", function() return build({ fd = FD, address = "0x48", channel = "2" }) end)
conv_raw12 = 100
res = expect_ok("call 通道 2（call 参数切换 MUX）", function() return call({ fd = FD, address = "0x48", channel = "2" }) end)
if res then
	check(res.raw == 100, "raw=100", tostring(res.raw))
	check(last_wr().val == 0xE583, "MUX 切到 AIN2（0xE583）",
		last_wr() and string.format("0x%04X", last_wr().val))
end

-- ==================== call：PGA 缺省回读芯片配置 ====================
reset()
expect_ok("call 前 build（±4.096V）", function() return build({ fd = FD, address = "0x48", pga = "1" }) end)
conv_raw12 = 1000
res = expect_ok("call 无 pga 参 -> 回读芯片配置换算", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(near(res.voltage, 1000 * 4.096 / 2048), "voltage = 2.0V（fs=±4.096V）", tostring(res.voltage))
end

-- 0.3.2 修复回归：PGA 字段 6/7 与 5 同为 ±0.256V（datasheet 冗余编码），
-- 原实现读回后重置为 2（±2.048V）换算错 8 倍
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
regs[0x01] = 0x4D83			-- 模拟芯片 PGA 字段=6（0x4583 的 pga 位改 110）
conv_raw12 = 1000
res = expect_ok("call PGA=6 冗余编码", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(near(res.voltage, 1000 * 0.256 / 2048), "voltage 按 ±0.256V 换算（不再错 8 倍）", tostring(res.voltage))
end

-- ==================== call：差分 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x48" }) end)
conv_raw12 = -300
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
expect_err("call 非法通道", function() return call({ fd = FD, address = "0x48", channel = "4" }) end, "Invalid channel")

-- ==================== call：慢速档与超时 ====================
reset()
expect_ok("call 前 build（128SPS 最慢档）", function() return build({ fd = FD, address = "0x48", dr = "0" }) end)
conv_raw12 = 5
res = expect_ok("call 128SPS 档等待转换", function() return call({ fd = FD, address = "0x48" }) end)
if res then
	check(res.raw == 5, "raw=5", tostring(res.raw))
	check(now_ms >= 7.8125, "轮询至 7.8ms 转换完成", tostring(now_ms))
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

-- ==================== destroy ====================
reset()
expect_ok("destroy 前 build", function() return build({ fd = FD, address = "0x48" }) end)
reset()
regs[0x01] = 0x4483			-- 模拟连续模式配置（MODE=0）
expect_ok("destroy 置单次断电态", function() return destroy({ fd = FD, address = "0x48" }) end)
check(last_wr() ~= nil and last_wr().reg == 0x01 and last_wr().val == 0x4583,
	"读改写置 MODE=1，保留 PGA/DR/MUX（0x4483 -> 0x4583）",
	last_wr() and string.format("0x%04X", last_wr().val))

reset()
expect_ok("destroy 读失败安全返回", function() return destroy({ fd = FD, address = "0x48" }) end)
rd_nak = false

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
