package bot4cj.hd
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func I2C_setAddr(fd: Int16, addr: UInt8): Bool
foreign func I2C_writeReg(fd: Int16, reg: UInt8, val: UInt8): Bool
foreign func I2C_readReg(fd: Int16, reg: UInt8): UInt8
foreign func I2C_readRegFlag(fd: Int16, reg: UInt8): Int8
const I2C_FILE_NOT_OPENED: Int16 = -1

/* -------- 打开一个I2C设备 在gc回收时关闭 --------
 *     fixme: 对I2C的操作都要加锁
 *     TODO: 直接使用libi2c.so  */
public class I2C {

	/* -------- 主初始化器 - 打开I2C设备 -------- */
	public init(path: String) {
		this.path = path
		unsafe{
			var cstr = LibC.mallocCString(path)  /* 申请资源 */
			fd = I2C_init(cstr)                  /* 打开设备 */
			LibC.free(cstr)                      /* 释放资源 */
			if (fd == I2C_FILE_NOT_OPENED) {     /* 打开设备失败 */
				throw I2cOpenedFailed(path)  }
		}
	}

	/* -------- 析构函数 -------- */
	~init() {
		unsafe {
			if (fd == -1) {
				I2C_free(fd)
				fd == -1
			}
		}
	}

	/* -------- 关闭I2C设备 -------- */
	public func close() {
		unsafe {
			if (fd == -1) {
				I2C_free(fd)
				fd == -1
			}
		}
	}

	/* -------- 设置I2C设备地址 -------- */
	public func setAddr(addr: UInt8): Unit {
		unsafe {
			if (I2C_setAddr(fd, addr)) {  throw I2cSetAddressFailed(path)  }
		}
	}

	/* -------- 写入从机寄存器 -------- */
	public func writeReg(reg: UInt8, val:UInt8): Unit {
		unsafe {
			if (I2C_writeReg(fd, reg, val)) {  throw I2cWriteRegFailed(path)  }
		}
	}

	/* -------- 读取I2C从机寄存器 --------
	 * -- 失败返回0 -- */
	public func readReg(reg: UInt8): UInt8 {
		unsafe {
			return I2C_readReg(fd, reg)  }
	}

	/* -------- 读取I2C从机寄存器 - 带符号 --------
	 * -- 失败返回0 -- */
	public func readRegFlag(reg: UInt8): Int8 {
		unsafe {
			return I2C_readRegFlag(fd, reg)  }
	}

	/* -------- 存放文件描述符 -------- */
	private var fd: Int16

	/* -------- 存放设备文件地址 -------- */
	private var path: String

}

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
