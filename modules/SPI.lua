-- SPI 硬件

--
-- 加载纯 Lua 基层硬件抽象层（HAL，issue #14：不再依赖仓颉 cfunc）
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/HAL.lua")

SPI_SUCCESS = 0
SPI_FAILED = 1

-- 全局文件句柄
fd = 0

-- 这个函数是辅助用的，它能够打印一张表出来
function print_table(t, indent)
    indent = indent or 0
    for k, v in pairs(t) do
        if type(v) == 'table' then
            print(string.rep(' ', indent) .. k .. ":")
            print_table(v, indent + 2)
        else
            print(string.rep(' ', indent) .. k .. ": " .. tostring(v))
        end
    end
end



function build(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	print("-------- SPI.Lua builder func get a table --------")
	print_table(data)
	print("--------------------------------------------------")
	local path = data.path

	-- 检查文件句柄参数是否存在
	if path == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: path"
		}
	end

	-- 模式
	local spi_mode = data.spi_mode
	if spi_mode == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: spi_mode"
		}
	end
	print(spi_mode)
	spi_mode = tonumber(spi_mode)

	-- 比特
	local bits = data.bits
	if bits == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: bits"
		}
	end
	bits = tonumber(bits)


	-- 速率
	local speed = data.speed
	if speed == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: speed"
		}
	end
	speed = tonumber(speed)

	--
	local delay = data.delay
	if delay == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: delay"
		}
	end
	delay = tonumber(delay)

	-- 开始执行
	fd = spi_open(path)
	if fd <= 0 then
		return {
			result = "ERR",
			message = "Failed to open SPI file"
		}
	end

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
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	print("-------- SPI.Lua ender func get a table --------")
	print_table(data)
	print(string.format("now fd: %i", fd))
	print("------------------------------------------------")

	spi_close(fd)
	fd = -1

	return {
		result = "OK"
	}

end





function call(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	print("-------- SPI.Lua caller func get a table --------")
	print_table(data)
	print("-------------------------------------------------")

	return {
		fd = fd
	}

end
