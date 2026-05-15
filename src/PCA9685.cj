/*  pca9685舵机控制版 - bot4cj
 *  TODO(20260515-qing): 由于对统一接口的需求，设计基于回调的虚拟化硬件 */


package bot4cj

class PCA9685 {
    private static const ADDR_0: UInt8 = 0x40   /* PCA9685 */
    private var i2c_setAddr: (UInt8) -> Unit
    private var i2c_readReg: (UInt8) -> Int8
    private var i2c_writeReg:(UInt8, UInt8) -> Unit
    private var addr: UInt8
    public init(i2c_setAddr!: (UInt8) -> Unit,
		i2c_writeReg!: (UInt8, UInt8) -> Unit,
		i2c_readReg!: (UInt8) -> Int8,
		addr!: UInt8 = ADDR_0) {
        this.i2c_setAddr = i2c_setAddr
        this.i2c_readReg = i2c_readReg
        this.i2c_writeReg= i2c_writeReg
        this.addr = addr
        i2c_setAddr(addr);
        i2c_writeReg(0x00, 0x20);   /* 开启AI（地址自增）和关闭SLEEP */
        sleep(5000 * Duration.nanosecond);  /* 等待振荡器启动 */
        
        set()
    }

    public func set() {
        /* 设置PWM频率为50Hz（舵机标准周期20ms） */
        var freq: Float32 = 50.0;
        var prescale_val: Float32 = 25000000.0 / (4096.0 * freq) - 1.0;    /* 25MHz主频 */
        var prescale: UInt8 = UInt8(prescale_val + 0.5);

        i2c_writeReg(0x00, 0x10);       /* 进入SLEEP模式（允许改频率） */
        i2c_writeReg(0xFE, prescale);   /* 写入预分频值 */
        i2c_writeReg(0x00, 0x20);       /* 退出SLEEP */
        sleep(5000 * Duration.nanosecond);  /* 等待振荡器启动 */
    }


    /* 设置指定通道的PWM信号（0-4095） */
    public func setPWM(channel: UInt8, on: UInt16, off: UInt16) {
        var reg: UInt8 = 0x06 + 4 * channel;   /* LED0 寄存器起始地址 */
        i2c_writeReg(reg,     UInt8(on & 0xFF));
        i2c_writeReg(reg + 1, UInt8(on >> 8));
        i2c_writeReg(reg + 2, UInt8(off & 0xFF));
        i2c_writeReg(reg + 3, UInt8(off >> 8));
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
        i2c_setAddr(this.addr);
        this.setPWM(channel, 0, off_val); /* ON=0, OFF=计算值 */
    }
}
