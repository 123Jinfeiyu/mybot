-- I2C驱动 Linux系统 硬件脚本（测试）（作废，将这种驱动写死？）

-- 接受一个表参数，返回另一个表
-- eg: {"filename": "/dev/i2c-1"}
function build(data)

	local filename = data.filename

	-- 验证表参数中是否有设备文件名
	if filename == nil then
		return {
			result = "ERR",
			message = "Invalied Arguments"
		}
	end

	-- 打开I2C设备
	local fd = i2c_open(filename)

	-- 检查返回值是否有效
	if fd > 0 then
		return {
			result = "OK",
			fd = tostring(fd)
		}
	else
		return {
			result = "ERR",
			message = "Failed to open i2c device"
		}
	end

end


function destroy(data)

	local fd = data.fd

	-- 检查参数是否存在
	if fd == nil then
		return {
			result = "ERR",
			message = "Invalid Arguments"
		}
	end

	-- 关闭I2C设备
	i2c_close(fd)

	-- 返回一个结果
	return {
		result = "OK"
	}

end
