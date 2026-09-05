-- SGP30.lua 离线测试（桩替身，不触碰真实 I2C 与文件系统）
-- 用法: luajit test_sgp30.lua（SGP30 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ina226.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局与 HAL.sys.*（test_hal_ffi.lua 同款间接层注入）。
-- 器件按命令式协议建模：记录写命令序列，按最后命令回对应应答；
-- 基线文件落在内存表；CRC 用独立实现重算，与驱动互为交叉验证。

dofile("modules/SGP30.lua")

local bitc = dofile("modules/bitcompat.lua")
local HALT = dofile("modules/lib/HAL.lua")		-- 守卫返回 _G.__BOT4CJ_HAL，同一张表

-- ==================== 独立 CRC8（多项式 0x31，初值 0xFF） ====================
local function crc8(word)
	local crc = 0xFF
	local hi = math.floor(word / 256) % 256
	local lo = word % 256
	for _, b in ipairs({hi, lo}) do
		crc = bitc.bxor(crc, b)
		for _ = 1, 8 do
			if bitc.band(crc, 0x80) ~= 0 then
				crc = bitc.band(bitc.bxor(bitc.lshift(crc, 1), 0x31), 0xFF)
			else
				crc = bitc.band(bitc.lshift(crc, 1), 0xFF)
			end
		end
	end
	return crc
end

local function ah_ticks_of(t, rh)	-- Sensirion 公式独立重算（交叉验证驱动实现）
	local ah = 216.7 * (rh / 100.0 * 6.112 * math.exp(17.62 * t / (243.12 + t)) / (273.15 + t))
	return math.floor(ah * 256 + 0.5)
end

-- ==================== 命令式器件模型 ====================
local words = {
	selftest = 0xD400,
	co2  = 400,
	tvoc = 0,
	base1 = 0x1234,
	base2 = 0x5678,
}
local bad_crc = { measure = false, baseline = false, selftest = false }
local wr_cmds = {}		-- 写命令记录 {cmd = 0x20xx, payload = {...}}
local nak = false			-- 写无应答（在位失败）
local read_fail = false		-- 读失败
local set_addr_fail = false	-- set_addr 失败（纯软件记录，与在位无关）

local function word3(w, corrupt)
	local b = { math.floor(w / 256) % 256, w % 256, crc8(w) }
	if corrupt then b[3] = bitc.bxor(b[3], 0x5A) end
	return b
end

local function resp_bytes(n)
	local c = wr_cmds[#wr_cmds] and wr_cmds[#wr_cmds].cmd or 0
	if c == 0x2008 then
		local w1 = word3(words.co2, bad_crc.measure)
		local w2 = word3(words.tvoc, bad_crc.measure)
		return { w1[1], w1[2], w1[3], w2[1], w2[2], w2[3] }
	end
	if c == 0x2015 then
		local w1 = word3(words.base1, bad_crc.baseline)
		local w2 = word3(words.base2, bad_crc.baseline)
		return { w1[1], w1[2], w1[3], w2[1], w2[2], w2[3] }
	end
	if c == 0x2032 then
		local w = word3(words.selftest, bad_crc.selftest)
		return { w[1], w[2], w[3] }
	end
	return {}
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return not set_addr_fail end

function i2c_write_reg(fd, hi, lo)
	if nak then return false end
	wr_cmds[#wr_cmds + 1] = { cmd = hi * 256 + lo, payload = {} }
	return true
end

function i2c_write_multi(fd, hi, lo, ...)
	if nak then return -1 end
	local payload = {}
	for _, v in ipairs({ ... }) do payload[#payload + 1] = math.floor(v) % 256 end
	wr_cmds[#wr_cmds + 1] = { cmd = hi * 256 + lo, payload = payload }
	return 0
end

function i2c_read(fd, n)
	if read_fail or nak then return -1 end
	local bytes = resp_bytes(n)
	local v = 0
	for i = 0, n - 1 do v = v * 256 + (bytes[i + 1] or 0) end
	return v
end

function milli_sleep(ms) end

-- ==================== HAL.sys 文件 mock（内存文件表） ====================
local files = {}		-- path -> 内容字符串
local next_fd = 10
local fd_of = {}		-- fd -> {path, off}

HALT.sys.open = function(path, flags, mode)
	if flags ~= 0 and not files[path] then files[path] = "" end		-- 写打开视作 O_CREAT
	if files[path] == nil then return -1 end
	next_fd = next_fd + 1
	fd_of[next_fd] = { path = path, off = 0 }
	return next_fd
end

HALT.sys.read = function(fd, buf, n)
	local e = fd_of[fd]
	local s = files[e.path]
	local cnt = math.min(n, #s - e.off)
	for i = 0, cnt - 1 do buf[i] = s:byte(e.off + i + 1) end
	e.off = e.off + cnt
	return cnt
end

HALT.sys.write = function(fd, buf, n)
	local bytes = {}
	for i = 0, n - 1 do bytes[i + 1] = string.char(math.floor(tonumber(buf[i]))) end
	files[fd_of[fd].path] = table.concat(bytes)		-- O_TRUNC 语义：整文件替换
	return n
end

HALT.sys.close = function(fd) fd_of[fd] = nil; return 0 end

-- os.time 覆盖（基线存盘节拍用）
local now_s = 1000000000
os.time = function() return now_s end

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

local function cmd_list()
	local out = {}
	for _, c in ipairs(wr_cmds) do out[#out + 1] = string.format("0x%04X", c.cmd) end
	return table.concat(out, ",")
end

local function cmd_count(cmd)
	local n = 0
	for _, c in ipairs(wr_cmds) do
		if c.cmd == cmd then n = n + 1 end
	end
	return n
end

local function payload_of(cmd)	-- 最后一次该命令的 payload
	local p
	for _, c in ipairs(wr_cmds) do
		if c.cmd == cmd then p = c.payload end
	end
	return p
end

local FD, ADDR = 3, 0x58
local function reset()
	wr_cmds = {}
end

-- ==================== build ====================
reset()
expect_ok("build 默认（无 baseline 列）", function() return build({ fd = FD, address = "0x58" }) end)
check(cmd_list() == "0x2032,0x2003", "build 命令序：自检 -> Init", cmd_list())

reset()
expect_ok("build baseline 文件不存在 -> fresh", function() return build({ fd = FD, address = "0x58", baseline = "/mock/base" }) end)
check(cmd_count(0x201E) == 0, "无文件不恢复基线", cmd_list())

nak = true
reset()
expect_err("build 器件无应答", function() return build({ fd = FD, address = "0x58" }) end, "not responding")
nak = false

reset()
words.selftest = 0x1234
expect_err("build 自检字错误", function() return build({ fd = FD, address = "0x58" }) end, "selftest failed")
words.selftest = 0xD400

reset()
bad_crc.selftest = true
expect_err("build 自检 CRC 坏", function() return build({ fd = FD, address = "0x58" }) end, "selftest failed")
bad_crc.selftest = false

read_fail = true
reset()
expect_err("build 自检读失败", function() return build({ fd = FD, address = "0x58" }) end, "Failed to read")
read_fail = false

reset()
expect_err("build 缺 fd", function() return build({ address = "0x58" }) end, "fd or address")

set_addr_fail = true
reset()
expect_err("build set_addr 失败", function() return build({ fd = FD, address = "0x58" }) end, "set_addr failed")
set_addr_fail = false

-- ==================== call：测量与湿度补偿 ====================
reset()
local res = expect_ok("call 默认测量", function() return call({ fd = FD, address = "0x58" }) end)
if res then
	check(res.CO2EQ == 400 and res.TVOC == 0, "CO2EQ=400 TVOC=0", string.format("%s/%s", tostring(res.CO2EQ), tostring(res.TVOC)))
	check(res.CRC_OK == 1, "CRC_OK=1")
	check(cmd_count(0x2061) == 0, "无补偿输入不下发湿度命令", cmd_list())
end

reset()
local ticks = ah_ticks_of(25.0, 40.0)
res = expect_ok("call 湿度补偿 25C/40%", function()
	return call({ fd = FD, address = "0x58", t_field = "v0", v0 = 25.0, rh_field = "v1", v1 = 40.0 })
end)
if res then
	local p = payload_of(0x2061)
	check(p ~= nil and #p == 3, "下发 Set_absolute_humidity", cmd_list())
	check(p ~= nil and p[1] == math.floor(ticks / 256) % 256 and p[2] == ticks % 256 and p[3] == crc8(ticks),
		"AH ticks 与独立公式一致", p and string.format("%d vs %d", p[1] * 256 + p[2], ticks) or "?")
end

reset()
expect_ok("call 湿度不变不重发", function()
	return call({ fd = FD, address = "0x58", t_field = "v0", v0 = 25.0, rh_field = "v1", v1 = 40.0 })
end)
check(cmd_count(0x2061) == 0, "AH 未变化跳过命令", cmd_list())

reset()
expect_ok("call 湿度变化重发", function()
	return call({ fd = FD, address = "0x58", t_field = "v0", v0 = 30.0, rh_field = "v1", v1 = 40.0 })
end)
check(cmd_count(0x2061) == 1, "AH 变化重新下发", cmd_list())

reset()
bad_crc.measure = true
res = expect_ok("call 应答 CRC 坏 -> 标志位", function() return call({ fd = FD, address = "0x58" }) end)
if res then check(res.CRC_OK == 0, "CRC_OK=0（兼容契约）", tostring(res.CRC_OK)) end
bad_crc.measure = false

read_fail = true
reset()
expect_err("call 读失败", function() return call({ fd = FD, address = "0x58" }) end, "Failed to read")
read_fail = false

nak = true
reset()
expect_err("call 器件无应答", function() return call({ fd = FD, address = "0x58" }) end, "not responding")
nak = false

-- ==================== 基线持久化 ====================
reset()
expect_ok("build 带 baseline 路径", function() return build({ fd = FD, address = "0x58", baseline = "/mock/base" }) end)

now_s = now_s + 3600		-- 触发周期存盘
reset()
expect_ok("call 触发周期存盘", function() return call({ fd = FD, address = "0x58" }) end)
check(cmd_count(0x2015) == 1, "Get_baseline 下发", cmd_list())
local saved = files["/mock/base"]
check(saved ~= nil and #saved == 6, "基线文件 6 字节", saved and tostring(#saved) or "nil")
local w1, w2 = words.base1, words.base2
check(saved ~= nil and saved:byte(1) == math.floor(w1 / 256) and saved:byte(2) == w1 % 256
	and saved:byte(3) == crc8(w1) and saved:byte(4) == math.floor(w2 / 256)
	and saved:byte(5) == w2 % 256 and saved:byte(6) == crc8(w2),
	"文件内容 = word+crc, word+crc")

now_s = now_s + 60		-- 未到存盘周期
reset()
expect_ok("call 未到周期不存盘", function() return call({ fd = FD, address = "0x58" }) end)
check(cmd_count(0x2015) == 0, "60s 内不再 Get_baseline", cmd_list())

reset()
expect_ok("destroy 再存一次基线", function() return destroy({ fd = FD, address = "0x58" }) end)
check(cmd_count(0x2015) == 1, "destroy 触发 Get_baseline", cmd_list())

-- 重新 build：应从文件恢复基线
reset()
expect_ok("重建后恢复基线", function() return build({ fd = FD, address = "0x58", baseline = "/mock/base" }) end)
local p = payload_of(0x201E)
check(p ~= nil and #p == 6, "Set_baseline 下发 6 字节", p and tostring(#p) or "nil")
check(p ~= nil and p[1] == math.floor(w1 / 256) and p[2] == w1 % 256 and p[3] == crc8(w1)
	and p[4] == math.floor(w2 / 256) and p[5] == w2 % 256 and p[6] == crc8(w2),
	"恢复内容与存盘一致")

-- 文件 CRC 坏 -> 安全跳过
files["/mock/corrupt"] = string.char(0x12, 0x34, 0x00, 0x56, 0x78, 0x00)
reset()
expect_ok("build 文件 CRC 坏 -> 跳过恢复", function() return build({ fd = FD, address = "0x58", baseline = "/mock/corrupt" }) end)
check(cmd_count(0x201E) == 0, "坏文件不下发 Set_baseline", cmd_list())

-- 器件基线全 0 -> 不覆盖旧文件
words.base1, words.base2 = 0, 0
local before = files["/mock/base"]
reset()
expect_ok("destroy 全 0 基线", function() return destroy({ fd = FD, address = "0x58" }) end)
check(files["/mock/base"] == before, "全 0 不覆盖旧基线文件")
words.base1, words.base2 = 0x1234, 0x5678

reset()
expect_ok("destroy 缺 fd 安全返回", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
