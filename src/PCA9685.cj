/* pca9685舵机控制版 - bot4cj */


package bot4cj

class PCA9685 {
    private static const ADDR_0: UInt8 = 0x40
    private var i2c: Option<I2C>
    public init(i2c: I2C, addr!: UInt8 = ADDR_0) {
        this.i2c = i2c
        i2c.setAddr(addr);
        i2c.writeReg(0x00, 0x20);   /* 开启AI（地址自增）和关闭SLEEP */
        sleep(5000 * Duration.millisecond);  /* 等待振荡器启动 */
        
        set()
    }
    public func set() {
        let i2c = this.i2c.getOrThrow()
        /* 设置PWM频率为50Hz（舵机标准周期20ms） */
        var freq: Float32 = 50.0;
        var prescale_val: Float32 = 25000000.0 / (4096.0 * freq) - 1.0;    /* 25MHz主频 */
        var prescale: UInt8 = UInt8(prescale_val + 0.5);

        i2c.writeReg(0x00, 0x10);       /* 进入SLEEP模式（允许改频率） */
        i2c.writeReg(0xFE, prescale);   /* 写入预分频值 */
        i2c.writeReg(0x00, 0x20);       /* 退出SLEEP */
        sleep(5000 * Duration.millisecond);  /* 等待振荡器启动 */
    }
}
