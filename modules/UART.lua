-- UART 硬件（串口总线矩阵）

--
-- 加载纯 Lua 基层硬件抽象层（HAL，issue #14：不再依赖仓颉 cfunc）
-- 调试打印辅助 print_table（modules/lib/）
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/HAL.lua")
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/print_table.lua")

-- 全局文件句柄
fd = 0

-- 全局波特率（build 从元数据设置；call 随 fd 返回给引用本变量的设备驱动）
baud = 9600

function build(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	dbg_table("-------- UART.lua builder func get a table --------", data)

	local path = data.path
	if path == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: path"
		}
	end

	local uart_baud = data.baud
	if uart_baud == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: baud"
		}
	end
	print(uart_baud)

	-- 打开串口（raw 8N1 + 非阻塞；波特率白名单在 HAL 内校验）
	local opened = uart_open(path, tonumber(uart_baud))
	if opened == nil or opened < 0 then
		return {
			result = "ERR",
			message = "Failed to open UART file"
		}
	end

	fd = opened
	baud = tonumber(uart_baud)

	print("UART opened: " .. path .. " @ " .. tostring(baud) .. " baud, fd=" .. tostring(fd))
	return {  result = "OK"  }

end

-- 【销毁UART】关闭串口句柄
function destroy(data)

	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	dbg_table("-------- UART.lua ender func get a table --------", data)

	if fd ~= nil and fd > 0 then
		uart_close(fd)
		fd = 0
	end

	return {  result = "OK"  }

end


-- 【调用UART】把句柄与波特率交给引用本变量的设备驱动
-- NOTE: 接收一个参数（表），返回一个结果（表）
function call(data)

	if type(data) ~= "table" then	-- 检查该参数是否是一张表
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	dbg_table("-------- UART.lua caller func get a table --------", data)

	if fd == nil or fd <= 0 then
		return {
			result = "ERR",
			message = "UART not opened (build first)"
		}
	end

	-- 返回字典（fd 经引用机制流入设备驱动元数据）
 	return {
		result = "OK",
		fd     = fd,
		baud   = baud
	}

end
