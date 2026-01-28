package bot4cj
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit

main(): Int64 {
    unsafe{
        var path = LibC.mallocCString("/dev/i2c-1")
        var fd: Int16 = I2C_init(path)
        println(fd)
        PCA9685_init(fd, 0x40)
        println("hello world")
        I2C_free(fd)
        LibC.free(path)
    }
    return 0
}
