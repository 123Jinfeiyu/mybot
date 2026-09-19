-- Camera.lua 单元测试：仿真路径 + base64 + 错误路径
-- 用法（项目根目录）: luajit test/test_camera.lua
--
-- 本机没有摄像头也能全跑：驱动支持 src=<图片文件> 的仿真模式，抓帧结果与真机同构
-- （同一段 base64 返回逻辑）。真机 V4L2 路径见 test/test_camera_v4l2.lua，
-- 无 /dev/video* 时由守门按 `-- requires: video` 自动跳过。

local tester = dofile("test/lib/tester.lua")
local check = tester.check

dofile("modules/Camera.lua")	-- 注册全局 build/call/destroy

local ffi = require("ffi")
ffi.cdef[[
int symlink(const char *target, const char *linkpath);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
]]
-- 用例确定性：环境变量会改变仿真白名单，先清掉
ffi.C.unsetenv("BOT4CJ_CAMERA_SRC_DIR")

-- 软链与临时目录（不依赖 shell 也能建链，建目录仍需 mkdir）
local function tmpdir(tag)
	local d = os.tmpname() .. "." .. tag
	os.execute("mkdir -p " .. d)
	return d
end

local function dirname(p)
	return p:match("^(.*)/[^/]*$")
end

local function write_file(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

-- base64 解码：用来验证驱动返回的 b64 与源字节逐字节一致（不依赖外部库）
local function b64decode(s)
	local map, chars = {}, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	for i = 1, #chars do map[chars:sub(i, i)] = i - 1 end
	local out, n, acc, bits = {}, 0, 0, 0
	for i = 1, #s do
		local c = s:sub(i, i)
		if c == "=" then break end
		local v = map[c]
		if v == nil then error("非法 base64 字符: " .. c) end
		acc = acc * 64 + v
		bits = bits + 6
		if bits >= 8 then
			bits = bits - 8
			local byte = math.floor(acc / 2 ^ bits) % 256
			acc = acc % 2 ^ bits
			n = n + 1
			out[n] = string.char(byte)
		end
	end
	return table.concat(out)
end

-- ---- base64 已知向量（经仿真路径间接验证编码器）----
do
	local vectors = {
		{ "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
		{ "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
	}
	for i, v in ipairs(vectors) do
		local path = os.tmpname()
		write_file(path, v[1])
		build({ src = path, src_root = dirname(path) })
		local res = call({})
		check("base64 向量 #" .. i .. " (" .. v[1] .. ")", res.b64 == v[2],
			tostring(res.b64) .. " 期望 " .. v[2])
		destroy({})
		os.remove(path)
	end
end

-- ---- 仿真抓帧：返回的字节必须与源文件完全一致 ----
do
	local path = os.tmpname()
	-- 造一段带二进制字节的假 JPEG（驱动只负责搬运，不解释内容）
	local payload = "\255\216\255\224" .. string.rep("\0\1\2\3\254\253", 5000) .. "\255\217"
	write_file(path, payload)

	local b = build({ src = path, src_root = dirname(path) })
	check("仿真 build 返回 OK", b.result == "OK")
	local res = call({})
	check("仿真抓帧返回 OK", res.result == "OK")
	check("来源标记为 file", res.source == "file")
	check("格式标记为 jpeg", res.format == "jpeg")
	check("字节数与源一致", res.bytes == #payload, tostring(res.bytes) .. " vs " .. #payload)
	check("base64 解码回源字节（逐字节一致）", b64decode(res.b64) == payload)
	check("仿真结果带 note 说明", type(res.note) == "string" and res.note:find("模拟图片") ~= nil)
	check("destroy 返回 OK", destroy({}).result == "OK")
	os.remove(path)
end

-- ---- 仿真文件不存在：build 失败要清晰，而不是拖到抓帧 ----
do
	-- 目录在白名单内（/tmp 存在），只是文件不存在 → 走「打不开」而不是「未放行」
	local b = build({ src = "/tmp/mybot-no-such-image.jpg", src_root = "/tmp" })
	check("仿真文件不存在 → build ERR", b.result == "ERR")
	check("ERR 消息点名文件", tostring(b.message):find("无法打开模拟图片", 1, true) ~= nil,
		tostring(b.message))
	destroy({})
end

-- ---- 入参类型守卫：Camera 是全仓唯一不挂 drv_common.arg_table 样板的驱动，
-- ---- 非表入参必须按 ERR 契约拒绝，而不是 data.src 索引访问裸抛 Lua error ----
do
	local b = build("not-a-table")
	check("build 非表入参 → ERR（契约表，不裸抛）",
		type(b) == "table" and b.result == "ERR", tostring(type(b) == "table" and b.message))
	check("build 非表入参提示类型错误",
		tostring(type(b) == "table" and b.message):find("Invalid Arguments Type", 1, true) ~= nil,
		tostring(type(b) == "table" and b.message))
	local r = call(42)
	check("call 非表入参 → ERR（契约表，不裸抛）",
		type(r) == "table" and r.result == "ERR", tostring(type(r) == "table" and r.message))
	destroy({})
end

-- ---- 仿真白名单（P1-2）：默认拒绝 / 越界拒绝 / 软链逃逸拒绝 / 环境变量放行 ----
do
	local root, other = tmpdir("cam-root"), tmpdir("cam-other")
	local inside, outside = root .. "/pic.jpg", other .. "/secret.bin"
	write_file(inside, "IN")
	write_file(outside, "SECRET")

	-- 1) 既无 src_root 也无环境变量：默认拒绝（不给「读任意路径」留口子）
	local b0 = build({ src = inside })
	check("未放行目录 → build ERR（默认拒绝）", b0.result == "ERR", tostring(b0.message))
	check("默认拒绝提示指向 --camera-src",
		tostring(b0.message):find("未放行", 1, true) ~= nil, tostring(b0.message))
	destroy({})

	-- 2) 给了 src_root 但 src 在其外：拒绝
	local b1 = build({ src = outside, src_root = root })
	check("白名单目录外 → build ERR", b1.result == "ERR", tostring(b1.message))
	check("越界提示含「白名单」",
		tostring(b1.message):find("白名单", 1, true) ~= nil, tostring(b1.message))
	destroy({})

	-- 3) 目录内软链指向外部：realpath 展开后仍被判越界
	local link = root .. "/link.jpg"
	if ffi.C.symlink(outside, link) ~= 0 then
		os.execute("ln -sf " .. outside .. " " .. link)
	end
	local b2 = build({ src = link, src_root = root })
	check("软链逃逸白名单 → build ERR", b2.result == "ERR", tostring(b2.message))
	check("软链逃逸提示含「白名单」",
		tostring(b2.message):find("白名单", 1, true) ~= nil, tostring(b2.message))
	destroy({})

	-- 4) 环境变量放行（结构脚本直驱场景的显式开关）
	ffi.C.setenv("BOT4CJ_CAMERA_SRC_DIR", root, 1)
	local b3 = build({ src = inside })
	check("BOT4CJ_CAMERA_SRC_DIR 放行目录内文件", b3.result == "OK", tostring(b3.message))
	local r3 = call({})
	check("环境变量放行后抓帧成功", r3.result == "OK" and r3.source == "file")
	destroy({})
	ffi.C.unsetenv("BOT4CJ_CAMERA_SRC_DIR")

	os.execute("rm -rf " .. root .. " " .. other)
end

-- ---- 设备路径白名单（P1-2）：只放行 /dev/videoN ----
do
	local res = call({ path = "/etc/passwd" })
	check("非 /dev/videoN 设备路径 → ERR", res.result == "ERR")
	check("提示只允许 /dev/videoN",
		tostring(res.message):find("/dev/videoN", 1, true) ~= nil, tostring(res.message))
	check("拒绝时点名给的路径",
		tostring(res.message):find("/etc/passwd", 1, true) ~= nil, tostring(res.message))
	destroy({})
end

-- ---- 摄像头是可选外设：build 不碰设备，抓拍时才按调用参数惰性打开并报错 ----
do
	local b = build({ path = "/dev/video999" })
	check("build 返回 OK 且不警告（不在上电阶段碰设备）", b.result == "OK" and b.warning == nil)
	local res = call({ path = "/dev/video999" })
	check("抓帧时才如实报 ERR", res.result == "ERR")
	check("ERR 消息含调用时给的设备路径", tostring(res.message):find("/dev/video999") ~= nil,
		tostring(res.message))
	check("ERR 消息点明 errno", tostring(res.message):find("errno=") ~= nil)
	destroy({})
end

-- ---- MJPEG 帧完整性校验（lib/mjpeg_frame.lua）：截断帧重取的判据 ----
do
	local mjpeg = dofile("modules/lib/mjpeg_frame.lua")

	local SOI = "\xff\xd8"
	local EOI = "\xff\xd9"
	local scan = string.rep("\x5a\xa5", 64)	-- 假熵编码数据（不含 ffd9 组合）

	local full = SOI .. scan .. EOI
	check("完整帧原样返回", mjpeg.normalize(full) == full)
	check("EOI 后的尾部填充被截掉（实测设备会跟 2 字节 0x00）",
		mjpeg.normalize(full .. "\x00\x00") == full)
	check("截断帧（尾部无 EOI）返回 nil，调用方应重取",
		mjpeg.normalize(SOI .. scan) == nil)

	-- EXIF 缩略图陷阱：APP 段内嵌完整 JPEG（含它自己的 EOI）。只搜帧尾窗口，
	-- 内嵌 EOI 不得把截断帧误判成完整，也不得让规范化截在缩略图上。
	local thumbnail = SOI .. "\x11\x22" .. EOI
	check("内嵌缩略图的截断帧仍判为截断",
		mjpeg.normalize(SOI .. thumbnail .. scan) == nil)
	check("主扫描完整时截到主 EOI（内嵌 EOI 不干扰）",
		mjpeg.normalize(SOI .. thumbnail .. scan .. EOI .. "\x00")
			== SOI .. thumbnail .. scan .. EOI)

	check("过短数据一律 nil", mjpeg.normalize(SOI) == nil
		and mjpeg.normalize("") == nil and mjpeg.normalize(nil) == nil)
end

tester.summary("Camera.lua 单测")
