-- sensor_daemon.lua - 万能配置驱动采样守护 chunk
-- bot4cj 仓颉语言机器人框架
--
-- 一个通用 chunk 服务任意「build/call/destroy 型」传感器驱动：从 DAEMON_CONFIG
-- 读取配置，动态 dofile 对应驱动，开总线→build→循环 call 采样→按 fields 提取→publish。
-- 新增传感器无需写 Lua，只在 @Daemon 矩阵加一行配置。
--
-- DAEMON_CONFIG（"k=v;k=v" 串，来自 @Daemon 矩阵除 name/chunk 外的列）：
--   driver   必填，驱动模块名（modules/<driver>.lua，如 MPU6050/BME280/ADS1115/SGP30）
--   proto    i2c（默认）| spi
--   path     总线设备路径（默认 i2c:/dev/i2c-1，spi:/dev/spidev0.0）
--   addr     器件地址（i2c 如 0x68；spi 忽略）
--   interval 采样间隔 ms（默认 10）
--   fields   逗号分隔的 call() 返回字段名，按序 publish（如 ACCEL_X,ACCEL_Y,ACCEL_Z）
--   sign     可选，16 表示对提取值做 16 位二进制补码符号扩展
--   spi 专用：mode/bits/speed/delay
--   其余任意键（如 channel/pga）作为 build/call 的附加参数透传给驱动

if jit then jit.off() end

local dir = os.getenv("BOT4CJ_MODULE_PATH") or "./modules"
local P = dofile(dir .. "/lib/pthread.lua")
dofile(dir .. "/lib/HAL.lua")

-- 解析配置串
local cfg = {}
for k, v in string.gmatch(DAEMON_CONFIG or "", "([^;=]+)=([^;]*)") do
	cfg[k] = v
end

local driver   = cfg.driver
local proto    = cfg.proto or "i2c"
local path     = cfg.path or (proto == "spi" and "/dev/spidev0.0" or "/dev/i2c-1")
local addr     = cfg.addr
local interval = tonumber(cfg.interval) or 10
local sign     = tonumber(cfg.sign)

local slot = P.slot_attach(DAEMON_SLOT_ADDR)

-- 无 driver 配置：发布错误标志退出
if not driver or driver == "" then
	P.publish(slot, { -9 })		-- 标志：缺少 driver 配置
	return
end

-- fields 列表（逗号或管道分隔；SQL 结构脚本中逗号是值分隔符，推荐用管道 |）
local fields = {}
for f in string.gmatch(cfg.fields or "", "[^,|]+") do
	fields[#fields + 1] = f
end

-- 加载驱动模块（提供全局 build/call/destroy）
local ok_load, load_err = pcall(dofile, dir .. "/" .. driver .. ".lua")
if not ok_load then
	P.publish(slot, { -8 })		-- 标志：驱动加载失败
	return
end

-- 高速循环中静默驱动的 print 刷屏（本状态机独立）
print = function() end

-- 符号扩展
local function sx(v)
	v = math.floor(tonumber(v) or 0)
	if sign == 16 then
		v = v % 65536
		if v >= 32768 then v = v - 65536 end
	end
	return v
end

-- 组装驱动参数：fd/address + 配置中的额外键（排除已知的框架键）
local known = {
	driver = 1, proto = 1, path = 1, addr = 1, interval = 1,
	fields = 1, sign = 1, mode = 1, bits = 1, speed = 1, delay = 1,
}
local params = {}
if addr then params.address = addr end
for k, v in pairs(cfg) do
	if not known[k] then params[k] = v end
end

-- 打开总线
local fd
if proto == "spi" then
	fd = spi_open(path)
	if fd and fd >= 0 then
		spi_setup(fd, tonumber(cfg.mode or 0), tonumber(cfg.bits or 8),
		          tonumber(cfg.speed or 1000000), tonumber(cfg.delay or 0))
	end
else
	fd = i2c_open(path)
end
if not fd or fd < 0 then
	P.publish(slot, { -1 })		-- 标志：总线打开失败
	return
end
params.fd = fd

-- 配置器件
local r = build(params)
if type(r) ~= "table" or r.result ~= "OK" then
	P.publish(slot, { -2 })		-- 标志：器件配置失败
	if proto == "spi" then spi_close(fd) else i2c_close(fd) end
	return
end

-- 采样循环
while not P.should_stop(slot) do
	local s = call(params)
	if type(s) == "table" and s.result == "OK" then
		local vals = {}
		for i = 1, #fields do
			vals[i] = sx(s[fields[i]])
		end
		P.publish(slot, vals)
	end
	milli_sleep(interval)
end

-- 收尾
destroy(params)
if proto == "spi" then spi_close(fd) else i2c_close(fd) end
