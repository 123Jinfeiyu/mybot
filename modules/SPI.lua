-- SPI 硬件

--
-- 加载纯 Lua 基层硬件抽象层（HAL，issue #14：不再依赖仓颉 cfunc）
-- 调试打印辅助 print_table（modules/lib/）
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/HAL.lua")
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/print_table.lua")
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/drv_common.lua")

-- 全局文件句柄
fd = 0

-- 全局传输延时（build 从元数据设置；call 随 fd 返回给引用本变量的设备驱动）
delay_us = 0

function build(data)

	-- 检查该参数是否是一张表
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dbg_table("-------- SPI.Lua builder func get a table --------", data)
	local path = data.path

	-- 检查文件句柄参数是否存在
	if path == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: path"
		}
	end

	-- 模式（0~3：CPOL/CPHA 组合，与 Linux spidev 一致）
	local spi_mode = tonumber(data.spi_mode)
	if spi_mode == nil or spi_mode < 0 or spi_mode > 3 then
		return {
			result = "ERR",
			message = "Invalid Arguments: spi_mode (0~3)"
		}
	end

	-- 比特（每字位数，7~16）
	local bits = tonumber(data.bits)
	if bits == nil or bits < 7 or bits > 16 then
		return {
			result = "ERR",
			message = "Invalid Arguments: bits (7~16)"
		}
	end


	-- 速率（Hz，必须为正）
	local speed = tonumber(data.speed)
	if speed == nil or speed <= 0 then
		return {
			result = "ERR",
			message = "Invalid Arguments: speed (>0)"
		}
	end

	-- 延时（单次传输后 CS 拉高前等待，us，非负）
	local delay = tonumber(data.delay)
	if delay == nil or delay < 0 then
		return {
			result = "ERR",
			message = "Invalid Arguments: delay (>=0)"
		}
	end

	-- 开始执行
	fd = spi_open(path)
	if fd <= 0 then
		return {
			result = "ERR",
			message = "Failed to open SPI file"
		}
	end

	delay_us = delay or 0
	local res = spi_setup(fd, spi_mode, bits, speed, delay)
	if res ~= 0  then
		return {
			result = "ERR",
			message = "Failed to setup SPI device"
		}
	end

	return {
		result = "OK",
		fd = fd
	}

end




function destroy(data)

	-- 检查该参数是否是一张表
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dbg_table("-------- SPI.Lua ender func get a table --------", data)

	spi_close(fd)
	fd = -1

	return {
		result = "OK"
	}

end





function call(data)

	-- 检查该参数是否是一张表
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dbg_table("-------- SPI.Lua caller func get a table --------", data)

	return {
		fd = fd,
		delay = delay_us
	}

end
