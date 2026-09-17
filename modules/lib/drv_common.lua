-- drv_common.lua - 驱动公共样板（modules/lib/）
-- bot4cj 仓颉语言机器人框架
--
-- 收敛 Low 项"样板重复"：各 I2C 驱动内联重复的开头样板：
--   1. type(data) ~= "table" 参数校验（build/call/destroy 每函数一份）
--   2. fd/address 存在性 + i2c_set_addr 校验（build 与 call 双份）
--   3. { result = "ERR", message = ... } 返回表构造
-- 行为与各驱动旧内联版本等价；调用点语义不变。
-- 驱动顶部在 HAL 之后 dofile 加载；依赖 i2c_set_addr（HAL）在此运行时可解析。
-- 幂等：同状态机内只定义一次（多个驱动文件 dofile 本库时安全）。
-- 用本库私有标记判断重复加载——若用 arg_table 等全局函数的存在性判断，
-- 状态机「先定义了同名全局函数但缺 err/i2c_pair」时会整体 return，消费者则崩溃。
if __BOT4CJ_DRV_COMMON_LOADED ~= nil then return end
__BOT4CJ_DRV_COMMON_LOADED = true

-- 统一 ERR 返回契约（与各驱动旧内联 {result="ERR",message=...} 一致）
function err(msg)
	return {  result = "ERR",  message = msg  }
end

-- ---- 非守护路径的虚拟总线前置（proto=virt 的设备行）---------------------
-- 背景：注入虚拟总线此前只发生在 sensor_daemon.lua（守护路径）里，所以全部
-- virt 端到端用例都是守护型，@Action 直接驱动的写路径（舵机/DAC）无从仿真。
-- 挂在 arg_table 上是因为它是所有驱动 build/call/destroy 的第一行——本状态机内
-- 必须在驱动碰总线之前完成注入，且不需要改动任何驱动文件。
local virt_state = "idle"		-- idle → done | error:<原因>；每状态机只尝试一次

-- 返回 nil 表示无需动作或已就绪；返回字符串表示必须中止本次驱动调用
local function virt_setup(data)
	local base = (os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+")
	local ok, vbus = pcall(dofile, base .. "/lib/virtbus.lua")
	if not ok or type(vbus) ~= "table" then
		virt_state = "error:cannot load lib/virtbus.lua"
		return virt_state:sub(7)
	end
	-- 守护路径已注入过：模型实例归它管，这里再 attach 一份会出现
	-- 「守护泵 A 实例、驱动写 B 实例」的静默错位，故整体跳过。
	if vbus._injected then virt_state = "done"; return nil end

	-- 模型按约定发现：本状态机只加载了一个驱动文件，其内嵌全局至多一个 *_VIRT
	local model, driver
	for k in pairs(_G) do
		if type(k) == "string" then
			local n = k:match("^(.+)_VIRT$")
			if n then model, driver = rawget(_G, k), n end
		end
	end

	local addr = tonumber(data.address)
	if addr ~= nil and model == nil then
		-- 设备行要求仿真却拿不到模型：绝不静默回落到真实总线（那会让
		-- 「无硬件跑通」的测试悄悄打到真机舵机上），必须报错中止。
		virt_state = "error:" .. tostring(driver or "device")
			.. " declared proto=virt but no embedded <DRIVER>_VIRT model was found"
		return virt_state:sub(7)
	end

	if addr ~= nil then
		-- attach 器件模型只属于带 address 的设备行；总线行（I2C/SPI/UART）无
		-- address，跳过 attach，仅靠下面的 inject 让自己的 open 变成虚拟 open。
		vbus.attach("VIRT:" .. driver .. "@" .. tostring(addr), addr, model)
	end
	-- fd 由总线变量（modules/I2C.lua）在它自己的状态机里 open 出来；本状态机要
	-- 先把这个进程级整数登记进本地 virtbus，否则 i2c_set_addr 认不出该句柄。
	if data.fd ~= nil then vbus.adopt_fd(data.fd, data.path, addr) end
	vbus.inject()
	virt_state = "done"

	print(" [drv_common] *** VIRTUAL HARDWARE -- NO REAL DEVICE *** ("
		.. tostring(addr and driver or (data.name or "bus"))
		.. (addr and (" @0x" .. string.format("%X", addr)) or "") .. ")")
	return nil
end

-- 请求参数必须是表；否则返回 ERR
-- 用法：local ok, e = arg_table(data); if not ok then return e end
-- 附带一次性的虚拟总线前置（见上），故必须保持在各驱动入口的首个调用位置。
function arg_table(data)
	if type(data) ~= "table" then
		return false, err("Invalid Arguments Type: table")
	end
	if virt_state ~= "idle" then
		if virt_state ~= "done" then
			-- 前置曾失败：每次调用都重放同一原因，绝不放行到总线调用
			return false, err("proto=virt setup failed: " .. virt_state:sub(7))
		end
	elseif data.proto == "virt" then
		local why = virt_setup(data)
		if why then return false, err("proto=virt setup failed: " .. why) end
	else
		virt_state = "done"		-- 真机路径：一次比较即永久关闭本前置
	end
	return true, nil
end

-- I2C 传感器 build/call 公共前置：fd/address 存在性 + set_addr
-- 用法：local fd, addr, e = i2c_pair(data); if fd == nil then return e end
-- 返回：成功 => (fd, addr, nil)；失败 => (nil, -1, err表)
-- 原各驱动 build 与 call 双份校验语义一致，统一于此
function i2c_pair(data)
	local fd = tonumber(data.fd)
	local addr = tonumber(data.address)
	if fd == nil or addr == nil then
		return nil, -1, err("Invalid Argument: fd or address")
	end
	if not i2c_set_addr(fd, addr) then
		return nil, -1, err("Invalid Argument: fd or address (set_addr failed)")
	end
	return fd, addr, nil
end