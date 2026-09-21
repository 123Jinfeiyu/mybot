-- llm_http.lua —— LLM 驱动共用的 HTTP 传输（LuaJIT ffi 直驱 libcurl）
--
-- 这一层只做「把一段 JSON 文本 POST 出去、把响应文本拿回来」，不认识任何模型
-- 协议；协议差异在各自的驱动（LlmOpenAI.lua / LlmDeepSeek.lua）里。
--
-- 【为什么用 ffi 而不是 shell-out】
--   * 驱动沙箱已中和 os.execute/io.popen（luasandbox.cpp），shell-out 本就不可用；
--   * ffi 直调 libcurl 无需头文件、无需编译期依赖，只要求运行期有 libcurl.so；
--   * HTTPS/TLS 由 libcurl 负责（默认 CA 路径），驱动不必自己碰 OpenSSL。
--
-- 【必须用定签名 cast 调用 curl_easy_setopt】
--   curl_easy_setopt 是变参函数。LuaJIT 的**裸变参调用**会把数字按 double 传，
--   而 libcurl 按 va_arg(ap, long) 读 —— 实测后果是整型选项被**静默忽略**
--   （CURLOPT_TIMEOUT=1 不生效、请求跑满 5 秒且 rc=0，等于 agent 会无限挂起）。
--   因此下面所有整型选项一律走 setopt_long，指针类走 setopt_ptr。
-- 重复加载保护（与 HAL.lua 同一做法）：驱动与 llm_wire_openai.lua 都会 dofile
-- 本文件，而 ffi.cdef 在同一状态机内重复声明同一结构体会直接报错
-- （"attempt to redefine 'curl_slist'"），故用全局缓存保证只初始化一次。
if _G.__BOT4CJ_LLM_HTTP ~= nil then return _G.__BOT4CJ_LLM_HTTP end

local ffi = require("ffi")

-- 模块路径引导（与 llm_wire_openai.lua 同一写法）：本文件被驱动直接 dofile，
-- 也需要 utf8.lua 的消毒兜底，故单独解析一次模块根。
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
local utf8 = dofile(mpath.resolve("lib/utf8.lua"))

ffi.cdef[[
typedef void CURL;
struct curl_slist { char *data; struct curl_slist *next; };
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *h);
void curl_easy_reset(CURL *h);
int curl_easy_setopt(CURL *h, int opt, ...);
int curl_easy_perform(CURL *h);
int curl_easy_getinfo(CURL *h, int info, ...);
const char *curl_easy_strerror(int code);
struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s);
void curl_slist_free_all(struct curl_slist *l);
int curl_global_init(long flags);
]]

local M = {}

-- CURLOPT/CURLINFO 常量（curl ABI 稳定；漂移由 test/test_curl_consts.c 编译期
-- 与 curl.h 比对——F-24 常量自检同族。libcurl 开发头文件非构建依赖，构建机
-- 没装时该自检不产出、run_all.sh 按跳过计；CI 已安装以保证门禁覆盖）
local OPT_URL = 10002
local OPT_POSTFIELDS = 10015
local OPT_POSTFIELDSIZE = 60
local OPT_WRITEFUNCTION = 20011
local OPT_HTTPHEADER = 10023
local OPT_TIMEOUT = 13
local OPT_CONNECTTIMEOUT = 78
local OPT_NOSIGNAL = 99
local INFO_RESPONSE_CODE = 2097154
local CURL_GLOBAL_ALL = 3

local DEFAULT_MAX_RESPONSE = 8 * 1024 * 1024
local DEFAULT_TIMEOUT = 180
local DEFAULT_CONNECT_TIMEOUT = 15
-- 超时钳制区间（秒）。libcurl 语义下 CURLOPT_TIMEOUT=0 是**不限时**，负数同样
-- 透传——配置里一个 0 就能让单次模型调用永远挂着，与全仓 fail-fast 相悖；
-- 上限 600 则防「为了保险配大一点」把交互拖成无界等待。
local MIN_TIMEOUT = 1
local MAX_TIMEOUT = 600

-- 单点钳制（P1-3 防线，2026-09-19 评估）：所有超时都从这里流向 curl_easy_setopt，
-- 非法值（nil/非数字/NaN）回落缺省，越界值夹到区间端点。
function M.clamp_timeout(v, default)
	local n = tonumber(v)
	if n == nil or n ~= n then return default end
	if n < MIN_TIMEOUT then return MIN_TIMEOUT end
	if n > MAX_TIMEOUT then return MAX_TIMEOUT end
	return math.floor(n)
end

-- 响应字节上限钳制（2026-09-20 审计收口，与 clamp_timeout 同构）：
-- 该上限保护写回调里的 `size + len > max_response` 判读——NaN（tonumber("nan")
-- 得到 truthy 的 NaN，不触发 `or` 回落）会使该比较恒为假，让 8 MiB 上限**彻底
-- 失效**、异常服务端可持续追加 chunks 直至内存耗尽。±inf 同理（比较永不真）。
-- 故这里只接受「有限」的值：非数字/nil/NaN/±inf 一律回落缺省；正数按向下
-- 取整进整数域（上限语义）；负值/零则夹到 1（非正上限会让首个 chunk 就误判
-- 溢出，所有请求无条件失败）。
local MIN_MAX_RESPONSE = 1
function M.clamp_max_response(v, default)
	local n = tonumber(v)
	if n == nil or n ~= n or n == math.huge or n == -math.huge then return default end
	if n < MIN_MAX_RESPONSE then return MIN_MAX_RESPONSE end
	return math.floor(n)
end

-- 库名按常见发行版顺序探测：Debian/Ubuntu 为 libcurl.so.4（可能是 gnutls 变体），
-- 开发包提供 libcurl.so
local CANDIDATES = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" }

local curl, setopt_long, setopt_ptr, getinfo_long
local load_err

-- 惰性加载：build() 阶段调用一次即可把「缺库」变成清晰报错，
-- 而不是等到第一次调模型才失败
function M.load()
	if curl then return true end
	if load_err then return false, load_err end
	local tried = {}
	for _, name in ipairs(CANDIDATES) do
		local ok, lib = pcall(ffi.load, name)
		if ok then
			curl = lib
			break
		end
		tried[#tried + 1] = name
	end
	if not curl then
		load_err = "libcurl 不可用（已尝试 " .. table.concat(tried, ", ")
			.. "）；请安装 libcurl4 后重试"
		return false, load_err
	end
	setopt_long = ffi.cast("int (*)(void *, int, long)", curl.curl_easy_setopt)
	setopt_ptr = ffi.cast("int (*)(void *, int, const void *)", curl.curl_easy_setopt)
	getinfo_long = ffi.cast("int (*)(void *, int, long *)", curl.curl_easy_getinfo)
	curl.curl_global_init(CURL_GLOBAL_ALL)
	return true
end

function M.available()
	local ok = M.load()
	return ok
end

-- 注意：这里**刻意**不提供 curl_global_cleanup 的封装。
-- 实测它对本文档开头那批 libcrypto 初始化残留没有任何作用（34 处 / 3.4KB 原样存在），
-- 而 curl 全局态是进程级的，早清理反而会让并存的第二颗驱动句柄失效。
-- 该残留由仓库根的 asan.supp 按库抑制，理由见那里的注释。
-- 需要重载驱动时调 M.load() 即可（幂等）。

local Session = {}
Session.__index = Session

-- 创建一个可复用的会话（保留连接与 TLS 会话，省掉每轮重新握手）。
-- 调用方负责 close；不复用时直接用 M.post。
function M.session()
	local ok, err = M.load()
	if not ok then return nil, err end
	local h = curl.curl_easy_init()
	if h == nil then return nil, "curl_easy_init 失败" end
	return setmetatable({ handle = h }, Session)
end

function Session:close()
	if self.handle then
		curl.curl_easy_cleanup(self.handle)
		self.handle = nil
	end
end

-- opts = { url=, headers={str,...}, body=, timeout=秒, connect_timeout=秒,
--          max_response=字节 }
-- 返回 { ok=true, status=<HTTP码>, body=<响应文本> }
--   或 { ok=false, err=<人话原因>, status=<可选> }
function Session:post(opts)
	if self.handle == nil then return { ok = false, err = "会话已关闭" } end
	local url = opts.url
	if type(url) ~= "string" or url == "" then
		return { ok = false, err = "缺少 url" }
	end

	local max_response = M.clamp_max_response(opts.max_response, DEFAULT_MAX_RESPONSE)
	local chunks, size, overflow = {}, 0, false
	-- 回调里只做「追加」：本回调是在 curl_easy_perform 的 C 栈上被调用的，
	-- 在其中 error 会穿透 C 栈（UB）。超限时 return 0 让 curl 主动中止。
	local cb = ffi.cast("size_t (*)(char *, size_t, size_t, void *)",
		function(p, s, n, _)
			local len = tonumber(s) * tonumber(n)
			if size + len > max_response then
				overflow = true
				return 0
			end
			chunks[#chunks + 1] = ffi.string(p, len)
			size = size + len
			return len
		end)

	curl.curl_easy_reset(self.handle)
	local h = self.handle
	local slist = nil
	for _, hdr in ipairs(opts.headers or {}) do
		slist = curl.curl_slist_append(slist, hdr)
	end
	local body = opts.body or ""
	-- 发包前最后一道兜底（2026-09-21 审计 AG-1）：请求体的非法 UTF-8 会被严格
	-- 网关 400 拒掉，而那条消息可能已经落在**不可回滚**的历史里——会话从此
	-- 每次都以同一字节偏移失败。宿主已在「数据进入模型上下文」的边界消毒
	-- （run_command 输出 / 脚本首注释 / 结构脚本头），此处兜的是宿主没覆盖到
	-- 的路径（例如驱动 extras 里由 provider 文本原样带回的字段）。
	-- 站在 HTTP 层看，请求已经就要发出去了，能做的只有「就地替换 + 让调用方
	-- 看得见」：写 stderr 告警（不静默），再把坏字节替换成 U+FFFD 后照发——
	-- 与宿主侧同一套替换文案，排查时两边对得上。
	if not utf8.valid(body) then
		io.stderr:write("警告: LLM 请求体含非法 UTF-8 字节，已就地替换为 U+FFFD"
			.. "（严格网关会以 400 拒绝此类请求；请检查工具输出与提示词来源）\n")
		body = utf8.sanitize(body)
	end
	setopt_ptr(h, OPT_URL, url)
	setopt_ptr(h, OPT_POSTFIELDS, body)
	setopt_long(h, OPT_POSTFIELDSIZE, #body)
	setopt_ptr(h, OPT_WRITEFUNCTION, cb)
	if slist ~= nil then setopt_ptr(h, OPT_HTTPHEADER, slist) end
	setopt_long(h, OPT_TIMEOUT, M.clamp_timeout(opts.timeout, DEFAULT_TIMEOUT))
	setopt_long(h, OPT_CONNECTTIMEOUT,
		M.clamp_timeout(opts.connect_timeout, DEFAULT_CONNECT_TIMEOUT))
	setopt_long(h, OPT_NOSIGNAL, 1)

	local rc = curl.curl_easy_perform(h)
	local code = ffi.new("long[1]")
	getinfo_long(h, INFO_RESPONSE_CODE, code)
	if slist ~= nil then curl.curl_slist_free_all(slist) end
	-- 句柄收尾顺序有讲究（2026-09-21 审计 AG-16）：**先 reset 再 free 回调**。
	-- 每请求 cast 出的回调 cdata 用了就释放，否则 LuaJIT 的回调槽位要等 GC 才
	-- 归还，长驻 agent 会话高频请求下会持续累积（这是 cb:free() 的动机）。
	-- 但旧实现只 free 不清句柄：WRITEFUNCTION/URL/POSTFIELDS/HTTPHEADER 仍指在
	-- 已释放的对象上。当前之所以没出事，只因 Session:post 每次都先
	-- curl_easy_reset——而 Session.handle 是**公开字段**，任何「一个句柄 perform
	-- 两次 / 加重试 / 多句柄」的重构都会把它变成内存安全问题（实测：post 一次
	-- 后直接 perform 报 bad callback；连做 500 次 post 让槽位被复用后再直接
	-- perform，SIGSEGV）。reset 是零成本的（不发网络请求），把这条隐含前提
	-- 变成句柄自身的不变量。注意顺序：reset 必须在 free 之前，否则 reset 期间
	-- 若 libcurl 触碰该指针就已经晚了。
	curl.curl_easy_reset(h)
	cb:free()

	if overflow then
		return { ok = false, err = "响应超过 " .. max_response .. " 字节上限" }
	end
	if rc ~= 0 then
		return { ok = false, err = "curl rc=" .. tonumber(rc) .. ": "
			.. ffi.string(curl.curl_easy_strerror(rc)) }
	end
	return { ok = true, status = tonumber(code[0]), body = table.concat(chunks) }
end

-- 一次性请求（内部建/销会话）
function M.post(opts)
	local session, err = M.session()
	if session == nil then return { ok = false, err = err } end
	local res = session:post(opts)
	session:close()
	return res
end

_G.__BOT4CJ_LLM_HTTP = M
return M
