-- test_virtbus_bh1750.lua - 真实驱动在虚拟总线上的链路回归（issue #18）
-- 用 virtbus 虚拟 I2C 总线加载【真实】BH1750 驱动，无真机完成：
--   build（PowerOn + 模式命令）→ call（读照度换算 LUX）→ destroy（PowerDown），
--   并覆盖"器件不在总线"的 NACK 负路径（build 应显式报 ERR）。
-- 依赖：LuaJIT（HAL 要求 ffi，BH1750 dofile HAL 时加载真 ffi 库；
--       milli_sleep 走真 usleep，无硬件副作用）。
-- 用法（项目根目录，LuaJIT）: luajit test/test_virtbus_bh1750.lua（run_all.sh 自动纳入）
local tester = dofile("test/lib/tester.lua")
local vbus = dofile("modules/lib/virtbus.lua")
vbus.reset()

-- 虚拟 BH1750（地址 0x23）：单字节命令器件，纯读 2 字节照度计数值
local lux_count = 30000                     -- 0x7530 → mode H 下 25000 lux
local cmds = {}
vbus.attach("bh1750@0x23", 0x23, {
	write = function(bytes)
		if #bytes == 1 then cmds[#cmds + 1] = bytes[1] end
	end,
	read = function(n)
		return { math.floor(lux_count / 256) % 256, lux_count % 256 }
	end,
})
vbus.inject()

local mpath = dofile("modules/lib/mpath.lua")
dofile("modules/BH1750.lua")
-- BH1750 加载时会重新加载 HAL，覆盖 virtbus 注入，需要恢复后重新注入
vbus.restore()
vbus.inject()

-- build：应在位判定成功（PowerOn 0x01 / 连续 H 0x10 写成功）
local fd = i2c_open("/dev/i2c-1")
cmds = {}
local res = build({ fd = fd, address = 0x23, mode = "H" })
tester.check("build 在位判定成功返回 OK", tostring(res.result) == "OK")
tester.check("build 发出 PowerOn 命令 0x01", cmds[1] == 0x01)
tester.check("build 发出连续 H 命令 0x10", cmds[2] == 0x10)

-- call：读回 0x7530 → LUX = count/1.2 = 25000
res = call({ fd = fd, address = 0x23, mode = "H" })
tester.check("call 返回 OK", tostring(res.result) == "OK")
tester.check("call 照度换算正确 (30000/1.2)", math.abs(res.LUX - 25000) < 1e-6)

-- destroy：发 PowerDown 0x00
cmds = {}
res = destroy({ fd = fd, address = 0x23 })
tester.check("destroy 返回 OK", tostring(res.result) == "OK")
tester.check("destroy 发出 PowerDown 0x00", cmds[#cmds] == 0x00)

-- 负路径：地址不在总线（0x50 无器件）→ build 写 NACK → 显式 ERR
res = build({ fd = fd, address = 0x50, mode = "H" })
tester.check("地址无器件 build 显式 ERR", tostring(res.result) == "ERR")

i2c_close(fd)
vbus.restore()

tester.summary("test_virtbus_bh1750")
return 0