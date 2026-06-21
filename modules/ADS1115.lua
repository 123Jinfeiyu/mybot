-- ADS1115模数转换器 硬件脚本（测试）


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

ADS1115_ADDR		= 0x48		-- ADS1115的I2C地址
ADS1115_REG_CONVERSION	= 0x00	-- 转换结果（只读）
ADS1115_REG_CONFIG	= 0x01	-- 配置寄存器（读/写）
ADS1115_REG_LO_THRESH	= 0x02	-- 低阈值比较
ADS1115_REG_HI_THRESH	= 0x03	-- 高阈值比较
-- ====================================
-- ADS1115 配置寄存器 bit 定义（16位）
-- ====================================
-- BIT15:	OS[0]		=> 单次转换启动位（1=开始）
-- BIT14-12:	MUX[2:0]	=> 输入多路选择
-- BIT11-9:	PGA[2:0]	=> 可编程增益放大器
-- BIT8:	MODE		=> 0=连续转换，1=单次（省电）
-- BIT7-5:	DR[2:0]		=> 数据速率
-- BIT4:	COMP_MODE	=> 比较模式（0=传统)
-- BIT3:	COMP_POL	=> 比较极性（0=低有效）
-- BIT2:	COMP_LAT	=> 比较锁存（0=不锁存）
-- BIT1-0:	COMP_QUE	=> 比较器队列（11=禁用）
--
-- ==== MUX 输入通道 ====
ADS1115_MUX_AIN0_AIN1		= 0x0000	-- 差分：P=AIN0，N=AIN1（默认）
ADS1115_MUX_AIN0_AIN3		= 0x1000	-- 差分：P=AIN0，N=AIN3
ADS1115_MUX_AIN1_AIN3		= 0x2000	-- 差分：P=AIN1，N=AIN3
ADS1115_MUX_AIN2_AIN3		= 0x3000	-- 差分：P=AIN2，N=AIN3
ADS1115_MUX_AIN0_GND		= 0x4000	-- 单端：AIN0
ADS1115_MUX_AIN1_GND		= 0x5000	-- 单端：AIN1
ADS1115_MUX_AIN2_GND		= 0x6000	-- 单端：AIN2
ADS1115_MUX_AIN3_GND		= 0x7000  -- 单端：AIN3

-- ==== PGA 增益（满量程范围）====
ADS1115_PGA_6_144V		= 0x0000	-- ±6.144V (1LSB=0.1875mV)
ADS1115_PGA_4_096V		= 0x0200	-- ±4.096V (1LSB=0.125mV)
ADS1115_PGA_2_048V		= 0x0400	-- ±2.048V (1LSB=0.0625mV)(默认)
ADS1115_PGA_1_024V		= 0x0600	-- ±1.024V (1LSB=0.03125mV)
ADS1115_PGA_0_512V		= 0x0800	-- ±0.512V (1LSB=0.015625mV)
ADS1115_PGA_0_256V		= 0x0A00	-- ±0.256V (1LSB=0.0078125mV)

-- ==== 数据速率 ====
ADS1115_DR_8SPS			= 0x0000	-- 8 SPS
ADS1115_DR_16SPS		= 0x0020	-- 16 SPS
ADS1115_DR_32SPS		= 0x0040	-- 32 SPS
ADS1115_DR_64SPS		= 0x0060	-- 64 SPS
ADS1115_DR_128SPS		= 0x0080	-- 128 SPS (默认)
ADS1115_DR_250SPS		= 0x00A0	-- 250 SPS
ADS1115_DR_475SPS		= 0x00C0	-- 475 SPS
ADS1115_DR_860SPS		= 0x00E0	-- 860 SPS

-- ==== 比较器队列（COMP_QUE）====
ADS1115_COMP_QUE_1		= 0x0000	-- 1次后触发
ADS1115_COMP_QUE_2		= 0x0001	-- 2次后触发
ADS1115_COMP_QUE_4		= 0x0002	-- 4次后触发
ADS1115_COMP_QUE_DISABLE	= 0x0003	-- 禁用比较器(默认)

-- ==== 操作模式 ====
ADS1115_MODE_CONTINUOUS		= 0x0000	-- 连续转换
ADS1115_MODE_SINGLE		= 0x0100	-- 单次转换（默认，省电）

-- ==== 启动转换 ====
ADS1115_OS_START		= 0x8000	-- 启动单次转换

-- ADS1115 配置上下文
ads1115_t = {
	addr = 0x00,
	config = 0x00,		-- 16bits
	fs_valtage = 0.0	-- 满量程电压（取决于PGA）
}

-- PGA对应的满量程电压（单位：V）
pga_fs = {
	6.144,	-- ADS1115_PGA_6_144V [0]
	4.096,	-- ADS1115_PGA_4_096V [1]
	2.048,	-- ADS1115_PGA_2_048V [2]
	1.024,	-- ADS1115_PGA_1_024V [3]
	0.512,	-- ADS1115_PGA_0_512V [4]
	0.256	-- ADS1115_PGA_0_256V [5]
}

-- PGA 增益对应的满量程电压名
pga_names = {
	"±6.144V",	-- [0]
	"±4.096V",	-- [1]
	"±2.048V",	-- [2]
	"±1.024V",	-- [3]
	"±0.512V",	-- [4]
	"±0.256V"	-- [5]
}

-- 数据速率字符串
dr_names = {
	"8 SPS",	-- [0]
	"16 SPS",	-- [1]
	"32 SPS",	-- [2]
	"64 SPS",	-- [3]
	"128 SPS",	-- [4]
	"250 SPS",	-- [5]
	"475 SPS",	-- [6]
	"860 SPS"	-- [7]
}



-- I2C写2字节（大端）
function i2c_write_word(fd, reg, val)
	i2c_write_reg(fd, reg + 0, (val >> 8) | 0xFF)
	i2c_write_reg(fd, reg + 1, val & 0xFF)
end

--
-- 【配置脚本】
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

	-- 写进配置表
	ads1115.addr = addr


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

