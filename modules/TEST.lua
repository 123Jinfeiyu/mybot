-- PCA9685 PWM控制板 硬件脚本（测试）

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




-- 【配置脚本】
-- 该脚本负责初始化PCA9685
-- FIXME(20260602-qing): 异常处理
function build(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	print("-------- Lua builder func get a table --------")
	print_table(data)
	print("----------------------------------------------")

	-- 返回成功信号
	return {
		result = "OK"
	}

end

-- 【销毁PCA9685的脚本】
-- 重置PCA9685
-- NOTE(20260612-qing): 其实可以不需要，因为我们只是清空设置
function destroy(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	print("-------- Lua ender func get a table --------")
	print_table(data)
	print("--------------------------------------------")


	-- 返回成功信号
	return {
		result = "OK"
	}

end


-- 【调用PCA9685】
-- 将一个舵机通道转动到一个角度
-- NOTE(20260612-qing): 接收一个参数（表），返回一个结果（表）
function call(data)

	-- 检查该参数是否是一张表
	if type(data) ~= "table" then
		return {
			result = "ERR",
			message = "Invalid Arguments: table"
		}
	end

	print("-------- Lua caller func get a table --------")
	print_table(data)
	print("---------------------------------------------")

	-- 返回字典
 	return {
		result = "OK"
	}

end

