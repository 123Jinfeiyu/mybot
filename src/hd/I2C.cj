package bot4cj.hd
//foreign func I2C_init(path: CString): Int16
//foreign func I2C_free(fd: Int16): Unit
//foreign func I2C_setAddr(fd: Int16, addr: UInt8): Bool
//foreign func I2C_writeReg(fd: Int16, reg: UInt8, val: UInt8): Bool
//foreign func I2C_readReg(fd: Int16, reg: UInt8): UInt8
//foreign func I2C_readRegFlag(fd: Int16, reg: UInt8): Int8
//const I2C_FILE_NOT_OPENED: Int16 = -1


/* -------- 我们将直接调用glibc中的函数 --------
 * TODO(20260518-qing):  稍后我们专门做一个i2c库，这样就能够区分不同架构上面的int的大小  */
foreign func open(path: CString, flag: Int32, ...): Int32
foreign func close(fd: Int32): Int32	/* 成功返回0，失败返回-1 */
foreign func ioctl(fd: Int32, addr: UInt64, ...): Int32	/* 成功返回非负整数，失败返回-1 */
foreign func read(fd: Int32, buf: CPointer<Unit>, count: UIntNative): IntNative
foreign func write(fd: Int32, buf: CPointer<Unit>, count: UIntNative): IntNative


@C
func getPtr(p: CPointer<UInt8>): CPointer<Unit> {
	return CPointer<Unit>(p)
}
public class I2C_Orig {
	private static let O_RDWR = 2i32
	private static let I2C_FAILED = -1i32
	private static let I2C_SUCCESS = 0i32
	private static let I2C_SLAVE = 0x0703u64	/* at least on arm64 */
	private var fd: Int32 = -1
	private var path: String
	public init (path: String) {
		this.path = path
		let s1 = unsafe { LibC.mallocCString(path) }
		let fd = unsafe { open(s1, O_RDWR) }
		if (fd > 0) { this.fd = fd }
		else { throw I2cOpenedFailed(path) }
	}

	public func _close(): Unit {
		let ret = unsafe { close(fd) }
		if (ret == I2C_FAILED) {
			throw I2cClosedFailed(path) }
		else if (ret == I2C_SUCCESS) {
			;  }
		else { throw I2cUnknowExp(path, "$function returned: {ret.toString()}") }
	}

	public func setAddr(addr: UInt8) {
		let ret  = unsafe {  ioctl(fd, I2C_SLAVE, addr)  }
		if (ret < 0) { throw I2cSetAddressFailed(path) }
	}

	public func writeReg(reg: UInt8, val:UInt8): Unit {
		var a: VArray<UInt8, $2> = [reg, val]
		var p: CPointer<Unit> = unsafe { getPtr(inout a) }
		let ret = unsafe { write(fd, p, 2) }
		if (ret != 2) { throw I2cWriteRegFailed(path)  }
	}

	public func readReg(reg: UInt8): UInt8 {
		var value: UInt8 = 0
		var p = unsafe { getPtr(inout value) }
		let ret = unsafe { read(fd, p, 1) }
		if (ret != 1) { throw I2cReadRegFailed(path) }
		return value
	}
}


/* -------- 打开一个I2C设备 在gc回收时关闭 --------
 *     fixme: 对I2C的操作都要加锁
 *     TODO: 直接使用libi2c.so  */
//public class I2C {

	/* -------- 主初始化器 - 打开I2C设备 -------- */
//	public init(path: String) {
//		this.path = path
//		unsafe{
//			var cstr = LibC.mallocCString(path)  /* 申请资源 */
//			fd = I2C_init(cstr)                  /* 打开设备 */
//			LibC.free(cstr)                      /* 释放资源 */
//			if (fd == I2C_FILE_NOT_OPENED) {     /* 打开设备失败 */
//				throw I2cOpenedFailed(path)  }
//		}
//	}

	/* -------- 析构函数 -------- */
//	~init() {
//		unsafe {
//			if (fd == -1) {
//				I2C_free(fd)
//				fd == -1
//			}
//		}
//	}

	/* -------- 关闭I2C设备 -------- */
//	public func close() {
//		unsafe {
//			if (fd == -1) {
//				I2C_free(fd)
//				fd == -1
//			}
//		}
//	}

	/* -------- 设置I2C设备地址 -------- */
//	public func setAddr(addr: UInt8): Unit {
//		unsafe {
//			if (I2C_setAddr(fd, addr)) {  throw I2cSetAddressFailed(path)  }
//		}
//	}

	/* -------- 写入从机寄存器 -------- */
//	public func writeReg(reg: UInt8, val:UInt8): Unit {
//		unsafe {
//			if (I2C_writeReg(fd, reg, val)) {  throw I2cWriteRegFailed(path)  }
//		}
//	}

	/* -------- 读取I2C从机寄存器 --------
	 * -- 失败返回0 -- */
//	public func readReg(reg: UInt8): UInt8 {
//		unsafe {
//			return I2C_readReg(fd, reg)  }
//	}

	/* -------- 读取I2C从机寄存器 - 带符号 --------
	 * -- 失败返回0 -- */
//	public func readRegFlag(reg: UInt8): Int8 {
//		unsafe {
//			return I2C_readRegFlag(fd, reg)  }
//	}

	/* -------- 存放文件描述符 -------- */
//	private var fd: Int16

	/* -------- 存放设备文件地址 -------- */
//	private var path: String

//}

/* -------- I2C设备异常 -------- */
open private class I2cExcept <: Exception {
	public I2cExcept(dev: String, msg: String) {
		super('${dev}: ${msg}')  }
}

/* -------- 打开I2C设备失败 -------- */
private class I2cOpenedFailed <: I2cExcept {
	public I2cOpenedFailed(dev: String) {
		super(dev, "failed to open i2c device")  }
}

/* -------- 关闭I2C设备失败 -------- */
private class I2cClosedFailed <: I2cExcept {
	public I2cClosedFailed(dev: String) {
		super(dev, "failed to close i2c device")  }
}

/* -------- I2C设置地址失败 -------- */
private class I2cSetAddressFailed <: I2cExcept {
	public I2cSetAddressFailed(dev: String) {
		super(dev, "failed to open i2c device")  }
}

/* -------- I2C写从机I2C失败 -------- */
private class I2cWriteRegFailed <: I2cExcept {
	public I2cWriteRegFailed(dev: String) {
		super(dev, "failed to write i2c reg")  }
}

/* -------- I2C读从机I2C失败 -------- */
private class I2cReadRegFailed <: I2cExcept {
	public I2cReadRegFailed(dev: String) {
		super(dev, "failed to read i2c reg")  }
}

/* -------- 未知I2C异常 -------- */
private class I2cUnknowExp <: I2cExcept {
	public I2cUnknowExp(dev: String, msg: String) {
		super(dev, msg)  }
}
