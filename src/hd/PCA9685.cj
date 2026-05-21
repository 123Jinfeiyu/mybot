package bot4cj.hd
import bot4cj.th.*

/*  pca9685舵机控制版 - bot4cj
 *  TODO(20260515-qing): 由于对统一接口的需求，设计基于回调的虚拟化硬件
 *  TODO(20260516-qing): 获取pca9685的中文或者英文参考资料  */
public class PCA9685 {
	private static const ADDR_0: UInt8 = 0x40   /* PCA9685的i2c地址 */
	//private var i2c_setAddr: (UInt8) -> Unit
	//private var i2c_readReg: (UInt8) -> Int8
	//private var i2c_writeReg:(UInt8, UInt8) -> Unit
	private let i2c_fd: Int32
	private let addr: UInt8
	public init(//i2c_setAddr!: (UInt8) -> Unit,		/* 设置通信地址 - 回调函数 */
		//	i2c_writeReg!: (UInt8, UInt8) -> Unit,	/* 写i2c地址 - 回调函数 */
		//	i2c_readReg!: (UInt8) -> Int8,		/* 读i2c地址 - 回调函数 */
			i2c_fd: Int32,
			addr!: UInt8 = ADDR_0) {		/* PCA9685硬件地址 */
		//this.i2c_setAddr = i2c_setAddr
		//this.i2c_readReg = i2c_readReg
		//this.i2c_writeReg= i2c_writeReg
		this.i2c_fd = i2c_fd
		this.addr = addr
		/* -------- 初始化PCA9685 --------
		 * FIXME(20260517-qing): 在旧版机器人代码移除以后，移除这一段 */

	}

	/*  -------- 设置PWM频率为50Hz（舵机标准周期20ms0） --------
	 *  TODO(20260516-qing): 不将数值写死 */
	public func setup() {
		unsafe {
			I2cLock_lock(i2c_fd) 
			setI2cAddr(i2c_fd, addr);
			writeI2cReg(i2c_fd, 0x00, 0x20);   /* 开启AI（地址自增）和关闭SLEEP */
			sleep(5000 * Duration.nanosecond);	/* 等待振荡器启动 */
        					/* FIXME: 这一条是不是重复了 */
			var freq: Float32 = 50.0;
			var prescale_val: Float32 = 25000000.0 / (4096.0 * freq) - 1.0;    /* 25MHz主频 */
			var prescale: UInt8 = UInt8(prescale_val + 0.5);

			writeI2cReg(i2c_fd, 0x00, 0x10);       /* 进入SLEEP模式（允许改频率） */
			writeI2cReg(i2c_fd, 0xFE, prescale);   /* 写入预分频值 */
			writeI2cReg(i2c_fd, 0x00, 0x20);       /* 退出SLEEP */

			I2cLock_unlock(i2c_fd)
		}
		sleep(5000 * Duration.nanosecond);  /* 等待振荡器启动 */
	}


	/* -------- 设置指定通道的PWM信号（0-4095）--------  */
	public func setPWM(channel: UInt8, on: UInt16, off: UInt16) {
		var reg: UInt8 = 0x06 + 4 * channel;   /* LED0 寄存器起始地址 */
		unsafe {
			writeI2cReg(i2c_fd, reg,     UInt8(on & 0xFF));
			writeI2cReg(i2c_fd, reg + 1, UInt8(on >> 8));
			writeI2cReg(i2c_fd, reg + 2, UInt8(off & 0xFF));
			writeI2cReg(i2c_fd, reg + 3, UInt8(off >> 8));
		}
		return
	}

	/* -------- 角度转PWM值（0°~180° → 0.5ms~2.5ms脉宽） -------- */
	public func setAngle(channel: UInt8, angle: Float32) {
		const min: Float32 = 0.5;    /* 0°对应0.5ms */
		const max: Float32 = 2.5;    /* 180°对应2.5ms */
		let width: Float32 = min + (max - min) * (angle / 180.0);
		let off_val: UInt16 = UInt16(width * 4096.0 / 20.0);        /* 20ms 周期 */
		unsafe {
			I2cLock_lock(i2c_fd)
			setI2cAddr(i2c_fd, this.addr);
			this.setPWM(channel, 0, off_val); /* ON=0, OFF=计算值 */
			I2cLock_unlock(i2c_fd)
		}
	}
}
