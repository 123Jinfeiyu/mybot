-- Camera.lua —— 摄像头驱动（V4L2 + LuaJIT ffi，MJPEG 直取）
--
-- 【为什么在 Lua 里做】摄像头是硬件，硬件的说明书在本项目就是 modules/*.lua 驱动。
-- 因此宿主不必链接任何采集/编码库（robot_agent 的 C++ 版为此需要 libjpeg）。
-- 抓帧走 V4L2 mmap + MJPEG：摄像头硬件已把画面编码成 JPEG，抓帧即用，不需要编码器。
--
-- 【只支持 MJPEG】仅提供 YUYV 的设备会返回清晰 ERR。YUYV 要么自带 JPEG 编码器、
-- 要么引入 libjpeg，两条路都比「直接取 MJPEG」贵得多，而 USB 摄像头普遍支持 MJPEG。
-- 树莓派 CSI 摄像头走 libcamera，不经 V4L2 这条路径，也不在本驱动范围内。
--
-- 【仿真】data.src 指向一个图片文件时直接读它并 base64 返回——无摄像头也能把
-- 「拍照 → 进上下文 → 多模态模型看图」全链路跑通，与 robot_agent 的模拟模式同语义。
--
-- 【ABI 自检】ffi 声明一旦与内核头不符（尺寸小了），ioctl 就会越界读写内存。
-- 加载期 assert 尺寸、构建期 test/test_v4l2_consts.c 再与 <linux/videodev2.h> 比对
-- （F-24 同族做法：常量漂移在构建期即失败）。
local mpath = dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua")
local HAL = dofile(mpath.resolve("lib/HAL.lua"))	-- 复用底座：open/close/ioctl 已在其中声明
local ffi = require("ffi")
local bit = require("bit")

ffi.cdef[[
/* V4L2 结构（与内核 ABI 对齐；尺寸见文件末尾 assert）。
 * 前缀 cam_ 避免与 HAL.lua 已声明的 spi_ioc_transfer / cj_i2c_msg 冲突。 */
typedef struct { long tv_sec; long tv_usec; } cam_timeval;

typedef struct {
	unsigned int  type;
	unsigned int  flags;
	unsigned char frames;
	unsigned char seconds;
	unsigned char minutes;
	unsigned char hours;
	unsigned char userbits[4];
} cam_timecode;

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned int pixelformat;
	unsigned int field;
	unsigned int bytesperline;
	unsigned int sizeimage;
	unsigned int colorspace;
	unsigned int priv;
	unsigned int flags;
	unsigned int ycbcr_enc;
	unsigned int quantization;
	unsigned int xfer_func;
} cam_pix_format;

/* 内核的 union 里有指针成员 → 对齐 8、整体 208 字节。
 * align_ptr 就是为对齐而留：少了它 LuaJIT 会排出 204，与 _IOWR 里编码的长度不符。 */
typedef struct {
	unsigned int type;
	union {
		cam_pix_format pix;
		void          *align_ptr;
		unsigned char  raw[200];
	} fmt;
} cam_format;

typedef struct {
	unsigned int  count;
	unsigned int  type;
	unsigned int  memory;
	unsigned int  capabilities;
	unsigned char flags;
	unsigned char reserved[3];
} cam_requestbuffers;

typedef struct {
	unsigned int  index;
	unsigned int  type;
	unsigned int  bytesused;
	unsigned int  flags;
	unsigned int  field;
	cam_timeval   timestamp;
	cam_timecode  timecode;
	unsigned int  sequence;
	unsigned int  memory;
	union {
		unsigned int  offset;
		unsigned long userptr;
		void         *planes;
		int           fd;
	} m;
	unsigned int  length;
	unsigned int  reserved2;
	union { int request_fd; unsigned int reserved; } reserved_u;
} cam_buffer;

typedef struct {
	unsigned char driver[16];
	unsigned char card[32];
	unsigned char bus_info[32];
	unsigned int  version;
	unsigned int  capabilities;
	unsigned int  device_caps;
	unsigned int  reserved[3];
} cam_capability;

typedef struct { int fd; short events; short revents; } cam_pollfd;

/* HAL 没有声明的 libc 调用 */
int poll(cam_pollfd *fds, unsigned long nfds, int timeout);
void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
int munmap(void *addr, unsigned long length);
void *fopen(const char *path, const char *mode);
int fclose(void *stream);
int fseek(void *stream, long offset, int whence);
long ftell(void *stream);
unsigned long fread(void *ptr, unsigned long size, unsigned long n, void *stream);
]]

-- ABI 自检：与内核头不符即立刻失败，绝不放行到 ioctl（那时是越界读写，不是报错）
local ABI = {
	{ "cam_buffer", 88 }, { "cam_format", 208 }, { "cam_pix_format", 48 },
	{ "cam_capability", 104 }, { "cam_requestbuffers", 20 }, { "cam_timecode", 16 },
}
for _, spec in ipairs(ABI) do
	local got = ffi.sizeof(spec[1])
	assert(got == spec[2], string.format(
		"V4L2 ABI 不符：sizeof(struct %s)=%d，内核头为 %d（换内核/架构后需同步 ffi 声明）",
		spec[1], got, spec[2]))
end

------------------------------------------------------------------
-- 常量（构建期由 test/test_v4l2_consts.c 与内核头比对）
------------------------------------------------------------------
local VIDIOC_QUERYCAP   = 0x80685600
local VIDIOC_S_FMT      = 0xc0d05605
local VIDIOC_REQBUFS    = 0xc0145608
local VIDIOC_QUERYBUF   = 0xc0585609
local VIDIOC_QBUF       = 0xc058560f
local VIDIOC_DQBUF      = 0xc0585611
local VIDIOC_STREAMON   = 0x40045612
local VIDIOC_STREAMOFF  = 0x40045613

local O_RDWR                  = 2
local PROT_READ, PROT_WRITE   = 1, 2
local MAP_SHARED              = 1
local POLLIN                  = 0x001
local BUF_TYPE_VIDEO_CAPTURE  = 1
local MEMORY_MMAP             = 1
local FIELD_ANY               = 0
local CAP_VIDEO_CAPTURE       = 0x00000001
local PIX_FMT_MJPEG           = 0x47504a4d	-- fourcc('M','J','P','G')

local DEFAULT_DEVICE = "/dev/video0"
local DEFAULT_WIDTH, DEFAULT_HEIGHT = 640, 480
local BUFFER_COUNT = 4
local MAX_SIM_BYTES = 16 * 1024 * 1024
local POLL_TIMEOUT_MS = 3000

------------------------------------------------------------------
-- base64（返回给宿主的是文本，避免二进制穿过 Json/Lua 边界）
------------------------------------------------------------------
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function base64(data)
	local out, n = {}, 0
	local len = #data
	local i = 1
	while i + 2 <= len do
		local a, b, c = data:byte(i, i + 2)
		local v = a * 65536 + b * 256 + c
		out[n + 1] = B64:sub(math.floor(v / 262144) % 64 + 1, math.floor(v / 262144) % 64 + 1)
		out[n + 2] = B64:sub(math.floor(v / 4096) % 64 + 1, math.floor(v / 4096) % 64 + 1)
		out[n + 3] = B64:sub(math.floor(v / 64) % 64 + 1, math.floor(v / 64) % 64 + 1)
		out[n + 4] = B64:sub(v % 64 + 1, v % 64 + 1)
		n = n + 4
		i = i + 3
	end
	local rest = len - i + 1
	if rest == 1 then
		local a = data:byte(i)
		local v = a * 65536
		out[n + 1] = B64:sub(math.floor(v / 262144) % 64 + 1, math.floor(v / 262144) % 64 + 1)
		out[n + 2] = B64:sub(math.floor(v / 4096) % 64 + 1, math.floor(v / 4096) % 64 + 1)
		out[n + 3] = "="
		out[n + 4] = "="
	elseif rest == 2 then
		local a, b = data:byte(i, i + 1)
		local v = a * 65536 + b * 256
		out[n + 1] = B64:sub(math.floor(v / 262144) % 64 + 1, math.floor(v / 262144) % 64 + 1)
		out[n + 2] = B64:sub(math.floor(v / 4096) % 64 + 1, math.floor(v / 4096) % 64 + 1)
		out[n + 3] = B64:sub(math.floor(v / 64) % 64 + 1, math.floor(v / 64) % 64 + 1)
		out[n + 4] = "="
	end
	return table.concat(out)
end

------------------------------------------------------------------
-- 仿真：读文件（沙箱禁了 io.open，故走 ffi）
------------------------------------------------------------------
local function read_file(path)
	local f = ffi.C.fopen(path, "rb")
	if f == nil then return nil, "无法打开模拟图片 " .. path end
	local size = ffi.C.fseek(f, 0, 2)	-- SEEK_END
	if size ~= 0 then
		ffi.C.fclose(f)
		return nil, "模拟图片不可定位 " .. path
	end
	local len = tonumber(ffi.C.ftell(f))
	ffi.C.fseek(f, 0, 0)			-- SEEK_SET
	if len == nil or len <= 0 then
		ffi.C.fclose(f)
		return nil, "模拟图片为空 " .. path
	end
	if len > MAX_SIM_BYTES then
		ffi.C.fclose(f)
		return nil, string.format("模拟图片超过 %d 字节上限", MAX_SIM_BYTES)
	end
	local buf = ffi.new("unsigned char[?]", len)
	local got = tonumber(ffi.C.fread(buf, 1, len, f))
	ffi.C.fclose(f)
	if got ~= len then return nil, "模拟图片读取不完整 " .. path end
	return ffi.string(buf, len)
end

------------------------------------------------------------------
-- 真机：V4L2 打开 + MJPEG 协商 + mmap 缓冲 + 起流
------------------------------------------------------------------
local state = { fd = -1, buffers = {}, streaming = false, warmed = false, sim = nil }

local function device_close()
	if state.streaming then
		local t = ffi.new("unsigned int[1]", BUF_TYPE_VIDEO_CAPTURE)
		HAL.sys.ioctl_p(state.fd, VIDIOC_STREAMOFF, t)
		state.streaming = false
	end
	for _, b in ipairs(state.buffers) do
		if b.ptr ~= nil then ffi.C.munmap(b.ptr, b.len) end
	end
	state.buffers = {}
	if state.fd >= 0 then
		HAL.sys.close(state.fd)
		state.fd = -1
	end
end

local function device_open(path, data)
	local fd = HAL.sys.open(path, O_RDWR)
	if fd < 0 then
		return nil, string.format("无法打开摄像头 %s（errno=%d）：设备不存在、被占用或权限不足",
			path, HAL.sys.errno())
	end
	state.fd = fd

	local cap = ffi.new("cam_capability")
	if HAL.sys.ioctl_p(fd, VIDIOC_QUERYCAP, cap) ~= 0 then
		return nil, string.format("%s 不是 V4L2 设备（VIDIOC_QUERYCAP 失败）", path)
	end
	local caps = cap.capabilities
	if bit.band(caps, CAP_VIDEO_CAPTURE) == 0 then
		return nil, string.format("%s 不支持视频采集（capabilities=0x%x）", path, caps)
	end

	local width = tonumber(data.width) or DEFAULT_WIDTH
	local height = tonumber(data.height) or DEFAULT_HEIGHT
	local fmt = ffi.new("cam_format")
	fmt.type = BUF_TYPE_VIDEO_CAPTURE
	fmt.fmt.pix.width = width
	fmt.fmt.pix.height = height
	fmt.fmt.pix.pixelformat = PIX_FMT_MJPEG
	fmt.fmt.pix.field = FIELD_ANY
	if HAL.sys.ioctl_p(fd, VIDIOC_S_FMT, fmt) ~= 0 then
		return nil, string.format("%s 设置格式失败（%dx%d MJPEG）", path, width, height)
	end
	if fmt.fmt.pix.pixelformat ~= PIX_FMT_MJPEG then
		return nil, string.format(
			"%s 不支持 MJPEG（实际 fourcc=0x%x）。本驱动只取硬件编码的 MJPEG："
			.. "请换支持 MJPEG 的设备，或用 src 参数走模拟图片",
			path, fmt.fmt.pix.pixelformat)
	end
	state.width = fmt.fmt.pix.width
	state.height = fmt.fmt.pix.height

	local req = ffi.new("cam_requestbuffers")
	req.count = BUFFER_COUNT
	req.type = BUF_TYPE_VIDEO_CAPTURE
	req.memory = MEMORY_MMAP
	if HAL.sys.ioctl_p(fd, VIDIOC_REQBUFS, req) ~= 0 or req.count < 2 then
		return nil, string.format("%s 申请 mmap 缓冲失败（VIDIOC_REQBUFS）", path)
	end

	for i = 0, req.count - 1 do
		local buf = ffi.new("cam_buffer")
		buf.type = BUF_TYPE_VIDEO_CAPTURE
		buf.memory = MEMORY_MMAP
		buf.index = i
		if HAL.sys.ioctl_p(fd, VIDIOC_QUERYBUF, buf) ~= 0 then
			return nil, string.format("%s 查询缓冲 %d 失败（VIDIOC_QUERYBUF）", path, i)
		end
		local ptr = ffi.C.mmap(nil, buf.length, bit.bor(PROT_READ, PROT_WRITE), MAP_SHARED, fd, 0)
		if ptr == ffi.cast("void *", -1) then
			return nil, string.format("%s mmap 缓冲 %d 失败", path, i)
		end
		state.buffers[i + 1] = { ptr = ptr, len = buf.length }
		if HAL.sys.ioctl_p(fd, VIDIOC_QBUF, buf) ~= 0 then
			return nil, string.format("%s 缓冲 %d 入队失败（VIDIOC_QBUF）", path, i)
		end
	end

	local t = ffi.new("unsigned int[1]", BUF_TYPE_VIDEO_CAPTURE)
	if HAL.sys.ioctl_p(fd, VIDIOC_STREAMON, t) ~= 0 then
		return nil, string.format("%s 起流失败（VIDIOC_STREAMOFF/ON）", path)
	end
	state.streaming = true
	state.warmed = false
	return true
end

local function grab_one()
	local fds = ffi.new("cam_pollfd[1]")
	fds[0].fd = state.fd
	fds[0].events = POLLIN
	local pr = tonumber(ffi.C.poll(fds, 1, POLL_TIMEOUT_MS))
	if pr <= 0 then return nil, "等待帧超时（poll）" end

	local buf = ffi.new("cam_buffer")
	buf.type = BUF_TYPE_VIDEO_CAPTURE
	buf.memory = MEMORY_MMAP
	if HAL.sys.ioctl_p(state.fd, VIDIOC_DQBUF, buf) ~= 0 then
		return nil, "取帧失败（VIDIOC_DQBUF）"
	end
	local slot = state.buffers[buf.index + 1]
	local n = tonumber(buf.bytesused)
	local payload = nil
	if slot ~= nil and n > 0 and n <= slot.len then
		payload = ffi.string(slot.ptr, n)
	end
	HAL.sys.ioctl_p(state.fd, VIDIOC_QBUF, buf)	-- 立刻还回队列，别占着缓冲
	if payload == nil then return nil, "帧数据无效（bytesused 越界）" end
	return payload
end

------------------------------------------------------------------
-- 驱动入口
------------------------------------------------------------------
-- 摄像头是**可选**外设：build 不打开设备、也绝不返回 ERR，否则整机上不了电。
-- 设备在第一次 call 时按调用参数惰性打开（参数来自宿主，build 阶段拿不到）。
function build(data)
	local src = data.src
	if src ~= nil and src ~= "" then
		local bytes, err = read_file(src)
		if bytes == nil then return { result = "ERR", message = err } end
		state.sim = { path = src, bytes = bytes }
	end
	return { result = "OK" }
end

function call(data)
	-- 仿真优先：src 由宿主在调用时传入；没给但 build 阶段已按元数据加载过，
	-- 就继续用那一份（src 是会话级配置，不该因为调用没重复传参而失效）
	local src = data.src
	if (src == nil or src == "") and state.sim ~= nil then src = state.sim.path end
	if src ~= nil and src ~= "" then
		if state.sim == nil or state.sim.path ~= src then
			local bytes, err = read_file(src)
			if bytes == nil then return { result = "ERR", message = err } end
			state.sim = { path = src, bytes = bytes }
		end
		return {
			result = "OK",
			source = "file",
			format = "jpeg",
			bytes = #state.sim.bytes,
			b64 = base64(state.sim.bytes),
			note = "模拟图片（src=" .. state.sim.path .. "）",
		}
	end
	state.sim = nil

	-- 真机：路径以本次调用参数为准（空则用缺省）；换设备/热插拔都靠这句重开
	local path = data.path
	if path == nil or path == "" then path = DEFAULT_DEVICE end
	if state.fd < 0 or state.path ~= path then
		device_close()
		local ok, err = device_open(path, data)
		if not ok then return { result = "ERR", message = err } end
		state.path = path
	end

	-- 首帧丢弃（曝光/白平衡未稳定），与 robot_agent 的「丢第一帧」同做法
	if not state.warmed then
		local _, werr = grab_one()
		if werr ~= nil then
			device_close()
			return { result = "ERR", message = werr }
		end
		state.warmed = true
	end

	local payload, err = grab_one()
	if payload == nil then
		device_close()	-- 失败即释放，下次调用重试打开（支持热插拔）
		return { result = "ERR", message = err }
	end
	return {
		result = "OK",
		source = "v4l2",
		format = "jpeg",
		width = state.width,
		height = state.height,
		bytes = #payload,
		b64 = base64(payload),
	}
end

function destroy(data)
	state.sim = nil
	state.path = nil
	device_close()
	return { result = "OK" }
end

