-- I2C 硬件

--
-- 加载纯 Lua 基层硬件抽象层（HAL，issue #14：不再依赖仓颉 cfunc）
dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/HAL.lua")

I2C_SUCCESS = 0
I2C_FAILED  = 1

-- 全局文件句柄
fd = -1

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
	print("-------- I2C.Lua builder func get a table --------")
	print_table(data)
	print("--------------------------------------------------")

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
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid param Type"
		}
	end

	i2c_close(fd)
	fd = -1

	return {  result = "OK"  }

end





function call(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Params Type"
		}
	end

	return {  fd = fd  }

end
