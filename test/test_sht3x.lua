-- SHT3X.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test/test_sht3x.lua（SHT3X 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式：先 dofile 驱动（连带加载真 HAL），再把驱动用到的 HAL 全局替换为桩。
-- 桩内模拟器件语义：
--   i2c_write_reg：写 0x30A2 软复位 / 0xF32D 读状态 / 0x2400 触发测量
--   i2c_read：3 字节（状态寄存器）或 6 字节（测量结果 T_MSB T_LSB tCRC RH_MSB RH_LSB rhCRC）
-- 换算锁定：T = -45 + 175*raw/65535，RH = 100*raw/65535；
-- 状态寄存器 bit0=命令错误、bit1=复位标志。
-- CRC8 多项式 0x31、初值 0xFF（与驱动一致）；测试侧用驱动同源的
-- modules/bitcompat.lua 重算 CRC，保证期望值与驱动同语义。

dofile("modules/SHT3X.lua")
local bitc = dofile("modules/bitcompat.lua")

-- 驱动同款 CRC8（多项式 0x31、初值 0xFF）
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

-- ==================== 桩与器件模型 ====================
local read_fail = false	-- 模拟总线读失败
local set_addr_ok = true	-- 模拟 set_addr 成功
local meas_ok = true		-- 测量命令写入是否成功

-- 状态寄存器 3 字节（status_hi, status_lo, status_crc）
local status_bytes = { 0x00, 0x02, crc8(0x0002) }	-- 默认：复位标志置位、CRC 正确
-- 测量结果 6 字节（t_hi, t_lo, t_crc, rh_hi, rh_lo, rh_crc）
local meas_bytes = { 0, 0, 0, 0, 0, 0 }

function i2c_set_addr(fd, addr) return set_addr_ok end

function i2c_write_reg(fd, hi, lo)
	if not meas_ok and hi == 0x24 and lo == 0x00 then return false end
	return true
end

function i2c_read(fd, n)
	if read_fail then return -1 end
	if n == 3 then
		return status_bytes[1] * 65536 + status_bytes[2] * 256 + status_bytes[3]
	end
	if n == 6 then
		local v = 0
		for i = 1, 6 do v = v * 256 + meas_bytes[i] end
		return v
	end
	return -1
end

function milli_sleep(ms) end

-- 便捷：按原始 T/RH 值设置 meas_bytes（自动补 CRC）
local function set_meas(t_raw, rh_raw)
	local thi, tlo = math.floor(t_raw / 256), t_raw % 256
	local rhi, rlo = math.floor(rh_raw / 256), rh_raw % 256
	meas_bytes = { thi, tlo, crc8(t_raw), rhi, rlo, crc8(rh_raw) }
end

-- 便捷：由目标 T/RH 温度反推原始值
local function raw_of_T(T) return math.floor((T + 45) * 65535 / 175 + 0.5) end
local function raw_of_RH(RH) return math.floor(RH * 65535 / 100 + 0.5) end

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

-- ==================== build：正常路径 ====================
expect_ok("build 正常（复位标志置位）", function() return build({ fd = 3, address = "0x44" }) end)

-- 复位标志未置位（bit1=0）也继续，仅告警
status_bytes = { 0x00, 0x00, crc8(0x0000) }
expect_ok("build 复位标志未置位仍继续", function() return build({ fd = 3, address = "0x44" }) end)

-- ==================== build：错误通道 ====================
expect_err("build 缺 fd", function() return build({ address = "0x44" }) end, "fd or address")
expect_err("build 缺 address", function() return build({ fd = 3 }) end, "fd or address")

set_addr_ok = false
expect_err("build set_addr 失败", function() return build({ fd = 3, address = "0x44" }) end, "set_addr failed")
set_addr_ok = true

read_fail = true
expect_err("build 读状态失败", function() return build({ fd = 3, address = "0x44" }) end, "Failed to read")
read_fail = false

-- 状态 CRC 不匹配
status_bytes = { 0x00, 0x02, 0xFF }
expect_err("build 状态 CRC 不匹配", function() return build({ fd = 3, address = "0x44" }) end, "CRC mismatch")

-- 命令状态错误（bit0=1）
status_bytes = { 0x00, 0x01, crc8(0x0001) }
expect_err("build 命令状态错误 (bit0)", function() return build({ fd = 3, address = "0x44" }) end, "command status error")

-- 恢复正常状态供后续 call 测试
status_bytes = { 0x00, 0x02, crc8(0x0002) }

-- ==================== call：换算 ====================
-- T=25°C -> raw≈26220；RH=50% -> raw≈32768
set_meas(raw_of_T(25), raw_of_RH(50))
local res = expect_ok("call T=25C RH=50%", function() return call({ fd = 3, address = "0x44" }) end)
if res then
	check(near(res.T, 25, 0.5), "T ≈ 25C", tostring(res.T))
	check(near(res.RH, 50, 0.5), "RH ≈ 50%", tostring(res.RH))
	check(res.CRC_OK == 1, "CRC_OK=1", tostring(res.CRC_OK))
end

-- T=0°C -> raw≈16901；RH=0% -> raw=0
set_meas(raw_of_T(0), raw_of_RH(0))
res = expect_ok("call T=0C RH=0%", function() return call({ fd = 3, address = "0x44" }) end)
if res then
	check(near(res.T, 0, 0.5), "T ≈ 0C", tostring(res.T))
	check(near(res.RH, 0, 0.5), "RH ≈ 0%", tostring(res.RH))
end

-- 满量程：raw=65535 -> T=130C, RH=100%
set_meas(65535, 65535)
res = expect_ok("call 满量程 T=130C RH=100%", function() return call({ fd = 3, address = "0x44" }) end)
if res then
	check(near(res.T, 130, 0.5), "T ≈ 130C", tostring(res.T))
	check(near(res.RH, 100, 0.5), "RH ≈ 100%", tostring(res.RH))
end

-- 零值：raw=0 -> T=-45C, RH=0%
set_meas(0, 0)
res = expect_ok("call 零值 T=-45C RH=0%", function() return call({ fd = 3, address = "0x44" }) end)
if res then
	check(near(res.T, -45, 0.5), "T ≈ -45C", tostring(res.T))
	check(res.RH == 0, "RH = 0%", tostring(res.RH))
end

-- CRC 失败：故意破坏测量字节 CRC
set_meas(raw_of_T(25), raw_of_RH(50))
meas_bytes[2] = bitc.bxor(meas_bytes[2], 0xFF)	-- 破坏 T 低字节 -> T CRC 失败
res = expect_ok("call CRC 失败仍返回 OK（不中断）", function() return call({ fd = 3, address = "0x44" }) end)
if res then
	check(res.CRC_OK == 0, "CRC_OK=0", tostring(res.CRC_OK))
end

-- 测量命令写入失败
meas_ok = false
expect_err("call 测量命令写入失败", function() return call({ fd = 3, address = "0x44" }) end, "measure command")
meas_ok = true

-- 缺 fd
expect_err("call 缺 fd", function() return call({ address = "0x44" }) end, "fd or address")
expect_err("call 缺 address", function() return call({ fd = 3 }) end, "fd or address")

-- 读测量失败
read_fail = true
expect_err("call 读测量失败", function() return call({ fd = 3, address = "0x44" }) end, "Failed to read")
read_fail = false

-- ==================== destroy ====================
expect_ok("destroy 无资源需释放", function() return destroy({}) end)

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
