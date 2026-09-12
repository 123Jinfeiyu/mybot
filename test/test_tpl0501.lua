-- TPL0501.lua 离线测试（桩替身，不触碰真实 SPI）
-- 用法: luajit test/test_tpl0501.lua（TPL0501 走 SPI 写-only，需 HAL 桩）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把 spi_write 替换为桩。
-- 协议即一个 SPI 字节：CS 低 -> 移入 8 位抽头值 -> CS 高，无命令字/读回。
-- build 仅记录换算配置（rtotal），不写器件；写失败靠 spi_write 返回 0/-1 报告。
-- 量化：100k 型每步 390.6Ω（100000/255），10k 型每步 39.2Ω。

dofile("modules/TPL0501.lua")

-- ==================== 桩与器件模型 ====================
local last_spi = nil		-- 最近一次 spi_write 写入的抽头值
local spi_fail = false		-- 模拟 SPI 写失败（返回 -1）

function spi_write(fd, level, delay_us)
	last_spi = level
	if spi_fail then return -1 end
	return 0
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

-- ==================== build：配置记录 ====================
-- rtotal 缺省 100000（TPL0501-100）
expect_ok("build 缺省 rtotal", function() return build({ fd = 5 }) end)

-- rtotal='10000'（TPL0501-10）
expect_ok("build rtotal='10000'", function() return build({ fd = 5, rtotal = "10000" }) end)

-- rtotal='-' 归缺省 100000
expect_ok("build rtotal='-'", function() return build({ fd = 5, rtotal = "-" }) end)

-- rtotal='0'（非正）归缺省
expect_ok("build rtotal='0'", function() return build({ fd = 5, rtotal = "0" }) end)

-- 缺 fd
expect_err("build 缺 fd", function() return build({ rtotal = "100000" }) end, "fd")

-- ==================== call：level 直写 ====================
build({ fd = 5, rtotal = "100000" })

local res = expect_ok("call level=128", function() return call({ fd = 5, level = "128" }) end)
check(last_spi == 128, "SPI 写入抽头 128", tostring(last_spi))
if res then
	check(res.level == 128, "返回 level=128", tostring(res.level))
	-- ohms = 128/255*100000 = 50196.08
	check(near(res.ohms, 128 / 255 * 100000), "ohms ≈ 50196Ω", tostring(res.ohms))
end

-- level 字符串（框架元数据侧全为字符串）
res = expect_ok("call 字符串 level='64'", function() return call({ fd = 5, level = "64" }) end)
check(last_spi == 64, "SPI 写入抽头 64", tostring(last_spi))

-- level 越下限（负数 -> 0）
res = expect_ok("call level=-10 限幅 0", function() return call({ fd = 5, level = "-10" }) end)
check(last_spi == 0, "负数限幅 0", tostring(last_spi))

-- level 越上限（>255 -> 255）
res = expect_ok("call level=999 限幅 255", function() return call({ fd = 5, level = "999" }) end)
check(last_spi == 255, "超限限幅 255", tostring(last_spi))

-- ==================== call：percent 换算 ====================
-- percent=50 -> floor(50/100*255+0.5) = floor(127.5+0.5) = 128
res = expect_ok("call percent='50'", function() return call({ fd = 5, percent = "50" }) end)
check(last_spi == 128, "50% -> 抽头 128", tostring(last_spi))

-- percent=0 -> 0
res = expect_ok("call percent='0'", function() return call({ fd = 5, percent = "0" }) end)
check(last_spi == 0, "0% -> 抽头 0", tostring(last_spi))

-- percent=100 -> floor(255+0.5)=255
res = expect_ok("call percent='100'", function() return call({ fd = 5, percent = "100" }) end)
check(last_spi == 255, "100% -> 抽头 255", tostring(last_spi))

-- ==================== call：ohms 换算 ====================
-- rtotal=100000；ohms=50000 -> floor(50000/100000*255+0.5) = floor(127.5+0.5)=128
res = expect_ok("call ohms='50000'", function() return call({ fd = 5, ohms = "50000" }) end)
check(last_spi == 128, "50000Ω/100k -> 抽头 128", tostring(last_spi))

-- ohms 超限（>rtotal -> 255）
res = expect_ok("call ohms='200000' 超限", function() return call({ fd = 5, ohms = "200000" }) end)
check(last_spi == 255, "200kΩ/100k 限幅 255", tostring(last_spi))

-- 优先级：level > percent > ohms（同时传 -> 用 level）
res = expect_ok("call 三参数优先级 level", function()
	return call({ fd = 5, level = "10", percent = "50", ohms = "50000" })
end)
check(last_spi == 10, "三参数 -> 取 level=10", tostring(last_spi))

-- percent 优先于 ohms
res = expect_ok("call 两参数 percent 优先", function()
	return call({ fd = 5, percent = "10", ohms = "50000" })
end)
check(last_spi == 26, "percent 优先 (10% -> 26)", tostring(last_spi))

-- ==================== call：负路径 ====================
-- 无 level/percent/ohms
expect_err("call 缺三参数", function() return call({ fd = 5 }) end, "need level")

-- 非数字
expect_err("call level 非数字", function() return call({ fd = 5, level = "abc" }) end, "not a number")
expect_err("call percent 非数字", function() return call({ fd = 5, percent = "abc" }) end, "not a number")
expect_err("call ohms 非数字", function() return call({ fd = 5, ohms = "abc" }) end, "not a number")

-- 缺 fd
expect_err("call 缺 fd", function() return call({ level = "10" }) end, "fd")

-- SPI 写失败
spi_fail = true
expect_err("call SPI 写失败", function() return call({ fd = 5, level = "10" }) end, "Failed to write")
spi_fail = false

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
