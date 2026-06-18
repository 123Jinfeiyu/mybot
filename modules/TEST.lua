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
function build_pca9685(data)

	-- 返回成功信号
	return {
		result = "OK"
	}

end

-- 【销毁PCA9685的脚本】
-- 重置PCA9685
-- NOTE(20260612-qing): 其实可以不需要，因为我们只是清空设置
function destroy_pca9685(data)


	-- 返回成功信号
	return {
		result = "OK"
	}

end


-- 【调用PCA9685】
-- 将一个舵机通道转动到一个角度
-- NOTE(20260612-qing): 接收一个参数（表），返回一个结果（表）
function call_pca9685(data)


	-- 返回字典
 	return {
		result = "OK"
	}

end



-- 【脚本主体】接收传入的参数，调用处理函数，返回结果
-- NOTE(20260612-qing): 传入的参数在 ... 中（可变参数）
--
-- 获取第一个参数（我们的字典）
local input = ...  

-- 检查该参数是否是一张表
if type(input) ~= "table" then
	return {
		result = "ERR",
		message = "Invalid Arguments: table"
	}
end


print_table(input, 4)

--
-- 取得此次调用的模式
local mode = input.mode

-- 检查mode字段是否存在
if mode == nil then
	return {
		result = "ERR",
		message = "Invalid Arguments: no mode"
	}
end

-- 根据mode的值进行行动
if mode == "build" then
	return build_pca9685(input)
elseif mode == "destroy" then
	return destroy_pca9685(input)
elseif mode == "call" then
	return call_pca9685(input)
else
	return {
		result = "ERR",
		message = "Invalid Arguments: unknow mode"
	}
end

