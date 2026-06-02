package bot4cj.hd
import ctype4cj.*

const FBIOGET_VSCREENINFO = 0x4602u64
const FBIOGET_FSCREENINFO = 0x4600u64
const PROT_WRITE: C_Int = 0x2
const MAP_SHARED: C_Int = 0x01

/* --- 定义在I2C.cj里了 ---*/
//foreign func ioctl(fd: C_Int, request: C_ULong, ...): C_Int
/* ---- 内存映射 ---- */
public type off_t = C_LongLong
foreign func mmap(addr: CPointer<Unit>, length: UIntNative, prot: C_Int, flags: C_Int, fd: C_Int, offset: off_t): CPointer<Unit>

/* -------- 打开framebuffer帧缓冲设备 -------- */
@C
public func fb_open(fb_dev: CString): C_Int {
	return unsafe { open(fb_dev, O_RDWR) }
}

/* -------- 帧缓冲区固定参数 -------- */
@C
public struct fb_fix_screeninfo {
        var id: VArray<C_Char, $16>         /* 设备驱动名称字符串，比如"TT Builtin" */
        var smem_start: C_ULong = 0         /* 帧缓冲内存（显存）的物理起始地址 */
                                            /* (physical address) */
        var smem_len: UInt32 = 0            /* 帧缓冲内存的总长度，单位字节 */
        var type_: UInt32 = 0               /* 显存类型（fb存储布局）see FB_TYPE_* */
        var type_aux: UInt32 = 0            /* 显存附加类型 */
        var visual: UInt32 = 0              /* 色彩显示模式 see FB_VISUAL_* */
        var xpanstep: UInt16  = 0           /* zero if no hardware panning  */
        var ypanstep: UInt16  = 0           /* zero if no hardware panning  */
        var ywrapstep: UInt16  = 0          /* zero if no hardware ywrap    */
        var line_length: UInt32  = 0        /* 显示屏上一行像素数据所占用的字节数 */
        var mmio_start: C_ULong = 0         /* 内存映射I/O的物理起始地址 */
                                            /* (physical address) */
        var mmio_len: UInt32 = 0            /* 内存映射的区域长度，单位字节  */
        var accel: UInt32 = 0               /* 硬件加速类型  */
                                            /*  specific chip/card we have  */
        var capabilities: UInt16 = 0        /* see FB_CAP_*                 */
        var reserved: VArray<UInt16, $2>    /* 保留字段，用于未来扩展，确保兼容 */

	public init() {
		id = VArray<C_Char, $16> (repeat: 0)
		reserved = VArray<UInt16, $2> (repeat: 0)
	}
}

/* 颜色字段偏移量的解释：所有偏移量均从右侧开始计算，
 * 位于一个“像素”值内部，该像素值的宽度正好等于 'bits_per_pixel' 
 * （也就是说：你可以将偏移量作为 << 的右操作数使用）。
 * 之后的像素就是一个比特流，会原封不动地写入显存。
 *
 * 对于伪彩色：所有颜色分量的偏移量和长度应该相同。
 * 偏移量指定了调色板索引的最低有效位在像素值中的位置。
 * 长度表示可用的调色板条目数量（即条目数 = 1 << 长度）。
 */
@C
public struct fb_bitfield {
        var offset: UInt32 = 0              /* 位域的起始位置（从右侧算起）*/
        var length: UInt32 = 0              /* 位域的长度（比特数）       */
        var msb_right: UInt32 = 0           /* != 0 : 最高有效位在右侧     */
                                        /*        （即低位在右）       */
}

/* -------- 帧缓冲区可变参数 -------- */
@C
public struct fb_var_screeninfo {
        var xres: UInt32 = 0            /* visible resolution           */
        var yres: UInt32 = 0
        var xres_virtual: UInt32 = 0    /* virtual resolution           */
        var yres_virtual: UInt32 = 0
        var xoffset: UInt32 = 0         /* offset from virtual to visible */
        var yoffset: UInt32 = 0         /* resolution                   */

        var bits_per_pixel: UInt32 = 0  /* guess what                   */
        var grayscale: UInt32 = 0       /* 0 = color, 1 = grayscale,    */
                                        /* >1 = FOURCC                  */
        var red: fb_bitfield            /* bitfield in fb mem if true color, */
        var green: fb_bitfield          /* else only length is significant */
        var blue: fb_bitfield
        var transp: fb_bitfield         /* transparency                 */

        var nonstd: UInt32 = 0          /* != 0 Non standard pixel format */

        var activate: UInt32  = 0       /* see FB_ACTIVATE_*            */

        var height: UInt32 = 0          /* height of picture in mm    */
        var width: UInt32 = 0           /* width of picture in mm     */

        var accel_flags: UInt32 = 0     /* (OBSOLETE) see fb_info.flags */

        /* Timing: All values in pixclocks, except pixclock (of course) */
        var pixclock: UInt32 = 0        /* pixel clock in ps (pico seconds) */
        var left_margin: UInt32 = 0     /* time from sync to picture    */
        var right_margin: UInt32 = 0    /* time from picture to sync    */
        var upper_margin: UInt32 = 0    /* time from sync to picture    */
        var lower_margin: UInt32 = 0
        var hsync_len: UInt32 = 0       /* length of horizontal sync    */
        var vsync_len: UInt32 = 0       /* length of vertical sync      */
        var sync: UInt32  =0            /* see FB_SYNC_*                */
        var vmode: UInt32  = 0          /* see FB_VMODE_*               */
        var rotate: UInt32 = 0          /* angle we rotate counter clockwise */
        var colorspace: UInt32 = 0      /* colorspace for FOURCC-based modes */
        var reserved: VArray<UInt32, $4>/* Reserved for future compatibility */

	public init() {
		red = fb_bitfield()
		green = fb_bitfield()
		blue = fb_bitfield()
		transp = fb_bitfield()
		reserved = VArray<UInt32, $4> (repeat: 0)
	}
}

/* ---- 获取帧缓冲区可变参数 ---- */
@C
public func fb_get_var_info(fd: C_Int) {
	var info = fb_var_screeninfo()
	unsafe { ioctl(fd, FBIOGET_VSCREENINFO, inout info) }
	return info
}

/* ---- 获取帧缓冲区固定参数 ---- */
@C
public func fb_get_fix_info(fd: C_Int) {
	var info = fb_fix_screeninfo()
	unsafe { ioctl(fd, FBIOGET_FSCREENINFO, inout info) }
	return info
}

/* ---- 内存映射 ---- */
@C
public func fb_mmap(addr: CPointer<Unit>, length: UIntNative, fd: C_Int, offset: off_t): CPointer<Unit>
{
	let buf = unsafe { mmap(addr, length, PROT_WRITE, MAP_SHARED, fd, offset) }
	return buf
}
