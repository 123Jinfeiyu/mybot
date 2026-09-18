-- Camera.lua 单元测试：仿真路径 + base64 + 错误路径
-- 用法（项目根目录）: luajit test/test_camera.lua
--
-- 本机没有摄像头也能全跑：驱动支持 src=<图片文件> 的仿真模式，抓帧结果与真机同构
-- （同一段 base64 返回逻辑）。真机 V4L2 路径见 test/test_camera_v4l2.lua，
-- 无 /dev/video* 时由守门按 `-- requires: video` 自动跳过。

local tester = dofile("test/lib/tester.lua")
local check = tester.check

dofile("modules/Camera.lua")	-- 注册全局 build/call/destroy

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
		build({ src = path })
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

	local b = build({ src = path })
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
	local b = build({ src = "/no/such/image.jpg" })
	check("仿真文件不存在 → build ERR", b.result == "ERR")
	check("ERR 消息点名文件", tostring(b.message):find("无法打开模拟图片") ~= nil)
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

tester.summary("Camera.lua 单测")
