-- PCA9685 PWM控制板 硬件脚本（测试）




-- 配置脚本
-- FIXME(20260602-qing): 异常处理
function build_pca9685(data)

	local fd = data.fd

	-- 检查文件句柄参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments"
		}
	end

	local addr = data.address

	-- 检查地址参数是否存在
	if addr == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments"
		}
	end

	addr = tonumber(addr)

	-- 设置PWM频率
	local freq = 50.0
	local prescale_val = 25000000.0 / (4096.0 * freq) - 1.0	-- 25MHz主频
	local prescale = prescale_val + 0.5
	print(1)
	-- 开始配置PCA9685
	i2c_set_addr(fd, addr)

	print(1)
	-- 重启PCA9685
	i2c_write_reg(fd, 0x00, 0x80)

	print(1)
	-- 等待振荡器启动
	milli_sleep(5)

	print(1)
	i2c_write_reg(fd, 0x00, 0x10)	-- 进入SLEEP模式（允许改频率）
	i2c_write_reg(fd, 0xFE, math.floor(prescale))
	i2c_write_reg(fd, 0x00, 0x20)	-- 退出SLEEP模式

	print(1)
	-- 等待振荡器启动
	milli_sleep(5)

	-- 返回成功信号
	return {
		result = "OK"
	}

end

-- 销毁脚本
function destroy_pca9685(data)

	local fd = data.fd

	-- 检查fd参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments"
		}
	end

	local addr = data.address

	-- 检查address参数是否存在
	if addr == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments"
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


-- 调用脚本
-- 接收一个参数（表），返回另一个表
function call_pca9685(data)

	--local addr = 0x40
	--local fd = i2c_open("/dev/i2c-1")
	--if not fd then
	--    return {success=false, error="无法打开I2C设备 /dev/i2c-1"}
	--end
	--
	--

	local fd = data.fd

	-- 检查fd参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Argument"
		}
	end

	local addr = data.address

	-- 检查address参数是否存在
	if addr == nil then
		return {
			result = "ERR",
			message = "Invalid Argument"
		}
	end

	addr = tonumber(addr)

	local channel = data.channel

	-- 检查channel参数是否存在
	if channel == nil then
		return {
			result = "ERR",
			message = "Invalid Argument"
		}
	end

	local angle = data.angle

	-- 检查angle参数是否存在
	if angle == nil then
		return {
			result = "ERR",
			message = "Invalid Argument"
		}
	end

	i2c_set_addr(fd, addr)
	--i2c_write_reg(fd, 0x00, 0x80)  -- 重启PCA9685
	--milli_sleep(5)	-- 等待振荡器启动
	--local freq = 50.0
	--local prescale_val = 25000000.0 / (4096.0 * freq) - 1.0	-- 25MHz主频
	--local prescale = prescale_val + 0.5
	--i2c_write_reg(fd, 0x00, 0x10)	-- 进入SLEEP模式（允许改频率）
	--i2c_write_reg(fd, 0xFE, math.floor(prescale))
	--i2c_write_reg(fd, 0x00, 0x20)	-- 退出 SLEEP 模式
	--milli_sleep(5)	-- 等待振荡器启动

	--local angle = 45
	--local channel = 1
	local min = 0.5	-- 0度对应0.5ms
	local max = 2.5	-- 180度对应2.5ms
	local width = min + (max - min) * (angle / 180.0)
	local offval = math.floor(width * 4096.0 / 20.0)
	local reg = 0x06 + 4 * channel
	i2c_write_reg(fd, reg + 0, (0 >> 0)  & 0xFF)	-- 1 * 8
	i2c_write_reg(fd, reg + 1, (0 >> 8)  & 0xFF)	-- 2 * 8
	i2c_write_reg(fd, reg + 2, (offval >> 0) & 0xFF)	-- 3 * 8
	i2c_write_reg(fd, reg + 3, (offval >> 8) & 0xFF)	-- 4 * 8

	i2c_close(fd)

	-- 返回字典
 	return {
		result = "OK"
	}

end

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


-- 脚本主体：接收传入的参数，调用处理函数，返回结果
-- 注意：传入的参数在 ... 中（可变参数）
local input = ...  -- 获取第一个参数（我们的字典）

if type(input) ~= "table" then
	return {
		result = "ERR",
		message = "Invalid Arguments"
	}
end


local mode = input.mode

-- 检查mode字段是否存在
if mode == nil then
	return {
		result = "ERR",
		message = "Invalid Arguments"
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
		message = "Invalid Arguments"
	}
end

