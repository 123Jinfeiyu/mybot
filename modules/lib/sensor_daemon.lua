-- sensor_daemon.lua - 万能配置驱动采样守护 chunk（D 方案：setup 一次 + daemon_tick 单次采样）
-- bot4cj 仓颉语言机器人框架
--
-- daemon_body 先 pcall 本 chunk 一次完成 setup（解析配置、加载驱动、开总线、build 传感器、
-- 附着共享槽），随后逐拍 getglobal("daemon_tick")+pcall 采样一次，拍间由 Cangjie sleep 让出
-- 绿色线程（不再由 chunk 内 ffi usleep 阻塞 worker）。
--
-- DAEMON_CONFIG（"k=v;k=v"，来自 @Daemon 矩阵除 name/chunk 外的列）：
--   driver   必填，驱动模块名（modules/<driver>.lua，如 MPU6050/BME280/ADS1115/SGP30）
--   proto    i2c（默认）| spi | uart（uart 时 path=串口设备、baud=波特率）
--   path     总线设备路径（默认 i2c:/dev/i2c-1，spi:/dev/spidev0.0）
--   addr     器件地址（i2c 如 0x68；spi 忽略）
--   fields   逗号或管道分隔的 call() 返回字段名，按序 publish（SQL 里推荐用 |）
--   sign     可选，16 表示对提取值做 16 位补码符号扩展
--   spi 专用：mode/bits/speed/delay
--   其余任意键（如 channel/pga）作为 build/call 的附加参数透传给驱动
--   （interval 由 daemon_body 逐拍 sleep 使用，不在本 chunk 内处理）

if jit then jit.off() end

local dir = os.getenv("BOT4CJ_MODULE_PATH") or "./modules"
local P = dofile(dir .. "/lib/pthread.lua")
dofile(dir .. "/lib/HAL.lua")			-- i2c/spi 采样

-- 解析配置串
local cfg = {}
for k, v in string.gmatch(DAEMON_CONFIG or "", "([^;=]+)=([^;]*)") do
	cfg[k] = v
end

local driver = cfg.driver
local proto  = cfg.proto or "i2c"
local path   = cfg.path or (proto == "spi" and "/dev/spidev0.0" or "/dev/i2c-1")
local addr   = cfg.addr
local sign   = tonumber(cfg.sign)

local fields = {}
for f in string.gmatch(cfg.fields or "", "[^,|]+") do
	fields[#fields + 1] = f
end

local slot = P.slot_attach(DAEMON_SLOT_ADDR)

-- setup 失败：发布错误标志一次，daemon_tick 置为空转（daemon_body 仍会逐拍调用直到 stop）
local function fail(code)
	P.publish(slot, { code })
	function daemon_tick() end
end

if not driver or driver == "" then fail(-9); return end

local ok_load = pcall(dofile, dir .. "/" .. driver .. ".lua")
if not ok_load then fail(-8); return end

-- 高速采样中静默驱动的 print 刷屏（本状态机独立）
print = function() end

-- 字段提取：sign=16 时做 16 位二进制补码符号扩展（整型路径，先取整保证
-- % 65536 可用）；未配 sign 时原样透传（浮点字段如 INA226 的 V/I/P、
-- MLX90393 的 µT 值此前被无条件向下取整丢精度，现保留小数）
local function sx(v)
	v = tonumber(v) or 0
	if sign == 16 then
		v = math.floor(v) % 65536
		if v >= 32768 then v = v - 65536 end
	end
	return v
end

-- 组装驱动参数：fd/address + 配置中的额外键（排除框架键）
local known = {
	driver = 1, proto = 1, path = 1, addr = 1, interval = 1,
	fields = 1, sign = 1, mode = 1, bits = 1, speed = 1, delay = 1, baud = 1,
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
elseif proto == "uart" then
	-- 串口设备（如 MHZ19）：raw 8N1，波特率 baud 列（缺省 9600）
	fd = uart_open(path, tonumber(cfg.baud or 9600))
else
	fd = i2c_open(path)
end
if not fd or fd < 0 then fail(-1); return end
params.fd = fd

-- 配置器件
local r = build(params)
if type(r) ~= "table" or r.result ~= "OK" then fail(-2); return end

-- daemon_body 逐拍调用：单次采样 → 提取 fields → publish
function daemon_tick()
	local s = call(params)
	if type(s) == "table" and s.result == "OK" then
		local vals = {}
		for i = 1, #fields do
			vals[i] = sx(s[fields[i]])
		end
		P.publish(slot, vals)
	end
end
