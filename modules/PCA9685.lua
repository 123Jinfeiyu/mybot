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
	local fd = data.fd

	-- 检查文件句柄参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: fd"
		}
	end

	local addr = data.address

	-- 检查地址参数是否存在
	if addr == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: addr"
		}
	end

	addr = tonumber(addr)

	-- 设置PWM频率
	local freq = 50.0
	local prescale_val = 25000000.0 / (4096.0 * freq) - 1.0	-- 25MHz主频
	local prescale = prescale_val + 0.5

	-- 开始配置PCA9685
	i2c_set_addr(fd, addr)

	-- 重启PCA9685
	i2c_write_reg(fd, 0x00, 0x80)

	-- 等待振荡器启动
	milli_sleep(5)

	i2c_write_reg(fd, 0x00, 0x10)	-- 进入SLEEP模式（允许改频率）
	i2c_write_reg(fd, 0xFE, math.floor(prescale))
	i2c_write_reg(fd, 0x00, 0x20)	-- 退出SLEEP模式

	-- 等待振荡器启动
	milli_sleep(5)

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

	local fd = data.fd

	-- 检查fd参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: fd"
		}
	end

	local addr = data.address

	-- 检查address参数是否存在
	if addr == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments: addr"
		}
	end

	addr = tonumber(addr)

	-- 准备配置
	i2c_set_addr(fd, addr)

	-- 重启PCA9685
	i2c_write_reg(fd, 0x00, 0x80)

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


	local fd = data.fd

	-- 检查fd参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Argument: fd"
		}
	end

	local addr = data.address

	-- 检查address参数是否存在
	if addr == nil then
		return {
			result = "ERR",
			message = "Invalid Argument: addr"
		}
	end

	addr = tonumber(addr)

	local channel = data.channel

	-- 检查channel参数是否存在
	if channel == nil then
		return {
			result = "ERR",
			message = "Invalid Argument: channel"
		}
	end

	local angle = data.angle

	-- 检查angle参数是否存在
	if angle == nil then
		return {
			result = "ERR",
			message = "Invalid Argument: angle"
		}
	end

	print(string.format("Lua hardware drive set angle: %d", angle))

	-- 设置I2C地址
	i2c_set_addr(fd, addr)

	-- 计算需要的参数
	local min = 0.5	-- 0度对应0.5ms
	local max = 2.5	-- 180度对应2.5ms
	local width = min + (max - min) * (angle / 180.0)
	local offval = math.floor(width * 4096.0 / 20.0)
	local reg = 0x06 + 4 * channel

	-- 写I2C寄存器
	i2c_write_reg(fd, reg + 0, (0 >> 0)  & 0xFF)		-- 1 * 8
	i2c_write_reg(fd, reg + 1, (0 >> 8)  & 0xFF)		-- 2 * 8
	i2c_write_reg(fd, reg + 2, (offval >> 0) & 0xFF)	-- 3 * 8
	i2c_write_reg(fd, reg + 3, (offval >> 8) & 0xFF)	-- 4 * 8

	-- 返回字典
 	return {
		result = "OK"
	}

end

