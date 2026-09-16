-- 共享槽布局跨语言一致性测试（纯 luajit，无需真机、无需 cc）
-- 用法（项目根目录）: luajit test/test_slot_layout.lua
--
-- 背景：@Daemon 的 IPC 槽布局由三处手写维护，任一处漂移即跨语言内存踩踏：
--   ① src/HV/Daemon.cj 的 SLOT_*_OFF / SLOT_SIZE / SLOT_MAXVALS（仓颉侧读写偏移）
--   ② modules/lib/pthread.lua 的 daemon_slot_t cdef（Lua 侧读写同一块内存）
--   ③ pthread.lua assert_layout() 内的基准表（64/68/72/328）
-- pthread.lua 自带的 assert_layout 只校验 ② vs ③（同一份 ffi 布局的自证），
-- ① 从未被机器核对过——改仓颉侧偏移而忘改 Lua 侧时，只有 37 个守护 E2E 间接兜着，
-- 且只有被读到的字段才会暴露。本测试以编译器实际填充出的布局（ffi.offsetof/sizeof）
-- 为唯一事实源，同时校验 ① 与 ③，把三方对齐变成一条显式断言。

local T = dofile("test/lib/tester.lua")
local check = T.check

local C_SRC = "src/HV/Daemon.cj"

-- 读文件全文（测试进程是普通 luajit，不受运行时沙箱 io 中和影响）
local function slurp(path)
	local f = io.open(path, "r")
	if not f then return nil end
	local s = f:read("*a")
	f:close()
	return s
end

-- ① 解析仓颉侧常量：`public let SLOT_XXX: Int64 = N`
local function parse_cj_slots(src)
	local out = {}
	for line in src:gmatch("[^\n]*") do
		local k, v = line:match("^public let (SLOT_[%w_]+): Int64 = (%d+)")
		if k then out[k] = tonumber(v) end
	end
	return out
end

local src = slurp(C_SRC)
check("可读取 " .. C_SRC, src ~= nil)
if src == nil then
	-- 前置不成立时不得静默通过：直接判失败退出
	print("[FAIL] 源文件缺失，无法核对槽布局")
	os.exit(1)
end

local cj = parse_cj_slots(src)
for _, k in ipairs({ "SLOT_COUNT_OFF", "SLOT_NVALS_OFF", "SLOT_VALUES_OFF",
	"SLOT_STOP_OFF", "SLOT_SIZE", "SLOT_MAXVALS" }) do
	check("Daemon.cj 声明了 " .. k, cj[k] ~= nil, "解析失败则本测试失去意义")
end

-- ②/③ 加载 pthread.lua（其 cdef 随之注册），取 ffi 布局为事实源
local P = dofile("modules/lib/pthread.lua")
local ffi = require("ffi")
if ffi.offsetof == nil then
	print("[FAIL] 本 LuaJIT 无 ffi.offsetof，无法核对布局（请升级或改由 C 侧校验）")
	os.exit(1)
end

local fields = {
	{ "SLOT_COUNT_OFF",  "count"  },
	{ "SLOT_NVALS_OFF",  "nvals"  },
	{ "SLOT_VALUES_OFF", "values" },
	{ "SLOT_STOP_OFF",   "stop"   },
}
for _, pair in ipairs(fields) do
	local want, got = cj[pair[1]], ffi.offsetof("daemon_slot_t", pair[2])
	check("仓颉 " .. pair[1] .. "=" .. tostring(want) .. " == ffi 实测 " .. pair[2] .. "=" .. tostring(got),
		want == got, want .. " vs " .. tostring(got))
end
check("仓颉 SLOT_SIZE=" .. tostring(cj.SLOT_SIZE) .. " == ffi.sizeof(daemon_slot_t)="
	.. tostring(ffi.sizeof("daemon_slot_t")), cj.SLOT_SIZE == ffi.sizeof("daemon_slot_t"))

-- values[] 通道数三处一致：仓颉常量、pthread.lua 导出、cdef 数组长度
local vals = math.floor((cj.SLOT_STOP_OFF - cj.SLOT_VALUES_OFF) / ffi.sizeof("double"))
check("SLOT_MAXVALS=" .. tostring(cj.SLOT_MAXVALS) .. " == values 区可容纳通道数=" .. tostring(vals),
	cj.SLOT_MAXVALS == vals)
check("pthread.lua 导出的 P.SLOT_MAXVALS 与仓颉一致",
	P.SLOT_MAXVALS == cj.SLOT_MAXVALS, tostring(P.SLOT_MAXVALS) .. " vs " .. tostring(cj.SLOT_MAXVALS))

-- ③ pthread.lua assert_layout 的基准表不得与仓颉脱节：其内部若通过，说明 ②==③；
--    上面已证 ①==②，故 ①==③。这里直接跑一次它，确保其本身未坏。
local ok, err = pcall(P.assert_layout)
check("pthread.lua assert_layout() 自身通过", ok, tostring(err))

-- stop 标志必须在 values 区之后（守护 publish 写 values、宿主读 stop，重叠即踩踏）
check("stop 偏移 > values 偏移 + 通道数*8",
	cj.SLOT_STOP_OFF >= cj.SLOT_VALUES_OFF + cj.SLOT_MAXVALS * ffi.sizeof("double"),
	string.format("stop=%s values=%s maxvals=%s", cj.SLOT_STOP_OFF, cj.SLOT_VALUES_OFF, cj.SLOT_MAXVALS))
check("槽大小足以容纳 stop 字节", cj.SLOT_SIZE > cj.SLOT_STOP_OFF)

-- 真实 mutex 尺寸无法在 ffi 命名空间内 sizeof（HAL 只声明了截断别名），
-- 该项由 test_abi_consts.lua 在 cc 可用时校验「预留 64B ≥ 实际 pthread_mutex_t」。
T.summary("slot_layout")
