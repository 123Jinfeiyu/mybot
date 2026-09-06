-- MHZ19.lua 帧协议离线测试（桩替身，不触碰真实串口）
-- 用法: luajit test_mhz19.lua（MHZ19 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_ina226.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局的 uart_* 函数。验证：请求帧逐字节（含校验和 0x79）、
-- 帧头扫描容忍前导噪声、校验和校验、短读/写失败错误通道。

dofile("modules/MHZ19.lua")

-- ==================== uart 桩 ====================
local written = {}			-- 写帧记录（每次 uart_write 的字节表）
local resp_feed = {}		-- uart_read 按序吐出的应答表
local flush_count = 0
local write_fail = false
local read_fail = false

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function uart_flush(fd) flush_count = flush_count + 1; return 0 end

function uart_write(fd, data)
	if write_fail then return -1 end
	local copy = {}
	for i, v in ipairs(data) do copy[i] = v end
	written[#written + 1] = copy
	return #copy
end

function uart_read(fd, n, timeout_ms)
	if read_fail then return nil end
	local v = resp_feed[#resp_feed]		-- 简化：总是吐最后一帧（每次 call 只读一次）
	if v == nil then return {} end
	local copy = {}
	for i, x in ipairs(v) do copy[i] = x end
	return copy
end

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

local FD = 7

-- ==================== build ====================
written = {}
flush_count = 0
expect_ok("build 校验句柄", function() return build({ fd = FD }) end)
check(flush_count == 1, "build 清输入缓冲一次", tostring(flush_count))
expect_err("build 缺 fd", function() return build({}) end, "Invalid Argument: fd")

-- ==================== call：正常应答 ====================
written = {}
flush_count = 0
resp_feed = { { 0xFF, 0x86, 0x01, 0xF4, 0x42, 0x00, 0x00, 0x00, 0x43 } }
local res = expect_ok("call 正常应答（CO2=500）", function() return call({ fd = FD }) end)
if res then
	check(res.CO2 == 500, "CO2 = 0x01F4 = 500ppm", tostring(res.CO2))
	check(res.TEMP == 26, "TEMP = 字节-40 = 26（近似）", tostring(res.TEMP))
	check(res.S == 0, "S = 状态字节", tostring(res.S))
	check(#written == 1 and #written[1] == 9, "请求帧 9 字节", written[1] and tostring(#written[1]) or "nil")
	local want = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 }
	local same = true
	for i = 1, 9 do
		if written[1][i] ~= want[i] then same = false; break end
	end
	check(same, "请求帧逐字节一致（含校验和 0x79）", table.concat(written[1], ","))
	check(flush_count == 1, "call 发帧前清缓冲", tostring(flush_count))
end

-- ==================== call：前导噪声 + 帧头扫描 ====================
written = {}
resp_feed = { { 0xAA, 0x55, 0xFF, 0x86, 0x01, 0xF4, 0x42, 0x00, 0x00, 0x00, 0x43 } }
res = expect_ok("call 前导噪声容忍", function() return call({ fd = FD }) end)
if res then
	check(res.CO2 == 500, "帧头扫描跳过噪声定位 FF 86", tostring(res.CO2))
end

-- ==================== call：校验和错误 ====================
written = {}
resp_feed = { { 0xFF, 0x86, 0x01, 0xF4, 0x42, 0x00, 0x00, 0x00, 0x44 } }
expect_err("call 校验和不符", function() return call({ fd = FD }) end, "checksum")

-- ==================== call：短读/写失败 ====================
written = {}
resp_feed = { { 0xFF, 0x86, 0x01 } }
expect_err("call 应答短读", function() return call({ fd = FD }) end, "timeout/short")

resp_feed = { { 0xAA, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 } }
expect_err("call 无帧头", function() return call({ fd = FD }) end, "frame head")

write_fail = true
expect_err("call 写失败", function() return call({ fd = FD }) end, "write")
write_fail = false

expect_err("call 缺 fd", function() return call({}) end, "Invalid Argument: fd")

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
