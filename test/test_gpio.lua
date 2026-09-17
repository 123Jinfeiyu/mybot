-- GPIO 离线单元测试
-- 分两层，均不触硬件：
--   1) HAL 段：mock HAL.sys.* 记录 open/ioctl/close，验证 GET_LINEHANDLE 请求
--      结构体字段、句柄 fd 生命周期、路径白名单与 offset 校验（含非整数显式拒绝）
--   2) 驱动层：桩替换全局 gpio_*，验证 modules/GPIO.lua 的 build/call/destroy 契约
-- 用法（项目根目录）: luajit test/test_gpio.lua

local tester = dofile("test/lib/tester.lua")
local check = tester.check

local ffi = require("ffi")
local HAL = dofile("modules/lib/HAL.lua")

------------------------------------------------------------------
-- 第一层：HAL 段（mock 系统调用层）
------------------------------------------------------------------
local calls = {}			-- {op=...} 调用序列
local mock_open_ret = 7		-- gpiochip open 返回的 chip fd
local mock_open_deny = {}	-- path -> true 表示 open 失败
local mock_ioctl_ret = 0	-- 通用 ioctl 返回值
local mock_handle_fd = 42	-- GET_LINEHANDLE 回填的线句柄 fd
local set_values = nil		-- 最近一次 SET_LINE_VALUES 的 values[0]
local get_value = 0			-- GET_LINE_VALUES 回填值

HAL.sys.open = function(path, flags)
	calls[#calls + 1] = { op = "open", path = path, flags = flags }
	if mock_open_deny[path] then return -1 end
	return mock_open_ret
end
HAL.sys.close = function(fd)
	calls[#calls + 1] = { op = "close", fd = fd }
	return 0
end
HAL.sys.ioctl_p = function(fd, req, ptr)
	local rec = { op = "ioctl", fd = fd, req = req }
	calls[#calls + 1] = rec
	if req == HAL.GPIO_GET_LINEHANDLE then
		-- 解包请求结构体：逐字段留痕供断言
		rec.lineoffset = tonumber(ptr.lineoffsets[0])
		rec.flags = tonumber(ptr.flags)
		rec.default_value = tonumber(ptr.default_values[0])
		rec.lines = tonumber(ptr.lines)
		rec.label = ffi.string(ptr.consumer_label)
		if mock_ioctl_ret ~= 0 then return mock_ioctl_ret end
		ptr.fd = mock_handle_fd		-- 模拟内核分配线句柄
		return 0
	elseif req == HAL.GPIOHANDLE_SET_LINE_VALUES then
		rec.value = tonumber(ptr.values[0])
		set_values = rec.value
		return mock_ioctl_ret
	elseif req == HAL.GPIOHANDLE_GET_LINE_VALUES then
		if mock_ioctl_ret ~= 0 then return mock_ioctl_ret end
		ptr.values[0] = get_value
		return 0
	end
	return mock_ioctl_ret
end

local function reset()
	calls, mock_open_deny = {}, {}
	mock_open_ret, mock_ioctl_ret, mock_handle_fd = 7, 0, 42
	set_values, get_value = nil, 0
end
local function find(op, field, value)
	for _, c in ipairs(calls) do
		if c.op == op and (field == nil or c[field] == value) then return c end
	end
	return nil
end
local function count(op)
	local n = 0
	for _, c in ipairs(calls) do if c.op == op then n = n + 1 end end
	return n
end

-- 成功请求：结构体字段与 chip fd 生命周期
reset()
local hfd = gpio_request_line("/dev/gpiochip0", 17, "out", 1, false)
check("请求成功返回内核线句柄 fd", hfd == 42, hfd)
check("以 O_RDWR 打开 gpiochip", find("open", "path", "/dev/gpiochip0") ~= nil
	and find("open", "flags", 2) ~= nil)
do
	local c = find("ioctl", "req", HAL.GPIO_GET_LINEHANDLE)
	check("GET_LINEHANDLE 携带 offset=17", c ~= nil and c.lineoffset == 17,
		c and c.lineoffset)
	check("OUTPUT 标志（1<<1=2）", c ~= nil and c.flags == 2, c and c.flags)
	check("default_value=1", c ~= nil and c.default_value == 1, c and c.default_value)
	check("lines=1、consumer_label=bot4cj",
		c ~= nil and c.lines == 1 and c.label == "bot4cj",
		c and (tostring(c.lines) .. "/" .. tostring(c.label)))
end
check("取到句柄后关闭 chip fd", find("close", "fd", 7) ~= nil)
check("线句柄未被误关", find("close", "fd", 42) == nil)

-- default_value 归一与 active_low 标志位（位不重叠，算术加）
reset()
gpio_request_line("/dev/gpiochip0", 3, "out", 7, true)
local c = find("ioctl", "req", HAL.GPIO_GET_LINEHANDLE)
check("default_value 非 0 归一为 1", c ~= nil and c.default_value == 1,
	c and c.default_value)
check("OUTPUT|ACTIVE_LOW（2+4=6）", c ~= nil and c.flags == 6, c and c.flags)
reset()
gpio_request_line("/dev/gpiochip0", 3, "in", 1, true)
c = find("ioctl", "req", HAL.GPIO_GET_LINEHANDLE)
check("INPUT|ACTIVE_LOW（1+4=5）", c ~= nil and c.flags == 5, c and c.flags)
check("输入模式不写 default_values", c ~= nil and c.default_value == 0,
	c and c.default_value)
reset()
gpio_request_line("/dev/gpiochip0", 3, "in", 0, false)
c = find("ioctl", "req", HAL.GPIO_GET_LINEHANDLE)
check("INPUT 标志（1）", c ~= nil and c.flags == 1, c and c.flags)

-- 路径白名单
reset()
check("拒绝路径穿越", gpio_request_line("../../etc/passwd", 1, "out", 0) == -1)
check("拒绝注入路径", gpio_request_line("/dev/gpiochip0; reboot", 1, "out", 0) == -1)
check("非法路径不发 open", count("open") == 0)

-- offset 必须为非负整数（小数写入 uint32 会被静默截断成另一个线号）
reset()
check("拒绝小数 offset（17.9）", gpio_request_line("/dev/gpiochip0", 17.9, "out", 0) == -1)
check("拒绝负 offset", gpio_request_line("/dev/gpiochip0", -1, "out", 0) == -1)
check("拒绝超 uint32 offset", gpio_request_line("/dev/gpiochip0", 0x100000000, "out", 0) == -1)
check("拒绝非数值 offset", gpio_request_line("/dev/gpiochip0", "x", "out", 0) == -1)
check("非法 offset 不触碰硬件", count("open") == 0)
reset()
check("字符串整数 offset 接受", gpio_request_line("/dev/gpiochip0", "17", "out", 0) == 42)

-- 失败路径：ioctl 失败、内核回填 fd<0、chip 打不开
reset()
mock_ioctl_ret = -1
check("GET_LINEHANDLE 失败返回 -1", gpio_request_line("/dev/gpiochip0", 1, "out", 0) == -1)
check("失败时关闭 chip fd", find("close", "fd", 7) ~= nil)
reset()
mock_handle_fd = -1
check("内核回填 fd<0 判失败", gpio_request_line("/dev/gpiochip0", 1, "out", 0) == -1)
reset()
mock_open_deny["/dev/gpiochip4"] = true
check("chip 打不开返回 -1", gpio_request_line("/dev/gpiochip4", 1, "out", 0) == -1)

-- set / get / free
reset()
check("set: 非 0 归一为 1", gpio_set_value(42, 5) == true and set_values == 1,
	tostring(set_values))
reset()
check("set: 0 写 0", gpio_set_value(42, 0) == true and set_values == 0,
	tostring(set_values))
reset()
mock_ioctl_ret = -1
check("set: ioctl 失败返回 false", gpio_set_value(42, 1) == false)
check("set: 非法 fd 拒绝", gpio_set_value(nil, 1) == false)
reset()
get_value = 1
check("get: 返回 1", gpio_get_value(42) == 1)
reset()
get_value = 0
check("get: 返回 0", gpio_get_value(42) == 0)
reset()
mock_ioctl_ret = -1
check("get: ioctl 失败返回 -1", gpio_get_value(42) == -1)
check("get: 非法 fd 返回 -1", gpio_get_value("x") == -1)
reset()
gpio_free_line(42)
check("free: 关闭线句柄 fd", find("close", "fd", 42) ~= nil)
check("free: nil / -1 安全", gpio_free_line(nil) == nil and gpio_free_line(-1) == nil)

------------------------------------------------------------------
-- 第二层：驱动层（桩替换全局 gpio_*）
------------------------------------------------------------------
local plog = {}				-- 记录驱动对 HAL 的调用
local stub_request_ret = 42	-- gpio_request_line 返回值
local stub_set_ret = true	-- gpio_set_value 返回值
local stub_get_ret = 1		-- gpio_get_value 返回值

gpio_request_line = function(path, offset, direction, init, active_low)
	plog[#plog + 1] = { op = "request", path = path, offset = offset,
		direction = direction, init = init, active_low = active_low }
	return stub_request_ret
end
gpio_set_value = function(hfd, v)
	plog[#plog + 1] = { op = "set", hfd = hfd, v = v }
	return stub_set_ret
end
gpio_get_value = function(hfd)
	plog[#plog + 1] = { op = "get", hfd = hfd }
	return stub_get_ret
end
gpio_free_line = function(hfd)
	plog[#plog + 1] = { op = "free", hfd = hfd }
	return true
end

-- 重新加载驱动（模块级状态按实例隔离，重 dofile 等价于新建硬件变量）
local function reload()
	plog = {}
	stub_request_ret, stub_set_ret, stub_get_ret = 42, true, 1
	dofile("modules/GPIO.lua")
end
local function last(op)
	for i = #plog, 1, -1 do
		if plog[i].op == op then return plog[i] end
	end
	return nil
end
local function count2(op)
	local n = 0
	for _, e in ipairs(plog) do if e.op == op then n = n + 1 end end
	return n
end

-- build 参数校验（负路径，均不得触碰 HAL）
reload()
check("build: 参数非表报错", build("x").result == "ERR")
check("build: 缺 chip 报错", build({ offset = "17", direction = "out" }).result == "ERR")
check("build: chip=0 报错", build({ chip = "0", offset = "17", direction = "out" }).result == "ERR")
check("build: offset 缺失报错", build({ chip = "/dev/gpiochip0", direction = "out" }).result == "ERR")
check("build: direction 非法报错",
	build({ chip = "/dev/gpiochip0", offset = "17", direction = "x" }).result == "ERR")
check("build: 参数错误不发请求", count2("request") == 0)

-- 正常 build（输出 + 低有效 + 初值 1）
reload()
local r = build({ chip = "/dev/gpiochip0", offset = "17", direction = "out",
	active = "low", init = "1" })
check("build: 成功", r.result == "OK", r.message)
do
	local q = last("request")
	check("build: 以解析后的参数请求线",
		q ~= nil and q.path == "/dev/gpiochip0" and q.offset == 17
		and q.direction == "out" and q.init == 1 and q.active_low == true,
		q and (tostring(q.offset) .. "/" .. tostring(q.init) .. "/" .. tostring(q.active_low)))
end

-- 输入模式：init/active 缺省
reload()
r = build({ chip = "/dev/gpiochip0", offset = "27", direction = "in" })
check("build: 输入模式成功", r.result == "OK", r.message)
do
	local q = last("request")
	check("build: in 模式参数缺省",
		q ~= nil and q.direction == "in" and q.init == 0 and q.active_low == false)
end

-- 重复 build 先释放旧句柄再请求
reload()
build({ chip = "/dev/gpiochip0", offset = "17", direction = "out" })
check("首次 build 不释放", count2("free") == 0)
build({ chip = "/dev/gpiochip0", offset = "18", direction = "out" })
check("重复 build 先释放旧句柄", count2("free") == 1 and last("free").hfd == 42)
check("重复 build 请求新线号", last("request").offset == 18)

-- 请求失败
reload()
stub_request_ret = -1
r = build({ chip = "/dev/gpiochip0", offset = "17", direction = "out" })
check("build: 请求失败报 ERR", r.result == "ERR")
check("build: 失败信息含线号", (r.message or ""):find("17", 1, true) ~= nil, r.message)

-- call
reload()
check("call: 未 build 拒绝", call({ value = "1" }).result == "ERR")
check("call: 参数非表拒绝", call("x").result == "ERR")

-- call 输出模式
reload()
build({ chip = "/dev/gpiochip0", offset = "17", direction = "out" })
r = call({ value = "1" })
check("call: 设值成功", r.result == "OK" and r.value == 1, r.message)
check("call: 下发归一值与句柄",
	last("set") ~= nil and last("set").v == 1 and last("set").hfd == 42)
r = call({ value = "0" })
check("call: 设 0 成功", r.result == "OK" and r.value == 0)
check("call: 缺 value 报错", call({}).result == "ERR")
stub_set_ret = false
check("call: 设值失败报错", call({ value = "1" }).result == "ERR")

-- call 输入模式
reload()
build({ chip = "/dev/gpiochip0", offset = "27", direction = "in" })
r = call({})
check("call: 读值成功", r.result == "OK" and r.value == 1, r.message)
stub_get_ret = -1
check("call: 读值失败报错", call({}).result == "ERR")

-- destroy
reload()
build({ chip = "/dev/gpiochip0", offset = "17", direction = "out" })
r = destroy({})
check("destroy: 返回 OK", r.result == "OK")
check("destroy: 释放句柄", last("free") ~= nil and last("free").hfd == 42)
check("destroy 后 call 拒绝", call({ value = "1" }).result == "ERR")
check("destroy: 参数非表报错", destroy("x").result == "ERR")

tester.summary()
