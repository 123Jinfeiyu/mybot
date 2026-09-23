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
void *fopen(const char *path, const char *mode);
int fclose(void *fp);
]]

local M = {}

-- 会话创建计数：每次 M.session() 成功即 +1。守门用它做**决定性**判据——
-- **句柄地址不能当判据**：libcurl 每次 init 申请同样大小的块，free 之后 malloc
-- 常把同一地址还回来，实测「每次新建」也能得到同一个地址（反向验证当场抓到
-- 这条假绿）。计数没有歧义：复用 ⇒ 计数不变，每次新建 ⇒ 计数递加。
-- 声明必须在 M.session 之前（Lua 的 local 只从声明点起生效，写在文件后半段
-- 会让前半段的引用变成全局变量——实测报 "arithmetic on global 'session_seq'"）。
local session_seq = 0

-- CURLOPT/CURLINFO 常量（curl ABI 稳定；漂移由 test/test_curl_consts.c 编译期
-- 与 curl.h 比对——F-24 常量自检同族。libcurl 开发头文件非构建依赖，构建机
-- 没装时该自检不产出、run_all.sh 按跳过计；CI 已安装以保证门禁覆盖）
local OPT_URL = 10002
local OPT_POSTFIELDS = 10015
local OPT_COPYPOSTFIELDS = 10165
local OPT_POSTFIELDSIZE = 60
local OPT_WRITEFUNCTION = 20011
local OPT_HTTPHEADER = 10023
local OPT_TIMEOUT = 13
local OPT_CONNECTTIMEOUT = 78
local OPT_NOSIGNAL = 99
local OPT_NOPROGRESS = 43
local OPT_XFERINFOFUNCTION = 20219
local OPT_HEADERFUNCTION = 20079
local INFO_RESPONSE_CODE = 2097154
local CURL_GLOBAL_ALL = 3
local RC_ABORTED_BY_CALLBACK = 42

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
local loaded_name	-- 实际 dlopen 成功的那个名字（见 M.libname）

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
			loaded_name = name
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

-- 本模块实际加载的 libcurl 名字（未加载时返回 nil）。
--
-- 【为什么必须暴露它】同一进程里**只能有一份 libcurl 实例**。`CANDIDATES` 里的
-- 名字在有的发行版上指向**不同的文件**：本机（Raspberry Pi OS）
-- `libcurl.so → libcurl-gnutls.so`，而 `libcurl.so.4 → libcurl.so.4.8.0`，
-- 于是 `ffi.load("libcurl.so.4")` 与 `ffi.load("curl")` 得到两份独立实例
-- （实测 `curl_easy_init` 地址 0x7fff802a6d80 vs 0x7fff7eb65c60）。
-- 用 B 实例的函数去读 A 实例创建的 easy handle，等于按**另一套结构体布局**
-- 解引用不透明指针：`CURLINFO_EFFECTIVE_URL` 会读出错位字段（实测返回端口串
-- `"1"` 这样的垃圾，而同一句柄用 A 实例读是正确的 URL / reset 后为空）。
-- 这正是 AG-16 门禁在本机假红的成因。任何别的加载者（测试、第三方驱动）
-- 都必须用本函数返回的名字，才能与本模块共享同一份实例。
function M.libname()
	return loaded_name
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
-- 调用方负责 close；**池化复用**（agent 每轮模型调用）走下面的 M.post。
function M.session()
	local ok, err = M.load()
	if not ok then return nil, err end
	local h = curl.curl_easy_init()
	if h == nil then return nil, "curl_easy_init 失败" end
	session_seq = session_seq + 1
	return setmetatable({ handle = h }, Session)
end

function Session:close()
	if self.handle then
		curl.curl_easy_cleanup(self.handle)
		self.handle = nil
	end
end

-- 把句柄整个换掉（**显式调用**，请求路径不再自动调它）。
-- 用途：要丢掉全部连接缓存时的一次性重置（例如切换接入点）。
-- 为什么请求路径不自动重建（AG-20 的反向验证逼出来的第二条）：连接层失败
-- 并不需要换句柄——libcurl 自己会把那条连接标记为不可复用——而每次失败都
-- curl_easy_init 一份新句柄会**按失败次数泄漏**（LSan 实测约 71 字节/次，
-- share/DNS/TLS-session 缓存收不干净）。
function Session:recreate()
	if self.handle then
		curl.curl_easy_cleanup(self.handle)
		self.handle = nil
	end
	local h = curl.curl_easy_init()
	if h == nil then return false end
	self.handle = h
	return true
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

	-- 取消哨兵（P2a，2026-09-22）：宿主收到 SIGINT 时在 BOT4CJ_LLM_CANCEL_FILE
	-- 创建哨兵文件；libcurl 的进度回调（缺省约 1 秒一次）发现它存在即返回非 0
	-- 中止请求——模型调用因此能被 Ctrl-C **及时**打断，而不是只能等满
	-- CURLOPT_TIMEOUT（最长 600s）或按第二次 Ctrl-C 强杀（跳过 deactivate）。
	-- 跨层走文件系统是刻意的：驱动沙箱拿不到宿主内存，文件是零新依赖的通道。
	-- CURLOPT_NOPROGRESS 必须显式置 0：libcurl 缺省为 1（不调进度回调），
	-- 不置位则哨兵永远不会被检查——这条与「整型选项必须 setopt_long」是同一族。
	local cancel_path = os.getenv("BOT4CJ_LLM_CANCEL_FILE")
	local xcb = nil
	if cancel_path ~= nil and cancel_path ~= "" then
		xcb = ffi.cast("int (*)(void *, double, double, double, double)",
			function(_, _, _, _, _)
				-- 不用 io.open：驱动沙箱已移除 io.open（防驱动直接读写文件），
				-- 但 ffi 可用——直调 libc fopen/fclose 只做「存在性探测」，不开读写通道
				local f = ffi.C.fopen(cancel_path, "r")
				if f ~= nil then
					ffi.C.fclose(f)
					return 1	-- 非 0：libcurl 以 CURLE_ABORTED_BY_CALLBACK(42) 中止
				end
				return 0
			end)
	end

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
	-- M4 修复（2026-09-23）：CURLOPT_POSTFIELDS **不拷贝**传入的指针，而 Lua 字符串
	-- 由 GC 管理，curl_easy_perform 期间若触发 GC 就可能移动或释放该字符串——当前
	-- 仅因 body 是本函数的 live local 才不出事（变量栈上有引用、GC 不会回收）。
	-- 用 CURLOPT_COPYPOSTFIELDS 让 libcurl 在 setopt 时立即拷贝一份副本到句柄内部，
	-- 后续与 Lua 字符串生命周期脱钩；reset 时 libcurl 自动释放该副本。
	-- URL 同理：CURLOPT_URL 也不拷贝，但 url 同样是本函数 live local，当前安全。
	setopt_ptr(h, OPT_URL, url)
	setopt_ptr(h, OPT_COPYPOSTFIELDS, body)
	setopt_long(h, OPT_POSTFIELDSIZE, #body)
	setopt_ptr(h, OPT_WRITEFUNCTION, cb)
	-- Retry-After 捕获（SAN-3，2026-09-22）：限流场景下**服务端说了等多久**，
	-- 而本地指数退避（1s/2s/4s）与真实限流窗口毫无关系。libcurl 不会把响应头
	-- 交给我们（此前全仓没有任何头回调声明），所以挂一个只认这一行的头回调。
	-- 只解析 delta-seconds 形态（HTTP-date 形态少见且要时区处理，不值得引入
	-- 依赖；那类响应退化为本地指数退避，不影响正确性）。
	-- 上限 8 位数字：坏网关回一个天文数字不能让宿主把它当成毫秒数用。
	local retry_after_raw = nil
	local hcb = ffi.cast("size_t (*)(char *, size_t, size_t, void *)",
		function(p, s, n, _)
			local len = tonumber(s) * tonumber(n)
			if retry_after_raw == nil and len > 0 and len <= 256 then
				local line = ffi.string(p, len)
				local v = line:match("^[Rr]etry%-[Aa]fter:%s*(%d%d?%d?%d?%d?%d?%d?%d?%d?)%s*$")
				if v ~= nil then retry_after_raw = tonumber(v) end
			end
			return len	-- 必须返回收到的字节数，否则 libcurl 判回调失败
		end)
	setopt_ptr(h, OPT_HEADERFUNCTION, hcb)
	if slist ~= nil then setopt_ptr(h, OPT_HTTPHEADER, slist) end
	setopt_long(h, OPT_TIMEOUT, M.clamp_timeout(opts.timeout, DEFAULT_TIMEOUT))
	setopt_long(h, OPT_CONNECTTIMEOUT,
		M.clamp_timeout(opts.connect_timeout, DEFAULT_CONNECT_TIMEOUT))
	setopt_long(h, OPT_NOSIGNAL, 1)
	if xcb ~= nil then
		setopt_long(h, OPT_NOPROGRESS, 0)	-- 缺省 1 = 不调进度回调，哨兵将永远不被检查
		setopt_ptr(h, OPT_XFERINFOFUNCTION, xcb)
	end

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
	-- 若 libcurl 触碰该指针就已经晚了；也必须在 getinfo **之后**（reset 会把
	-- 响应码一起清掉，成功路径要留着它）。
	curl.curl_easy_reset(h)
	cb:free()
	if xcb ~= nil then xcb:free() end	-- 与 cb 同序：先 reset 再 free（AG-16）
	hcb:free()	-- 同上：头回调也必须在 reset 之后释放（AG-16 的同一族约束）

	-- 连接层失败（rc≠0）**刻意不重建句柄**（AG-20 的反向验证逼出来的第二条）：
	-- 曾在这里做「坏了就换新」，LSan 实测**每次失败请求泄漏约 71 字节**
	-- （easy handle 的 share / DNS / TLS-session 缓存随 curl_easy_init 走，
	-- cleanup 收不干净），断网重试场景下线性增长；实验证明与句柄复用无关
	-- （每请求 cleanup+init 同样泄漏，成功请求不泄漏）。而它本来也不必要：
	-- libcurl 在 perform 返回错误时会自己把那条连接标记为不可复用。
	-- 真正需要换句柄的场景（如切换接入点后要丢掉全部连接缓存）由调用方显式
	-- 调 Session:recreate。
	if overflow then
		return { ok = false, err = "响应超过 " .. max_response .. " 字节上限" }
	end
	if rc == RC_ABORTED_BY_CALLBACK then
		-- 取消哨兵命中：人话报错，宿主与操作者都看得出「这是被打断，不是故障」；
		-- 消息刻意不含「网络」等重试关键词（agentdriver 的可重试判据据此跳过）
		return { ok = false, err = "请求已取消（宿主收到停止信号）", cancelled = true }
	end
	if rc ~= 0 then
		-- curl_rc（SAN-3）：数值退出码是**结构化判据**，比让人去认
		-- curl_easy_strerror 的英文散文可靠得多（实测那些散文一个都不匹配
		-- 宿主原来的六个 CURLE_* 子串，重试因此在生产路径上从未生效）。
		return { ok = false, err = "curl rc=" .. tonumber(rc) .. ": "
			.. ffi.string(curl.curl_easy_strerror(rc)), curl_rc = tonumber(rc) }
	end
	local out = { ok = true, status = tonumber(code[0]), body = table.concat(chunks) }
	-- 只有真解析出数字才挂这个键：空/畸形头不该长出一个 nil 语义的字段
	if retry_after_raw ~= nil then out.retry_after_ms = retry_after_raw * 1000 end
	return out
end

-- ---- 池化会话：agent 每轮模型调用的默认通道（2026-09-21 第三轮审计 AG-20）----
-- 症状：M.post 原实现**每次请求都新建 curl 句柄、返回前销毁**，于是
-- `M.session` 存在的理由（「保留连接与 TLS 会话，省掉每轮重新握手」，
-- 见上面的注释）在任何生产路径上都没生效——两个内置驱动都走 M.post。
-- 后果有两层：① 每轮模型调用重做 DNS/TCP/TLS 握手；② 历史里那张 base64
-- 图**每一轮都重新编码、重新上传**（内存放大之外还白花上行带宽）。
-- 现在整颗驱动共用一份池化会话（驱动持 lua_State 到 destroy，生命周期天然对齐）。
local pooled = nil

local function pooled_session()
	if pooled == nil or pooled.handle == nil then
		local s, err = M.session()
		if s == nil then return nil, err end
		pooled = s
	end
	return pooled
end

-- 本 lua_State 已创建的会话总数（只读；判据见上面 session_seq 的注释）
function M.session_count()
	return session_seq
end

-- 池化会话的句柄（cdata 指针）。仅供离线单测断言「同一个句柄被复用」——
-- 句柄是裸指针，测试若不比对它，就只能靠数 TCP 连接数这种笨办法。
-- 注意：它会**惰性建池**（探针语义），因此不能用它判断「池是否为空」。
function M.pooled_handle()
	if pooled_session() == nil then return nil end
	return pooled.handle
end

-- 池里当前是否有一个活着的会话。**不建池**，供「关闭后必须为空」这类断言使用。
function M.pooled_active()
	return pooled ~= nil and pooled.handle ~= nil
end

-- 关闭池化会话（驱动 destroy 期或测试收尾用）。幂等。
function M.close_pooled()
	if pooled ~= nil then
		pooled:close()
		pooled = nil
	end
end

-- 发一次请求，**复用**池化会话（连接与 TLS 会话跨轮保留）。
-- 名称与语义说明：它不再是「一次性请求」——每次新建句柄的旧语义正是 AG-20。
function M.post(opts)
	local session, err = pooled_session()
	if session == nil then return { ok = false, err = err } end
	-- 见证行（BOT4CJ_HTTP_PROBE=1 才打）：agent E2E 据此断言「多轮请求复用同一份
	-- 会话」——计数在真报文链路上做决定性判据（句柄地址会被 malloc 复用，
	-- 见 session_seq 的注释）。默认不打，避免给正常运行加噪声。
	if os.getenv("BOT4CJ_HTTP_PROBE") == "1" then
		io.stderr:write(string.format("[http-probe] sessions=%d handle=%s\n",
			session_seq, tostring(session.handle)))
		io.stderr:flush()
	end
	return session:post(opts)
end

_G.__BOT4CJ_LLM_HTTP = M
return M
