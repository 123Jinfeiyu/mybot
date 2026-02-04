package bot4cj
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func I2C_setAddr(fd: Int16, addr: UInt8): Bool
foreign func I2C_writeReg(fd: Int16, reg: UInt8, val: UInt8): Bool
foreign func I2C_readReg(fd: Int16, reg: UInt8): UInt8
foreign func I2C_readRegFlag(fd: Int16, reg: UInt8): Int8
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit
foreign func PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit
foreign func MPU6050_readSensor(fd: Int16, data:  CPointer<MPU6050_sensorData>): Unit
foreign func MPU6050_init(fd: Int16): Unit
foreign func MPU6050_calAngle(data: CPointer<MPU6050_sensorData>, pitch: CPointer<Float32>, roll: CPointer<Float32>): Unit

/*
 * 打开一个I2C设备 在gc回收时关闭
 *
 *     对I2C的操作都要加锁
 */
class I2C {
    public init(path: String) {
        this.path = path
        unsafe{
            var cstr = LibC.mallocCString(path)
            fd = I2C_init(cstr)
            if (fd <= 0) { /* 打开设备失败 */
                throw Exception("Can't open i2c device")
            }
            LibC.free(cstr)
        }
    }
    ~init() {
        unsafe {
            I2C_free(fd)
        }
    }

    /* 设置I2C设备地址 */
    public func setAddr(addr: UInt8): Unit {
        unsafe {
            if (I2C_setAddr(fd, addr)) {
                throw I2cSetAddressFailed(path)
            }
        }
    }

    /* 写入从机寄存器 */
    public func writeReg(reg: UInt8, val:UInt8): Unit {
        unsafe {
            if (I2C_writeReg(fd, reg, val)) {
                throw I2cWriteRegFailed(path)
            }
        }
    }

    /* 读取I2C从机寄存器 */
    public func readReg(reg: UInt8): UInt8 {
        unsafe {
            return I2C_readReg(fd, reg)
        }
    }

    /* 读取I2C从机寄存器 - 带符号 */
    public func readRegFlag(reg: UInt8): Int8 {
        unsafe {
            return I2C_readRegFlag(fd, reg)
        }
    }
    /* --------------------
     * 初始化pca9685舵机控制板 */
    public func pca9685_init() {
        unsafe {
            PCA9685_init(fd, 0x40)
        }
    }
    /* 控制pca9685舵机旋转一个角度 */
    public func pca9685_turnAngle(channel: UInt8, angle: Float32) {
        unsafe {
            PCA9685_setAngle(fd, channel, angle)
        }
    }
    /* --------------------
     * 初始化mpu6050陀螺仪与加速度计 */
    public func mpu6050_init() {
        unsafe {
            MPU6050_init(fd) /* */
        }
    }
    /* 读取MPU6050的加速度计、陀螺仪数据 */
    public func mpu6050_readData(): MPU6050_sensorData {
        unsafe {
            var pData = LibC.malloc<MPU6050_sensorData> ()
            MPU6050_readSensor(fd, pData)
            let data = pData.read()
            LibC.free(pData)
            return data
        }
    }

    /* 存放文件描述符 */
    private var fd: Int16
    /* 存放设备文件地址 */
    private var path: String
}

/* I2C设备异常 */
open private class I2cExcept <: Exception {
    public I2cExcept(dev: String, msg: String) {
        super('${dev}: ${msg}')
    }
}

/* 打开I2C设备失败 */
private class I2cOpenedFailed <: I2cExcept {
    public I2cOpenedFailed(dev: String) {
        super(dev, "failed to open i2c device")
    }
}

/* I2C设置地址失败 */
private class I2cSetAddressFailed <: I2cExcept {
    public I2cSetAddressFailed(dev: String) {
        super(dev, "failed to open i2c device")
    }
}

/* I2C写从机I2C失败 */
private class I2cWriteRegFailed <: I2cExcept {
    public I2cWriteRegFailed(dev: String) {
        super(dev, "failed to write i2c reg")
    }
}
