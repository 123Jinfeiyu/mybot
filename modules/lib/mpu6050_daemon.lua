-- mpu6050_daemon.lua - MPU6050 六轴采样守护 chunk（Stage 2：真实传感器）
-- 运行在 @Daemon 的独立 lua_State 上：复用 MPU6050.lua 驱动读六轴，符号扩展后
-- 经 pthread.lua publish 到共享槽，主线程 play() 加锁读取。
--
-- 配置（来自 @Daemon 矩阵除 name/chunk 外的列，注入为 DAEMON_CONFIG "k=v;" 串）：
--   i2c      I2C 总线路径（默认 /dev/i2c-1）
--   addr     MPU6050 地址（默认 0x68）
--   interval 采样间隔 ms（默认 10 = 100Hz）

if jit then jit.off() end

local dir = os.getenv("BOT4CJ_MODULE_PATH") or "./modules"
local P = dofile(dir .. "/lib/pthread.lua")
dofile(dir .. "/lib/HAL.lua")		-- i2c_* / milli_sleep
dofile(dir .. "/MPU6050.lua")		-- 复用驱动：得到全局 build/call/destroy

-- 解析配置串 "k=v;k=v"
local cfg = {}
for k, v in string.gmatch(DAEMON_CONFIG or "", "([^;=]+)=([^;]*)") do
	cfg[k] = v
end
local bus      = cfg.i2c or "/dev/i2c-1"
local addr     = cfg.addr or "0x68"
local interval = tonumber(cfg.interval) or 10

local slot = P.slot_attach(DAEMON_SLOT_ADDR)

-- 高速采样循环中静默驱动的 print_table 刷屏（本状态机独立，不影响他处）
print = function() end

-- 16 位二进制补码符号扩展（MPU6050 轴数据为有符号）
local function s16(v)
	v = math.floor(tonumber(v) or 0) % 65536
	if v >= 32768 then v = v - 65536 end
	return v
end

local fd = i2c_open(bus)
if fd < 0 then
	P.publish(slot, { -1 })		-- 标志：总线打开失败
	return
end

local r = build({ mode = "build", fd = fd, address = addr })
if type(r) ~= "table" or r.result ~= "OK" then
	P.publish(slot, { -2 })		-- 标志：传感器配置失败
	i2c_close(fd)
	return
end

while not P.should_stop(slot) do
	local s = call({ mode = "call", fd = fd, address = addr })
	if type(s) == "table" and s.result == "OK" then
		P.publish(slot, {
			s16(s.ACCEL_X), s16(s.ACCEL_Y), s16(s.ACCEL_Z),
			s16(s.GYRO_X),  s16(s.GYRO_Y),  s16(s.GYRO_Z),
		})
	end
	milli_sleep(interval)
end

destroy({ mode = "destroy", fd = fd, address = addr })
i2c_close(fd)
