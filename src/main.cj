package bot4cj
foreign func I2C_init(path: CString): Bool
foreign func I2C_free(): Unit
main(): Int64 {
    unsafe{
        var path = LibC.mallocCString("/dev/i2c-1")
        I2C_init(path)
        println("hello world")
        I2C_free()
        LibC.free(path)
    }
    return 0
}
