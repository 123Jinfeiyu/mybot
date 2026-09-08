-- I2C 硬件

--
-- 加载纯 Lua 基层硬件抽象层（HAL，issue #14：不再依赖仓颉 cfunc）
-- 调试打印辅助 print_table（modules/lib/）
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/HAL.lua")
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/print_table.lua")
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/drv_common.lua")

-- 全局文件句柄
fd = -1

function build(data)

	-- 检查该参数是否是一张表
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end
	dbg_table("-------- I2C.Lua builder func get a table --------", data)

	local path = data.path
	if path == nil then	-- 检查文件句柄参数是否存在
		return {
			result = "ERR",
			message = "No I2C path"
		}
	end


	-- 开始执行
	fd = i2c_open(path)
	if fd <= 0 then
		return {
			result = "ERR",
			message = "Failed to open I2C file"
		}
	end

	
	-- 返回结果
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

	i2c_close(fd)
	fd = -1

	return {  result = "OK"  }

end





function call(data)

	-- 检查该参数是否是一张表
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	return {  fd = fd  }

end
