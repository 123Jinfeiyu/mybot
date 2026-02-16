/* pca9685舵机控制版 - bot4cj */


package bot4cj

class PCA9685 {
    private static const ADDR_0: UInt8 = 0x40
    private var i2c: Option<I2C>
    private var addr: UInt8
    public init(i2c: I2C, addr!: UInt8 = ADDR_0) {
        this.i2c = i2c
        this.addr = addr
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


    /* 设置指定通道的PWM信号（0-4095） */
    public func setPWM(channel: UInt8, on: UInt16, off: UInt16) {
        var reg: UInt8 = 0x06 + 4 * channel;   /* LED0 寄存器起始地址 */
        let i2c = this.i2c.getOrThrow()
        i2c.writeReg(reg,     UInt8(on & 0xFF));
        i2c.writeReg(reg + 1, UInt8(on >> 8));
        i2c.writeReg(reg + 2, UInt8(off & 0xFF));
        i2c.writeReg(reg + 3, UInt8(off >> 8));
        return
    }

    /*
     * 角度转PWM值（0°~180° → 0.5ms~2.5ms脉宽）
     */
    public func setAngle(channel: UInt8, angle: Float32) {
        const min: Float32 = 0.5;    /* 0°对应0.5ms */
        const max: Float32 = 2.5;    /* 180°对应2.5ms */
        let width: Float32 = min + (max - min) * (angle / 180.0);
        let off_val: UInt16 = UInt16(width * 4096.0 / 20.0);        /* 20ms 周期 */
        let i2c = this.i2c.getOrThrow()
        i2c.setAddr(this.addr);
        this.setPWM(channel, 0, off_val); /* ON=0, OFF=计算值 */
    }
}
