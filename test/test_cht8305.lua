-- CHT8305.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_cht8305.lua（CHT8305 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_sgp30.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按指针寄存器协议建模：
--   - 写单字节 = 设指针；纯读 2 字节 = 读当前指针寄存器（大端）
--   - MID/DID 常量可注入错误值验证自检；软复位写（0x02 <- 0x80）落模型
--   - 温/湿度原始码注入，期望 T/H 用独立算式重算交叉验证
--   - 帧日志断言协议序：指针写（单字节）与纯读交替

dofile("modules/CHT8305.lua")

-- ==================== 器件模型 ====================
local pointer = 0x00			-- 当前指针
local regmap = {				-- 寄存器值（可注入）
	[0x00] = nil,				-- 温度原始码（测试注入）
	[0x01] = nil,				-- 湿度原始码
	[0x02] = 0x0000,			-- 配置（POR）
	[0xFE] = 0x5959,			-- 厂商 ID
	[0xFF] = 0x8305,			-- 器件 ID
}
local wr_nak = false
local rd_nak = false
local frames = {}				-- 帧日志 {type="ptr"/"rd", reg=}

local function reset()
	pointer = 0x00
	regmap = { [0x02] = 0x0000, [0xFE] = 0x5959, [0xFF] = 0x8305 }
	wr_nak = false
	rd_nak = false
	frames = {}
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return true end

function i2c_write_multi(fd, val, ...)
	if wr_nak then return -1 end
	local extra = { ... }
	if #extra == 0 then
		-- 纯指针写（读前设指针）
		pointer = math.floor(tonumber(val) or 0) % 256
		frames[#frames + 1] = { type = "ptr", reg = pointer }
	else
		-- 单事务 [指针, 数据]：写寄存器
		assert(#extra == 1, "register write is pointer + 1 data byte")
		pointer = math.floor(tonumber(val) or 0) % 256
		local data = math.floor(tonumber(extra[1]) or 0) % 256
		regmap[pointer] = (regmap[pointer] or 0)
		if pointer == 0x02 and data >= 0x80 then
			-- 软复位：bit15 自清零，其余位由本次写入的值决定（此处仅 bit15）
			regmap[pointer] = 0x0000
		else
			regmap[pointer] = data
		end
		frames[#frames + 1] = { type = "wr", reg = pointer, data = data }
	end
	return 0
end

function i2c_read(fd, n)
	if rd_nak then return -1 end
	assert(n == 2, "pure 16-bit read expected")
	local v = regmap[pointer] or 0
	frames[#frames + 1] = { type = "rd", reg = pointer }
	return math.floor(v / 256) % 256 * 256 + v % 256
end

function milli_sleep(ms) end

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

local FD, ADDR = 3, 0x40

-- ==================== build：软复位与 ID 自检 ====================
reset()
regmap[0x00] = 0x6666		-- T 原始码：25.00°C
regmap[0x01] = 0x9999		-- H 原始码：60.00%RH（0x9999/65535 x 100）
expect_ok("build 默认（软复位 + 双 ID 自检）", function() return build({ fd = FD, address = "0x40" }) end)
local ptrs, rds, wrs = {}, {}, {}
for _, f in ipairs(frames) do
	if f.type == "ptr" then ptrs[#ptrs + 1] = string.format("0x%02X", f.reg)
	elseif f.type == "wr" then wrs[#wrs + 1] = string.format("0x%02X", f.reg)
	else rds[#rds + 1] = f.reg end
end
check(table.concat(wrs, ",") == "0x02", "软复位为单事务寄存器写（0x02 <- 0x80）", table.concat(wrs, ","))
check(table.concat(ptrs, ",") == "0xFE,0xFF", "指针序：MID -> DID", table.concat(ptrs, ","))
check(#rds == 2, "自检读两次（MID/DID）", tostring(#rds))
check(regmap[0x02] == 0x0000, "软复位后配置回 POR", string.format("0x%04X", regmap[0x02]))

reset()
regmap[0xFE] = 0x1234
expect_err("build 厂商 ID 错误", function() return build({ fd = FD, address = "0x40" }) end, "manufacturer id")
reset()
regmap[0xFF] = 0x1234
expect_err("build 器件 ID 错误", function() return build({ fd = FD, address = "0x40" }) end, "device id")

reset()
wr_nak = true
expect_err("build 写无应答", function() return build({ fd = FD, address = "0x40" }) end, "not responding")
wr_nak = false
rd_nak = true
expect_err("build 读无应答", function() return build({ fd = FD, address = "0x40" }) end, "not responding")
rd_nak = false
expect_err("build 缺 fd", function() return build({ address = "0x40" }) end, "fd or address")
expect_err("build 非表参数", function() return build(4) end, "Invalid Arguments Type")

-- ==================== call：换算与协议序 ====================
reset()
regmap[0x00] = 0x0000		-- -40.0°C
regmap[0x01] = 0xFFFF		-- 100.0%RH
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x40" }) end)
local res = expect_ok("call 边界值", function() return call({ fd = FD, address = "0x40" }) end)
if res then
	check(near(res.T, -40.0), "T=-40（零码）", tostring(res.T))
	check(near(res.H, 100.0), "H=100（满码）", tostring(res.H))
end

reset()
regmap[0x00] = 0x6666		-- 0x6666/65535 x 165 - 40
regmap[0x01] = 0x9999
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x40" }) end)
res = expect_ok("call 典型值", function() return call({ fd = FD, address = "0x40" }) end)
if res then
	check(near(res.T, 0x6666 * 165.0 / 65535.0 - 40.0), "T 与独立算式一致",
		string.format("%s vs %s", tostring(res.T), tostring(0x6666 * 165.0 / 65535.0 - 40.0)))
	check(near(res.H, 0x9999 * 100.0 / 65535.0), "H 与独立算式一致",
		string.format("%s vs %s", tostring(res.H), tostring(0x9999 * 100.0 / 65535.0)))
end

reset()
regmap[0x00] = 0x0000
regmap[0x01] = 0x0000
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x40" }) end)
reset_frames = frames
local n0 = #frames
res = expect_ok("call 协议序", function() return call({ fd = FD, address = "0x40" }) end)
if res then
	local seq = {}
	for i = n0 + 1, #frames do seq[#seq + 1] = frames[i].type .. ":" .. string.format("0x%02X", frames[i].reg) end
	check(table.concat(seq, ",") == "ptr:0x00,rd:0x00,ptr:0x01,rd:0x01",
		"温/湿度各一次指针写+纯读", table.concat(seq, ","))
end

-- ==================== call：错误路径 ====================
reset()
regmap[0x00] = 0x0000
regmap[0x01] = 0x0000
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x40" }) end)
wr_nak = true
expect_err("call 指针写无应答", function() return call({ fd = FD, address = "0x40" }) end, "not responding")
wr_nak = false
rd_nak = true
expect_err("call 读无应答", function() return call({ fd = FD, address = "0x40" }) end, "not responding")
rd_nak = false
expect_err("call 非表参数", function() return call(6) end, "Invalid Arguments")
expect_err("call 缺 address", function() return call({ fd = FD }) end, "fd or address")

-- ==================== destroy ====================
reset()
expect_ok("destroy 无关断寄存器", function() return destroy({ fd = FD, address = "0x40" }) end)
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
