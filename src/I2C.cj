package bot4cj
@C
struct MPU6050_sensorData {
    var accel_x: Int16 = 0
    var accel_y: Int16 = 0
    var accel_z: Int16 = 0
    var gyro_x:  Int16 = 0
    var gyro_y:  Int16 = 0
    var gyro_z:  Int16 = 0
}
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit
foreign func PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit
foreign func MPU6050_readSensor(fd: Int16, data:  CPointer<MPU6050_sensorData>): Unit
foreign func MPU6050_calAngle(data: CPointer<MPU6050_sensorData>, pitch: CPointer<Float32>, roll: CPointer<Float32>): Unit
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
        }
    }
    public func PCA9685_turnAngle(channel: UInt8, angle: Float32) {
        unsafe {
            PCA9685_setAngle(fd, channel, angle)
        }
    }
    public func MPU6050_calAngle(): (Float32, Float32) {
        unsafe {
            var pitch: Float32 = 0.0
            var roll: Float32  = 0.0
            var pData = LibC.malloc<MPU6050_sensorData> ()
            MPU6050_readSensor(fd, pData)
            MPU6050_calAngle(pData, inout pitch, inout roll)
            LibC.free(pData)
            return (pitch, roll)
        }
    }
    ~init() {
        unsafe {
            I2C_free(fd)
        }
    }
    private var fd: Int16
}
