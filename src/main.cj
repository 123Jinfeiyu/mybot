package bot4cj
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit
foreign func PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit

main(): Int64 {
    unsafe{
        var path = LibC.mallocCString("/dev/i2c-1")
        var fd: Int16 = I2C_init(path)
        if (fd <= 0) { /* 打开设备失败 */
            return 1
        }
        LibC.free(path)
        PCA9685_init(fd, 0x40)
        PCA9685_setAngle(fd, 0, 0.0)
        I2C_free(fd)
    }
    return 0
}
