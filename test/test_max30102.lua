-- MAX30102.lua 离线测试（桩替身，不触碰真实 I2C）
-- 用法: luajit test_max30102.lua（MAX30102 依赖 HAL 的 ffi，需 LuaJIT）
--
-- mock 方式（同 test_sgp30.lua 约定）：先 dofile 驱动（连带加载真 HAL），
-- 再替换 HAL 全局。器件按寄存器 + FIFO 环形队列建模：
--   - 写寄存器逐字节生效，RESET 位清空全部配置与 FIFO（与芯片一致）
--   - FIFO 弹样本按模式出 3/6 字节；18 位 MSB 对齐编码用独立公式
--     （floor/取模拆字节），与驱动的乘加解码互为交叉验证
--   - 指针 = 进出计数模 32，与驱动 (wr-rd)%32 可用数公式同一语义
--   - fake 时钟 + 注入表：驱动 wait 轮询用的 milli_sleep 推进时钟，
--     到点入队样本，模拟「等待期间芯片持续产样」

dofile("modules/MAX30102.lua")

-- ==================== 器件模型 ====================
local REG_FIFO_CFG, REG_MODE, REG_SPO2 = 0x08, 0x09, 0x0A
local REG_LED1, REG_LED2 = 0x0C, 0x0D

local regs = {}				-- reg -> byte
local wr_seq = {}			-- 写记录 {reg=, val=}
local part_id = 0x15
local nak = false			-- 写/读无应答
local fail_after = math.huge	-- 成功读 fail_after 次后开始失败（定位第几次读出错）
local reads_done = 0
local set_addr_fail = false

local function rd_gate()	-- 读失败门：先到的读照常成功，越过配额即失败
	if reads_done >= fail_after then return false end
	reads_done = reads_done + 1
	return true
end

local fifo = {}				-- 样本队列 {red=,ir=}（HR 模式 ir 忽略）
local pushed, popped = 0, 0
local now_ms = 0
local injections = {}		-- {at=ms, red=, ir=}：时钟到点入队

local function push(red, ir)
	if #fifo >= 32 then table.remove(fifo, 1) end	-- rollover：覆盖最旧
	fifo[#fifo + 1] = { red = red, ir = ir }
	pushed = pushed + 1
end

-- 通道 18 位 -> 3 字节（MSB 对齐：[17:10]/[9:2]/[1:0]<<6），独立于驱动解码
local function ch_bytes(s, out)
	out[#out + 1] = math.floor(s / 1024) % 256
	out[#out + 1] = math.floor(s / 4) % 256
	out[#out + 1] = (s % 4) * 64
end

-- 注意：桩定义在 dofile 之后（先定义会被 HAL 真函数覆盖）
function i2c_set_addr(fd, addr) return not set_addr_fail end

function i2c_write_reg(fd, reg, val)
	if nak then return false end
	val = math.floor(val) % 256
	wr_seq[#wr_seq + 1] = { reg = reg, val = val }
	if reg == REG_MODE and math.floor(val / 64) % 2 == 1 then
		regs = {}				-- RESET：全寄存器回上电默认、FIFO 与指针清零
		fifo = {}
		pushed, popped = 0, 0
		regs[REG_MODE] = val % 64		-- RESET 位自清零
		return true
	end
	regs[reg] = val
	return true
end

function i2c_read_reg(fd, reg)
	if nak or not rd_gate() then return -1 end
	if reg == 0xFF then return part_id end
	if reg == 0x04 then return pushed % 32 end
	if reg == 0x06 then return popped % 32 end
	return regs[reg] or 0
end

function i2c_read_multi(fd, reg, n)
	if nak or not rd_gate() then return -1 end
	assert(reg == 0x07, "unexpected burst register " .. tostring(reg))
	local s = fifo[1]
	if s == nil then return 0 end	-- 空读模型：全 0（驱动应先查指针避免）
	table.remove(fifo, 1)
	popped = popped + 1
	local bytes = {}
	ch_bytes(s.red, bytes)
	if n == 6 then ch_bytes(s.ir, bytes) end
	local v = 0
	for i = 1, n do v = v * 256 + (bytes[i] or 0) end
	return v
end

function milli_sleep(ms)
	now_ms = now_ms + ms
	for i = #injections, 1, -1 do
		local inj = injections[i]
		if now_ms >= inj.at then
			push(inj.red, inj.ir)
			table.remove(injections, i)
		end
	end
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

local FD, ADDR = 3, 0x57
local function reset()
	wr_seq = {}
	fifo = {}
	pushed, popped = 0, 0
	injections = {}
	now_ms = 0
	fail_after = math.huge
	reads_done = 0
	nak = false
	set_addr_fail = false
end

local function wr_count(reg)
	local n = 0
	for _, w in ipairs(wr_seq) do
		if w.reg == reg then n = n + 1 end
	end
	return n
end

local function last_wr()
	return wr_seq[#wr_seq]
end

-- ==================== build：命令序与配置编码 ====================
reset()
local res = expect_ok("build 默认配置（spo2/100sps/69us/4096nA/avg1/6.4mA）",
	function() return build({ fd = FD, address = "0x57" }) end)
check(#wr_seq == 9, "build 命令共 9 写（RESET+FIFO+SPO2+LED1+LED2+清3指针+MODE）", tostring(#wr_seq))
check(wr_seq[1].reg == REG_MODE and wr_seq[1].val == 0x40, "首写 RESET", string.format("0x%02X=0x%02X", wr_seq[1].reg, wr_seq[1].val))
check(regs[REG_MODE] == 0x03, "MODE=0x03（SpO2）", string.format("0x%02X", regs[REG_MODE]))
check(regs[REG_FIFO_CFG] == 0x08, "FIFO_CFG=0x08（avg=1+rollover）", string.format("0x%02X", regs[REG_FIFO_CFG]))
check(regs[REG_SPO2] == 0x24, "SPO2=0x24（adc4096+rate100+pw69）", string.format("0x%02X", regs[REG_SPO2]))
check(regs[REG_LED1] == 32 and regs[REG_LED2] == 32, "LED 电流 6.4mA -> 0x20", string.format("0x%02X/0x%02X", regs[REG_LED1], regs[REG_LED2]))
check(wr_count(0x04) == 1 and wr_count(0x05) == 1 and wr_count(0x06) == 1, "FIFO 三指针清零")

reset()
expect_ok("build hr 模式", function() return build({ fd = FD, address = "0x57", opmode = "hr" }) end)
check(regs[REG_MODE] == 0x02, "MODE=0x02（HR，仅红光）", string.format("0x%02X", regs[REG_MODE]))

reset()
expect_ok("build 组合配置编码", function()
	return build({ fd = FD, address = "0x57", rate = "1000", pw = "118", adc = "8192", avg = "16" })
end)
check(regs[REG_SPO2] == 0x55, "SPO2=0x55（adc8192+rate1000+pw118）", string.format("0x%02X", regs[REG_SPO2]))
check(regs[REG_FIFO_CFG] == 0x48, "FIFO_CFG=0x48（avg=16+rollover）", string.format("0x%02X", regs[REG_FIFO_CFG]))

reset()
expect_ok("build LED 编码与 led2 缺省继承 led1", function()
	return build({ fd = FD, address = "0x57", led1 = "12.8" })
end)
check(regs[REG_LED1] == 64 and regs[REG_LED2] == 64, "12.8mA -> 0x40，led2 继承", string.format("0x%02X/0x%02X", regs[REG_LED1], regs[REG_LED2]))

reset()
expect_ok("build LED 四舍五入与钳制", function()
	return build({ fd = FD, address = "0x57", led1 = "6.3", led2 = "60" })
end)
check(regs[REG_LED1] == 32, "6.3mA 四舍五入 -> 0x20", string.format("0x%02X", regs[REG_LED1]))
check(regs[REG_LED2] == 255, "60mA 超上限钳制 -> 0xFF", string.format("0x%02X", regs[REG_LED2]))

-- ==================== build：错误路径 ====================
reset()
expect_err("build 非法 rate", function() return build({ fd = FD, address = "0x57", rate = "333" }) end, "rate(sps)")
expect_err("build 非法 pw", function() return build({ fd = FD, address = "0x57", pw = "100" }) end, "pw(us)")
expect_err("build 非法 adc", function() return build({ fd = FD, address = "0x57", adc = "1000" }) end, "adc(nA)")
expect_err("build 非法 avg", function() return build({ fd = FD, address = "0x57", avg = "3" }) end, "avg")
expect_err("build 非法 opmode", function() return build({ fd = FD, address = "0x57", opmode = "xxx" }) end, "opmode")
expect_err("build 非法 n", function() return build({ fd = FD, address = "0x57", n = "9" }) end, "n(samples)")
expect_err("build 非法 led1", function() return build({ fd = FD, address = "0x57", led1 = "abc" }) end, "led1(mA)")

reset()
expect_err("build rate 超脉宽上限（800sps@411us）", function()
	return build({ fd = FD, address = "0x57", rate = "800", pw = "411" })
end, "exceeds limit")

reset()
part_id = 0x16
expect_err("build Part ID 错误", function() return build({ fd = FD, address = "0x57" }) end, "unexpected Part ID")
part_id = 0x15

reset()
nak = true			-- 注意：nak/set_addr_fail 须在 reset() 之后设置（reset 会清桩状态）
expect_err("build 器件无应答", function() return build({ fd = FD, address = "0x57" }) end, "not responding")
nak = false

reset()
expect_err("build 缺 fd", function() return build({ address = "0x57" }) end, "fd or address")
expect_err("build 非表参数", function() return build("bad") end, "Invalid Arguments Type")

reset()
set_addr_fail = true
expect_err("build set_addr 失败", function() return build({ fd = FD, address = "0x57" }) end, "set_addr failed")
set_addr_fail = false

-- ==================== call：样本编解码（交叉验证） ====================
reset()
expect_ok("call 前 build（默认 spo2）", function() return build({ fd = FD, address = "0x57" }) end)

reset()
push(100000, 50000)
res = expect_ok("call 单样本", function() return call({ fd = FD, address = "0x57" }) end)
if res then
	check(res.count == 1 and res.avail == 1, "count=1 avail=1", string.format("%s/%s", tostring(res.count), tostring(res.avail)))
	check(res.RED == 100000 and res.IR == 50000, "RED=100000 IR=50000（别名键）", string.format("%s/%s", tostring(res.RED), tostring(res.IR)))
	check(res.RED0 == 100000 and res.IR0 == 50000, "RED0/IR0 与别名一致")
	check(res.SAT == 0, "SAT=0")
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57" }) end)
push(0, 262143)
push(1, 131072)
push(65535, 262142)
push(262143, 1023)
res = expect_ok("call 18 位边界值编解码", function() return call({ fd = FD, address = "0x57", n = "8" }) end)
if res then
	check(res.count == 4, "count=4", tostring(res.count))
	check(res.RED0 == 0 and res.IR0 == 262143, "样本0: 0/0x3FFFF", string.format("%s/%s", tostring(res.RED0), tostring(res.IR0)))
	check(res.RED1 == 1 and res.IR1 == 131072, "样本1: 1/0x20000")
	check(res.RED2 == 65535 and res.IR2 == 262142, "样本2: 0xFFFF/0x3FFFE")
	check(res.RED3 == 262143 and res.IR3 == 1023, "样本3: 0x3FFFF/0x3FF")
	check(res.SAT == 1, "满码样本置 SAT=1", tostring(res.SAT))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57", n = "3" }) end)
for i = 1, 5 do push(1000 + i, 2000 + i) end
res = expect_ok("call n=3 排空至多 3 样本（FIFO 序旧->新）", function() return call({ fd = FD, address = "0x57" }) end)
if res then
	check(res.count == 3 and res.avail == 5, "count=3 avail=5", string.format("%s/%s", tostring(res.count), tostring(res.avail)))
	check(res.RED0 == 1001 and res.RED2 == 1003, "RED0=1001 RED2=1003（旧->新）", string.format("%s/%s", tostring(res.RED0), tostring(res.RED2)))
	check(res.IR0 == 2001 and res.IR2 == 2003, "IR0=2001 IR2=2003")
	check(res.RED == 1003, "别名 RED=最新样本", tostring(res.RED))
end
res = expect_ok("call 再次消费余量", function() return call({ fd = FD, address = "0x57" }) end)
if res then
	check(res.count == 2 and res.RED0 == 1004, "余 2 样本续排", string.format("%s/%s", tostring(res.count), tostring(res.RED0)))
end

-- ==================== call：空 FIFO 与 wait ====================
reset()
expect_ok("call 前 build（wait=30）", function() return build({ fd = FD, address = "0x57", wait = "30" }) end)
expect_err("call 空 FIFO 无注入 -> ERR", function() return call({ fd = FD, address = "0x57" }) end, "FIFO empty")

reset()
expect_ok("call 前 build（wait=30）", function() return build({ fd = FD, address = "0x57", wait = "30" }) end)
injections[1] = { at = 15, red = 777, ir = 888 }
res = expect_ok("call 等待期间产样 -> 取到", function() return call({ fd = FD, address = "0x57" }) end)
if res then
	check(res.count == 1 and res.RED == 777 and res.IR == 888, "等到 15ms 注入样本", string.format("%s/%s", tostring(res.RED), tostring(res.IR)))
end

reset()
expect_ok("call 前 build（wait 缺省 0）", function() return build({ fd = FD, address = "0x57" }) end)
now_ms = 0		-- build 的 RESET 延时已推进 5ms，这里归零便于断言 call 不再 sleep
expect_err("call wait=0 空即报错（不 sleep）", function() return call({ fd = FD, address = "0x57" }) end, "FIFO empty")
check(now_ms == 0, "wait=0 不推进 fake 时钟", tostring(now_ms))

-- ==================== call：hr 模式 ====================
reset()
expect_ok("build hr 模式", function() return build({ fd = FD, address = "0x57", opmode = "hr" }) end)
push(12345, 99999)
res = expect_ok("call hr 模式仅红光", function() return call({ fd = FD, address = "0x57" }) end)
if res then
	check(res.RED == 12345 and res.RED0 == 12345, "RED=12345", tostring(res.RED))
	check(res.IR == nil and res.IR0 == nil, "无 IR 键（HR 3 字节样本）")
end

-- ==================== call：回绕与错误路径 ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57" }) end)
for i = 1, 40 do push(9000 + i, 0) end	-- 32 深环形队列，rollover 覆盖最旧
res = expect_ok("call 指针差回绕（avail 只反映未回绕部分）", function() return call({ fd = FD, address = "0x57", n = "8" }) end)
if res then
	check(res.avail == 8 and res.count == 8, "avail=8 count=8", string.format("%s/%s", tostring(res.avail), tostring(res.count)))
	check(res.RED0 == 9009, "读到队列头部第 9 个入队样本（最旧未覆盖者）", tostring(res.RED0))
end

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57" }) end)
expect_err("call 非表参数", function() return call(42) end, "Invalid Arguments")
expect_err("call 缺 fd", function() return call({ address = "0x57" }) end, "fd or address")

-- ==================== 读失败定位（指针读 / 样本读 分 别 注 入） ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57" }) end)
fail_after = 0		-- 第 1 次读（FIFO 写指针）即失败
expect_err("call 指针读失败", function() return call({ fd = FD, address = "0x57" }) end, "read FIFO pointers failed")

reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57" }) end)
push(7, 7)
fail_after = 3		-- 放行 2 次指针读，第 3 次读（FIFO 数据）失败
expect_err("call 样本读失败", function() return call({ fd = FD, address = "0x57" }) end, "FIFO data read failed")

set_addr_fail = true
expect_err("call set_addr 失败", function() return call({ fd = FD, address = "0x57" }) end, "set_addr failed")
set_addr_fail = false

-- ==================== destroy ====================
reset()
expect_ok("call 前 build", function() return build({ fd = FD, address = "0x57" }) end)
reset()		-- 清写记录，destroy 的写单独看
expect_ok("destroy 置 SHDN", function() return destroy({ fd = FD, address = "0x57" }) end)
local lw = last_wr()
check(lw ~= nil and lw.reg == REG_MODE and lw.val == 0x80, "末写 MODE=0x80（SHDN）",
	lw and string.format("0x%02X=0x%02X", lw.reg, lw.val) or "nil")

reset()
expect_ok("destroy 缺 fd 安全返回", function() return destroy({}) end)
check(#wr_seq == 0, "缺 fd 不发任何写", tostring(#wr_seq))

reset()
expect_err("destroy 非表参数", function() return destroy(nil) end, "Invalid Arguments Type")

-- ==================== 汇总 ====================
if fails == 0 then
	print("all passed")
else
	print(fails .. " case(s) failed")
	os.exit(1)
end
