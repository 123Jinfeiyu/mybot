package bot4cj

/*
 * MPU6050传感器数据
 *
 * 因为我们可能会直接使用这些数据，需要在其中增加一些常用的方法
 */
@C
struct MPU6050_sensorData {
    var accel_x: Int16 = 0
    var accel_y: Int16 = 0
    var accel_z: Int16 = 0
    var gyro_x:  Int16 = 0
    var gyro_y:  Int16 = 0
    var gyro_z:  Int16 = 0
    public func print() {
        println("[accel x, y, z] ${accel_x}, ${accel_y}, ${accel_z}")
        println("[gyro  x, y, z] ${gyro_x},  ${gyro_y},  ${gyro_z}")
        println("--------------------")
    }
}
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit
foreign func PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit
foreign func MPU6050_readSensor(fd: Int16, data:  CPointer<MPU6050_sensorData>): Unit
foreign func MPU6050_init(fd: Int16): Unit
foreign func MPU6050_calAngle(data: CPointer<MPU6050_sensorData>, pitch: CPointer<Float32>, roll: CPointer<Float32>): Unit

/* 打开一个I2C设备 在gc回收时关闭 */
class I2C {
    public init(path: String) {
        unsafe{
            var cstr = LibC.mallocCString(path)
            fd = I2C_init(cstr)
            if (fd <= 0) { /* 打开设备失败 */
                throw Exception("Can't open i2c device")
            }
            LibC.free(cstr)
            PCA9685_init(fd, 0x40)
            MPU6050_init(fd) /* */
        }
    }
    ~init() {
        unsafe {
            I2C_free(fd)
        }
    }
    /* 控制pca9685舵机旋转一个角度 */
    public func PCA9685_turnAngle(channel: UInt8, angle: Float32) {
        unsafe {
            PCA9685_setAngle(fd, channel, angle)
        }
    }
    /* 读取MPU6050的加速度计、陀螺仪数据 */
    public func MPU6050_readData(): (Float32, Float32) {
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
}
