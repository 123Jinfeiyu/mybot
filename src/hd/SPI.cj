package bot4cj.hd

import ctype4cj.*

/* -------- 成功返回非负整数，失败返回-1 -------- */
//foreign func ioctl(fd: C_Int, addr: C_ULong, ...): C_Int				

public const SPI_SUCCESS= 0
public const SPI_FAILED = 1

public const SPI_MODE_0 = 0i8
public const SPI_IOC_WR_MODE = 0x40016B01u64
public const SPI_IOC_RD_MODE = 0x80016B01u64
public const SPI_IOC_WR_BITS_PER_WORD = 0x40016B03u64
public const SPI_IOC_RD_BITS_PER_WORD = 0x80016B03u64
public const SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04u64
public const SPI_IOC_RD_MAX_SPEED_HZ = 0x80046B04u64

/* ---- 打开SPI设备，
 * ---- 参数形如：/dev/spidev0.0
 * ---- 意思是 bus 0, chip select 0
 * ---- 直接返回文件句柄fd，打开成功的话该值大于（大于等于？）0
 * ---- */
@C
public func open_spi(device: CString): C_Int {
	let spi_fd  = unsafe { libc_open(device, O_RDWR) }
	return spi_fd
}


/* ---- 关闭一个SPI设备 ---- */
@C
public func close_spi(fd: C_Int): Unit {
	unsafe { libc_close(fd) }
}


/* ---- 配置SPI设备
 * ---- 从机时可以设置
 * ----
 * ---- mode => SPI_MODE_0
 * ---- bits => 8
 * ---- speed  => 1000000
 * ---- delay  => 0
 * ----
 * ---- */
@C
public func setup_spi(fd: C_Int, mode: UInt8, bits: UInt8, speed: UInt32, delay: UInt16): Int64 {

	var m = mode
	var b = bits
	var s = speed
	var d = delay

	/* ---- 设置SPI模式 ---- */
	if (unsafe{ ioctl(fd, SPI_IOC_WR_MODE, inout m) } == -1) { return SPI_FAILED }
	if (unsafe{ ioctl(fd, SPI_IOC_RD_MODE, inout m) } == -1) { return SPI_FAILED }

	/* ---- 设置字长 ---- */
	if (unsafe{ ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, inout b) } == -1) { return SPI_FAILED }
	if (unsafe{ ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, inout b) } == -1) { return SPI_FAILED }

	/* ---- 设置时钟频率 ---- */
	if (unsafe{ ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, inout s) } == -1) { return SPI_FAILED }
	if (unsafe{ ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, inout s) } == -1) { return SPI_FAILED }


	return SPI_SUCCESS

}


/* ---- 发送一个字节数据 ---- */
@C
public func write_spi(fd: C_Int, data: UInt8): Int64
{
	var buf = data
	var ptr = CPointer<Unit>(inout buf)
	let res = unsafe { libc_write(fd, ptr, 1) }
	if (res == 1) { return SPI_SUCCESS }
	else    { return SPI_FAILED }
}


/* ---- 接收数据，接收1个字节数据 ----
 * ---- NOTE(20260616-qing): 注意失败后返回256（大于u8的范围） */
@C
public func read_spi(fd: C_Int): Int64
{
	var buf = 0u16
	var ptr = CPointer<Unit>(inout buf)
	let res =  unsafe { libc_read(fd, ptr, 1) }
	if (res != 1) { return 256  }
	else   { return Int64(buf) }
}

