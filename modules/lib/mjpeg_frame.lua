-- mjpeg_frame.lua —— MJPEG 帧完整性校验与收尾规范化
--
-- 独立成库的原因与 llm_wire_* 相同：规则要能离线单测（test/test_camera.lua）。
--
-- 【为什么需要】UVC 摄像头的 MJPEG 帧可能**在熵编码中途截断**（2026-09-19 实测某
-- 设备约半数帧如此：内核不置 V4L2_BUF_FLAG_ERROR，V4L2 层无从感知；宽松解码器
-- libjpeg/ffmpeg 照样出图，但 OpenAI 兼容网关的严格解析器一律 400
-- 「图片输入格式/解析错误」）。截断意味着 MCU 数据缺失，字节级修补（补 EOI、
-- 补填充位）都救不回来——唯一正确的处理是「不完整就整帧丢弃重取」。
--
-- 判据：帧尾是否以 EOI（ffd9）收尾。JPEG 熵编码段内 0xFF 一律字节填充（ff→ff00）、
-- 重启标记是 ffd0-d7，段内不可能出现 ffd9；但 APP1（EXIF）里可能内嵌缩略图自带
-- 完整 JPEG（含它自己的 EOI），因此只在**帧尾窗口**里找，绝不全文搜索。
-- 找到 EOI 时同时截掉其后的尾部填充（实测见过 EOI 后跟 2 字节 0x00 的设备）。
--
-- M.normalize(payload)：
--   完整帧 → 规范化后的帧字节（截到 EOI 为止）
--   截断帧 → nil（调用方应丢弃重取）
if _G.__BOT4CJ_MJPEG_FRAME ~= nil then return _G.__BOT4CJ_MJPEG_FRAME end

local M = {}

local TAIL_WINDOW = 8	-- EOI 搜索窗口：覆盖帧尾填充即可（实测填充 ≤ 2 字节）

function M.normalize(payload)
	if type(payload) ~= "string" or #payload < 4 then return nil end
	local tail = payload:sub(-TAIL_WINDOW)
	local prefix = tail:match(".*\xff\xd9")
	if prefix == nil then return nil end	-- 尾部无 EOI = 熵编码截断
	return payload:sub(1, #payload - #tail + #prefix)
end

_G.__BOT4CJ_MJPEG_FRAME = M
return M
