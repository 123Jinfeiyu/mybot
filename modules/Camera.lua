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
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local HAL = dofile(mpath.resolve("lib/HAL.lua"))	-- 复用底座：open/close/ioctl 已在其中声明
local mjpeg = dofile(mpath.resolve("lib/mjpeg_frame.lua"))
-- 统一 ERR 构造器（2026-09-21，D2-2）：内联 {result="ERR",message=…} 收敛到
-- drv_common.err()。Camera 仍不挂 arg_table（可选外设白名单豁免，见 docs/drivers.md
-- 的 destroy/入口节），这里只取 err() 一个原语。
dofile(mpath.resolve("lib/drv_common.lua"))	-- 帧完整性校验（截断帧重取，见库头注释）
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
char *realpath(const char *path, char *resolved);
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
-- 仿真路径白名单（P1-2 防线，2026-09-19 评估）
------------------------------------------------------------------
-- 仿真模式直接经 ffi.C.fopen 读文件并 base64 回传，若不设约束，就等于给出一条
-- 「读任意路径」的原语——事实绕过沙箱对 io.open 的封禁（权限允许时 /etc/shadow
-- 也会被读走并进入模型上下文）。因此只放行白名单目录**之内**的文件：
--   1) 宿主传入的 src_root（agent 模式取 --camera-src 的所在目录）；
--   2) 环境变量 BOT4CJ_CAMERA_SRC_DIR（冒号分隔，供结构脚本直驱场景显式放行）。
-- 两者都没有 = 仿真模式整体禁用（默认拒绝）。判定用 realpath，符号链接会被
-- 展开后再比较，故「白名单目录内放个软链指向外部」同样被拒。
local PATH_MAX = 4096

local function real_path(path)
	local buf = ffi.new("char[?]", PATH_MAX)
	local p = ffi.C.realpath(path, buf)
	if p == nil then return nil end
	return ffi.string(p)
end

local function path_within(path, root)
	return path == root or path:sub(1, #root + 1) == root .. "/"
end

local function allowed_roots(data)
	local roots = {}
	local function add(p)
		if type(p) ~= "string" or p == "" then return end
		local r = real_path(p)
		if r ~= nil then roots[#roots + 1] = r end
	end
	add(data.src_root)
	local env = os.getenv("BOT4CJ_CAMERA_SRC_DIR")
	if type(env) == "string" then
		for seg in env:gmatch("[^:]+") do add(seg) end
	end
	return roots
end

-- 返回白名单内的规范化路径；越界/不存在/未放行都返回 nil + 原因
local function resolve_sim_path(data)
	local src = data.src
	if type(src) ~= "string" or src == "" then return nil, "缺少模拟图片路径" end
	local real = real_path(src)
	if real == nil then return nil, "无法打开模拟图片 " .. src end
	local roots = allowed_roots(data)
	if #roots == 0 then
		return nil, "仿真图片未放行：" .. src
			.. "（请用 --camera-src 指定，或在 BOT4CJ_CAMERA_SRC_DIR 里列出允许目录）"
	end
	for _, root in ipairs(roots) do
		if path_within(real, root) then return real end
	end
	return nil, "模拟图片超出允许目录（白名单）：" .. src
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
	-- 2026-09-20 审计 C6：path 的语义是「当前**处于打开状态**的设备」，
	-- 必须随 fd 一起清。旧实现只在 destroy 里清、成功路径由调用方补写，
	-- 于是「上一次成功打开 A、这一次打开 B 失败」会留下 fd≥0 且 path==A
	-- 的组合，调用方的 `state.fd < 0 or state.path ~= path` 判定 B 已打开，
	-- 直接拿半初始化的 fd 去抓帧。
	state.path = nil
end

-- 打开失败的统一出口：**先释放本次已申请的全部资源**（fd + 已 mmap 的缓冲）
-- 再返回错误。旧实现八条失败路径都直接 `return nil, ...`，fd 与 mmap 缓冲区
-- 就此泄漏（`state.fd` 早在校验之前就已赋值），且状态被留在「半开」。
local function open_fail(msg)
	device_close()
	return nil, msg
end

local function device_open(path, data)
	local fd = HAL.sys.open(path, O_RDWR + HAL.O_CLOEXEC)
	if fd < 0 then
		return open_fail(string.format("无法打开摄像头 %s（errno=%d）：设备不存在、被占用或权限不足",
			path, HAL.sys.errno()))
	end
	state.fd = fd

	local cap = ffi.new("cam_capability")
	if HAL.sys.ioctl_p(fd, VIDIOC_QUERYCAP, cap) ~= 0 then
		return open_fail(string.format("%s 不是 V4L2 设备（VIDIOC_QUERYCAP 失败）", path))
	end
	local caps = cap.capabilities
	if bit.band(caps, CAP_VIDEO_CAPTURE) == 0 then
		return open_fail(string.format("%s 不支持视频采集（capabilities=0x%x）", path, caps))
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
		return open_fail(string.format("%s 设置格式失败（%dx%d MJPEG）", path, width, height))
	end
	if fmt.fmt.pix.pixelformat ~= PIX_FMT_MJPEG then
		return open_fail(string.format(
			"%s 不支持 MJPEG（实际 fourcc=0x%x）。本驱动只取硬件编码的 MJPEG："
			.. "请换支持 MJPEG 的设备，或用 src 参数走模拟图片",
			path, fmt.fmt.pix.pixelformat))
	end
	state.width = fmt.fmt.pix.width
	state.height = fmt.fmt.pix.height

	local req = ffi.new("cam_requestbuffers")
	req.count = BUFFER_COUNT
	req.type = BUF_TYPE_VIDEO_CAPTURE
	req.memory = MEMORY_MMAP
	if HAL.sys.ioctl_p(fd, VIDIOC_REQBUFS, req) ~= 0 or req.count < 2 then
		return open_fail(string.format("%s 申请 mmap 缓冲失败（VIDIOC_REQBUFS）", path))
	end

	for i = 0, req.count - 1 do
		local buf = ffi.new("cam_buffer")
		buf.type = BUF_TYPE_VIDEO_CAPTURE
		buf.memory = MEMORY_MMAP
		buf.index = i
		if HAL.sys.ioctl_p(fd, VIDIOC_QUERYBUF, buf) ~= 0 then
			return open_fail(string.format("%s 查询缓冲 %d 失败（VIDIOC_QUERYBUF）", path, i))
		end
		local ptr = ffi.C.mmap(nil, buf.length, bit.bor(PROT_READ, PROT_WRITE), MAP_SHARED, fd, 0)
		if ptr == ffi.cast("void *", -1) then
			return open_fail(string.format("%s mmap 缓冲 %d 失败", path, i))
		end
		state.buffers[i + 1] = { ptr = ptr, len = buf.length }
		if HAL.sys.ioctl_p(fd, VIDIOC_QBUF, buf) ~= 0 then
			return open_fail(string.format("%s 缓冲 %d 入队失败（VIDIOC_QBUF）", path, i))
		end
	end

	local t = ffi.new("unsigned int[1]", BUF_TYPE_VIDEO_CAPTURE)
	if HAL.sys.ioctl_p(fd, VIDIOC_STREAMON, t) ~= 0 then
		return open_fail(string.format("%s 起流失败（VIDIOC_STREAMOFF/ON）", path))
	end
	state.streaming = true
	state.warmed = false
	state.path = path	-- C6：path 只在**成功**时登记，且由 device_close 清除
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
-- 摄像头是**可选**外设：build 不打开真实设备（推迟到首次 call 惰性打开），
-- 因此没接摄像头也能上电。但 `--camera-src` **仿真来源**是显式给定的：
-- 路径越界 / 文件缺失 / 读取失败时**刻意返回 ERR**（拒绝一次注定失败的仿真，
-- 有 test_camera.lua 的负路径覆盖）；真实设备路径下 build 不会返回 ERR。
-- 设备在第一次 call 时按调用参数惰性打开（参数来自宿主，build 阶段拿不到）。
-- 入参类型校验不可省（全仓其余驱动统一经 drv_common.arg_table 首位校验；
-- Camera 因不挂总线样板而自行内联同一条——非表入参按 ERR 契约拒绝，
-- 而不是让 data.src 的索引访问裸抛 Lua error）。ERR 表一律走 drv_common.err()。
--
-- 例外（2026-09-21 审计 AG-2）：err 是本 lua_State 的**共享全局**，任何驱动或
-- .mt 脚本都能覆盖它，而本文件历史上正是覆盖者（抓帧循环误写全局 err，见 call()
-- 内注释）。构造器一旦没了，`return err(...)` 自己就抛裸 Lua error——跨语言边界上
-- 「抛」与「返 ERR」对宿主的处理路径完全不同。故入口先做一次前置校验，缺失时
-- 直接返回同构的 ERR 表（fail-closed，且理由是显式的）。
local ERR_NO_CTOR = {
	result = "ERR",
	message = "driver error constructor 'err' is unavailable (clobbered global in this Lua state)",
}
local function err_missing()
	return type(err) ~= "function"
end

function build(data)
	if err_missing() then return ERR_NO_CTOR end
	if type(data) ~= "table" then
		return err("Invalid Arguments Type: table")
	end
	local src = data.src
	if src ~= nil and src ~= "" then
		local real, perr = resolve_sim_path(data)
		if real == nil then return err(perr) end
		local bytes, rerr = read_file(real)
		if bytes == nil then return err(rerr) end
		state.sim = { path = real, bytes = bytes }
	end
	return { result = "OK" }
end

function call(data)
	if err_missing() then return ERR_NO_CTOR end
	if type(data) ~= "table" then
		return err("Invalid Arguments Type: table")
	end
	-- 仿真优先：src 由宿主在调用时传入；没给但 build 阶段已按元数据加载过，
	-- 就继续用那一份（src 是会话级配置，不该因为调用没重复传参而失效）。
	-- 沿用 build 已放行的那份时不再重新校验：路径当时已 realpath 规范化，
	-- 且本次调用不带 src_root 也没有可校验的依据。
	local real = nil
	local src = data.src
	if src ~= nil and src ~= "" then
		local perr
		real, perr = resolve_sim_path(data)
		if real == nil then return err(perr) end
	elseif state.sim ~= nil then
		real = state.sim.path
	end
	if real ~= nil then
		if state.sim == nil or state.sim.path ~= real then
			local bytes, rerr = read_file(real)
			if bytes == nil then return err(rerr) end
			state.sim = { path = real, bytes = bytes }
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
	-- P1-2 防线：只放行 /dev/videoN 设备节点。否则 path 等价于一个「打开任意
	-- 路径」的原语（普通文件、其他设备节点都会被 HAL.sys.open 照单全收）。
	if type(path) ~= "string" or path:match("^/dev/video%d+$") == nil then
		return err("camera device path must be /dev/videoN: " .. tostring(path))
	end
	if state.fd < 0 or state.path ~= path then
		device_close()
		local ok, oerr = device_open(path, data)
		if not ok then return err(oerr) end
	end

	-- 首帧丢弃（曝光/白平衡未稳定），与 robot_agent 的「丢第一帧」同做法
	if not state.warmed then
		local _, werr = grab_one()
		if werr ~= nil then
			device_close()
			return err(werr)
		end
		state.warmed = true
	end

	-- 帧完整性重试：UVC 设备可能吐出熵编码中途截断的帧（机理与实测数据见
	-- lib/mjpeg_frame.lua 头注释）。截断帧宽松解码器看不出问题，送到模型网关
	-- 就是 400「图片输入格式/解析错误」——必须整帧丢弃重取。上限 8 帧（30fps
	-- 下约 0.3s）：实测该类设备约半数帧不完整，8 连缺已属设备/链路异常。
	--
	-- 局部 gerr 承接 grab_one 的失败原因。**不能写成全局 err**（2026-09-21 审计
	-- AG-2，实测复现）：err 是 drv_common 定义的全局构造器，这样写会把整个
	-- lua_State 的 err 覆盖掉——成功路径赋成 nil、抓帧失败赋成字符串，此后
	-- 本进程内**任何**驱动的 err(...) 都变成「attempt to call a nil/string value」，
	-- 且下一行那句诊断自己先撞上 nil/string，永远返回不了。
	local payload, gerr
	for _ = 1, 8 do
		payload, gerr = grab_one()
		if payload == nil then break end
		payload = mjpeg.normalize(payload)
		if payload ~= nil then break end
	end
	if payload == nil then
		device_close()	-- 失败即释放，下次调用重试打开（支持热插拔）
		-- 这里可以放心调 err：入口的前置校验保证了它在本函数体内一直是函数
		-- （本函数从入口到此处不再出现任何对 err 的赋值）。
		return err(gerr or
			"8 consecutive incomplete frames (MJPEG frame missing EOI): device/link fault, check the camera connection")
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

