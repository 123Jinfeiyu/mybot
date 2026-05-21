package bot4cj.hd
import bot4cj.th.*

//foreign func I2C_init(path: CString): Int16
//foreign func I2C_free(fd: Int16): Unit
//foreign func I2C_setAddr(fd: Int16, addr: UInt8): Bool
//foreign func I2C_writeReg(fd: Int16, reg: UInt8, val: UInt8): Bool
//foreign func I2C_readReg(fd: Int16, reg: UInt8): UInt8
//foreign func I2C_readRegFlag(fd: Int16, reg: UInt8): Int8
//const I2C_FILE_NOT_OPENED: Int16 = -1


/* -------- 我们将直接调用glibc中的函数 --------
 * TODO(20260518-qing):  稍后我们专门做一个i2c代码仓库，这样就能够区分不同架构上面的int的大小
 * TODO*(20260521-qing): C语言中的int类型一般是Int32，我们可以声明一个类型别名  */
foreign func open(path: CString, flag: Int32, ...): Int32
foreign func close(fd: Int32): Int32	/* 成功返回0，失败返回-1 */
foreign func ioctl(fd: Int32, addr: UInt64, ...): Int32	/* 成功返回非负整数，失败返回-1 */
foreign func read(fd: Int32, buf: CPointer<Unit>, count: UIntNative): IntNative
foreign func write(fd: Int32, buf: CPointer<Unit>, count: UIntNative): IntNative


const I2C_FAILED = -1i32
const I2C_SUCCESS = 0i32
const O_RDWR = 2i32	/* C头文件中的宏 */
const I2C_SLAVE = 0x0703u64	/* at least on arm64 */
/* -------- OPEN I2C DEVICE -------- */
@C
func openI2cDev(path: CString): Int32 {
	unsafe {
		let fd =  open(path, O_RDWR)
		if (fd > 0) { 
			I2cLock_create(fd)
			return  fd
		}
		else {
			throw I2cOpenedFailed("?") }
	}
}

/* -------- CLOSE I2C DEVICE -------- */
@C
func closeI2cDev(fd: Int32): Unit {
	unsafe {
		I2cLock_destroy(fd)
		let ret = close(fd)
		if (ret == I2C_FAILED) {
			throw I2cClosedFailed("?") }
		else if (ret == I2C_SUCCESS) { ;  }
		else {
			throw I2cUnknowExp(
				"?", "$function returned: {ret.toString()}")
		}
	}
}

/* -------- SET I2C ADDR -------- */
@C
func setI2cAddr(fd: Int32, addr: UInt8): Unit {
	unsafe {
		let ret = ioctl(fd, I2C_SLAVE, addr)
		if (ret < 0) {
			throw I2cSetAddressFailed("?") }
	}
}

/* -------- WRITE I2C REG -------- */
@C
func writeI2cReg(fd: Int32, reg: UInt8, val:UInt8): Unit {
	var a: VArray<UInt8, $2> = [reg, val]
	var p: CPointer<Unit> = unsafe { CPointer<Unit>(inout a) }
	if (2 !=  unsafe { write(fd, p, 2) }) {
		throw I2cWriteRegFailed("?")
	}
}

/* -------- READ I2C REG -------- */
@C
func readI2cReg(fd: Int32, reg: UInt8): UInt8 {
	var value: UInt8 = 0
	var p = unsafe { CPointer<Unit>(inout value) }
	let ret = unsafe { read(fd, p, 1) }
	if (ret != 1) { throw I2cReadRegFailed("?") }
	return value
}


/* -------- I2C锁具 --------
 * INFO(20260521-qing): 因为每个I2C接口在通信时具有原子化的需求
 * - 需要一个互斥锁集合，在打开第一个I2C设备时被初始化，
 * - 释放最后一个I2C设备时，将锁具释放
 * - 每次进行I2C通信时，进行上锁操作   */
@C
struct I2C_Lock {
	var i2c_fd: Int32 = 0
	var lock = CPointer<UInt8>()
	var is_active = false
}

var MAIN_LOCK = CPointer<UInt8>()
var LOCKS = CPointer<I2C_Lock>()

/* -------- 为一个I2C设备注册锁具 -------- */
@C
func I2cLock_create(fd: Int32): Unit {
	unsafe {
		/* -------- 初始化主要的锁 -------- */
		if (MAIN_LOCK.isNull()) {
			MAIN_LOCK = _mutex_create()
			LOCKS = LibC.malloc<I2C_Lock>(count: 20)
		}

		/* -------- 获取主锁 --------*/
		_mutex_lock(MAIN_LOCK)

		for (idx in 0..20) {
			var lock = (LOCKS+idx).read()
			if (lock.is_active == false) {
				lock.is_active = true
				lock.i2c_fd = fd
				lock.lock = _mutex_create()
				(LOCKS+idx).write(lock)
				_mutex_unlock(MAIN_LOCK)
				return
			}
		}

		_mutex_unlock(MAIN_LOCK)
		throw OverflowException("LOCKS is over.")
	}
}
/* -------- 为一个I2C设备注销锁具 -------- */
@C
func I2cLock_destroy(fd: Int32): Unit {
	unsafe {
		if (MAIN_LOCK.isNull()) {
			throw NoneValueException("no i2c lock")
		}
		_mutex_lock(MAIN_LOCK)

		var flag_any = false
		var flag_target = false
		for (idx in 0..20) {
			var lock = (LOCKS+idx).read()
			if (lock.is_active == true && lock.i2c_fd == fd) {
				lock.is_active = false
				_mutex_free(lock.lock)
				(LOCKS+idx).write(lock)
				flag_target = true
			}
			else if (lock.is_active == true && lock.i2c_fd != fd) {
				flag_any = true
			}
		}

		if (!flag_any) {
			LibC.free(LOCKS)
			_mutex_free(MAIN_LOCK)
			MAIN_LOCK = CPointer<UInt8>()
		}

		if (flag_target) {
			return
		}

		_mutex_unlock(MAIN_LOCK)
		throw NoneValueException("i2c lock nor found")
	}
}

/* -------- 为一个I2C锁具加锁 -------- */
@C
func I2cLock_lock(fd: Int32): Unit {

	unsafe {
		if (MAIN_LOCK.isNull()) {
			throw NoneValueException("no i2c lock")
		}
		_mutex_lock(MAIN_LOCK)

		for (idx in 0..20) {
			var lock = (LOCKS+idx).read()
			if (lock.is_active == true && lock.i2c_fd == fd) {
				lock.is_active = false
				_mutex_lock(lock.lock)
				(LOCKS+idx).write(lock)
				break
			}
		}

		_mutex_unlock(MAIN_LOCK)
		throw NoneValueException("i2c lock nor found")
	}
}

/* -------- 为一个I2C锁具解锁 -------- */
@C
func I2cLock_unlock(fd: Int32): Unit {

	unsafe {
		if (MAIN_LOCK.isNull()) {
			throw NoneValueException("no i2c lock")
		}
		_mutex_lock(MAIN_LOCK)

		for (idx in 0..20) {
			var lock = (LOCKS+idx).read()
			if (lock.is_active == true && lock.i2c_fd == fd) {
				lock.is_active = false
				_mutex_unlock(lock.lock)
				(LOCKS+idx).write(lock)
				break
			}
		}

		_mutex_unlock(MAIN_LOCK)
		throw NoneValueException("i2c lock nor found")
	}
}


/* -------- 原生I2C通信类--------
 * INFO(20260521-qing): 直接调用glibc的接口 */
public class I2C_Orig {
	private var fd: Int32 = -1
	private var path: String
	public init (path: String) {
		this.path = path
		let s1 = unsafe { 
			LibC.mallocCString(path) }
		fd = unsafe { openI2cDev(s1) }
	}

	public func _close(): Unit {
		unsafe { closeI2cDev(fd) }
	}

	public func setAddr(addr: UInt8) {
		unsafe {  setI2cAddr(fd, addr)  }
	}

	public func writeReg(reg: UInt8, val:UInt8): Unit {
		unsafe { writeI2cReg(fd, reg, val) }
	}

	public func readReg(reg: UInt8): UInt8 {
		var value = unsafe { readI2cReg(fd, reg) }
		return value
	}

	/*--------获取文件句柄--------*/
	public func getFd(): Int32 {
		return fd
	}

}


/* -------- 打开一个I2C设备 在gc回收时关闭 --------
 *     fixme: 对I2C的操作都要加锁
 *     TODO: 直接使用libi2c.so  */
//public class I2C {

//	/* -------- 主初始化器 - 打开I2C设备 -------- */
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
//
//	/* -------- 析构函数 -------- */
//	~init() {
//		unsafe {
//			if (fd == -1) {
//				I2C_free(fd)
//				fd == -1
//			}
//		}
//	}
//
//	/* -------- 关闭I2C设备 -------- */
//	public func close() {
//		unsafe {
//			if (fd == -1) {
//				I2C_free(fd)
//				fd == -1
//			}
//		}
//	}
//
//	/* -------- 设置I2C设备地址 -------- */
//	public func setAddr(addr: UInt8): Unit {
//		unsafe {
//			if (I2C_setAddr(fd, addr)) {  throw I2cSetAddressFailed(path)  }
//		}
//	}
//
//	/* -------- 写入从机寄存器 -------- */
//	public func writeReg(reg: UInt8, val:UInt8): Unit {
//		unsafe {
//			if (I2C_writeReg(fd, reg, val)) {  throw I2cWriteRegFailed(path)  }
//		}
//	}
//
//	/* -------- 读取I2C从机寄存器 --------
//	 * -- 失败返回0 -- */
//	public func readReg(reg: UInt8): UInt8 {
//		unsafe {
//			return I2C_readReg(fd, reg)  }
//	}
//
//	/* -------- 读取I2C从机寄存器 - 带符号 --------
//	 * -- 失败返回0 -- */
//	public func readRegFlag(reg: UInt8): Int8 {
//		unsafe {
//			return I2C_readRegFlag(fd, reg)  }
//	}
//
//	/* -------- 存放文件描述符 -------- */
//	private var fd: Int16
//
//	/* -------- 存放设备文件地址 -------- */
//	private var path: String
//
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
