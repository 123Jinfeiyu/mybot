#include "daemonvar.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <thread>

#include "util.hpp"

namespace bot {

/* N-7：abandoned 守护线程进程级计数——join 超时弃管的线程在进程退出时
 * 被 detach 硬杀，其私有 lua_State 泄漏在 exit 路径上无法观察（「宁漏不崩」
 * 已声明，但缺运行时可见性）。计数供 main 退出前统一告警。 */
static std::atomic<int> g_abandonedDaemons{0};

int abandonedDaemonCount() { return g_abandonedDaemons.load(); }

/* L4：共享槽布局硬编码 mutex 预留 64 字节（daemonvar.hpp），超出即越界写 count——
 * glibc aarch64=48B / x86_64=40B 远小于 64，但异种 libc 应在编译期就失败。 */
static_assert(sizeof(pthread_mutex_t) <= 64,
	"DaemonSlot reserves 64 bytes for pthread_mutex_t; this libc's pthread_mutex_t is larger");

static pthread_mutex_t* mutexOf(DaemonSlot* slot) {
	return reinterpret_cast<pthread_mutex_t*>(slot->mutex);
}

void daemon_request_stop(DaemonSlot* slot) {
	pthread_mutex_lock(mutexOf(slot));
	slot->stop = 1;
	pthread_mutex_unlock(mutexOf(slot));
}

bool daemon_stop_requested(DaemonSlot* slot) {
	pthread_mutex_lock(mutexOf(slot));
	std::uint8_t s = slot->stop;
	pthread_mutex_unlock(mutexOf(slot));
	return s != 0;
}

/* ---- 共享槽释放（2.4）：作为 shared_ptr 删除器，由「最后一个持有者」
 * 调用——引用计数保证此刻无任何线程仍在临界区内。 ---- */
static void releaseSlot(DaemonSlot* slot) {
	pthread_mutex_destroy(mutexOf(slot));
	std::free(slot);
}

/* ---- DaemonRunner RAII 兜底（见头文件注释）：只原子交出自己的引用，
 * 不触碰槽本身——未 spawn 时本成员是唯一所有者，交出即释放；已 spawn 时
 * 线程副本兜底到最后，既不悬垂也不泄漏。 ---- */
DaemonRunner::~DaemonRunner() {
	std::atomic_store(&slot_, std::shared_ptr<DaemonSlot>());
}

/* ---- 守护线程体：建私有 lua_State，注入槽地址 + 配置，setup 一次 + 逐拍 tick ----
 * 线程持 runner 的 shared_ptr 保生命周期；入口另取槽的强引用副本——
 * 整个 tick 循环期间槽由副本保证存活，弃管（主线程交出引用）后本副本
 * 就是最后所有者，返回时触发删除器。2.4：旧协议的退出检查点 + exchange
 * 唯一胜者仲裁（D2/审计 #2）在引用计数模型下不需要。 */
int64_t DaemonRunner::body(const std::string& chunk, const std::string& config, int64_t intervalMs) {
	std::shared_ptr<DaemonSlot> slot = std::atomic_load(&slot_);
	if (slot == nullptr) { return -1; }	/* 弃管早于本线程首次取槽的交错：槽已移交，无事可做 */
	lua_State* L = openSandboxedState();
	if (L == nullptr) { return -1; }

	/* 注入共享槽地址（64 位整数 tag，任意值逐位保真）与配置串为 Lua 全局 */
	lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(slot.get())));
	lua_setfield(L, LUA_GLOBALSINDEX, "DAEMON_SLOT_ADDR");
	lua_pushstring(L, config.c_str());
	lua_setfield(L, LUA_GLOBALSINDEX, "DAEMON_CONFIG");

	/* 2.7：注入宿主侧槽布局常量——pthread.lua 的跨语言布局断言以注入值
	 * 为基准（单一事实源 = daemonvar.hpp 的 struct + 静态断言）。此前 Lua
	 * 侧手抄一份基准值：C++ struct 改动而 Lua 未跟时，两侧各自自洽、
	 * 漂移不可见；注入后 cdef 与宿主事实不符即在守护启动时 fail-fast。 */
	lua_createtable(L, 0, 7);
	auto setOff = [&L](const char* k, int32_t v) {
		lua_pushinteger(L, v);
		lua_setfield(L, -2, k);
	};
	setOff("count", SLOT_COUNT_OFF);
	setOff("nvals", SLOT_NVALS_OFF);
	setOff("values", SLOT_VALUES_OFF);
	setOff("stop", SLOT_STOP_OFF);
	setOff("status", SLOT_STATUS_OFF);
	setOff("size", SLOT_SIZE);
	setOff("maxvals", SLOT_MAXVALS);
	lua_setfield(L, LUA_GLOBALSINDEX, "DAEMON_SLOT_LAYOUT");

	/* 跑 chunk 一次完成 setup 并定义全局 daemon_tick */
	int st = luaL_loadfile(L, chunk.c_str());
	if (st != LUA_OK) {
		std::cerr << "[daemon] chunk load error: " << luaErrorString(L) << std::endl;
		lua_close(L);
		return -1;
	}
	int setupStatus = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (setupStatus != LUA_OK) {
		std::cerr << "[daemon] chunk setup error: " << luaErrorString(L) << std::endl;
		lua_close(L);
		return -1;
	}
	lua_settop(L, 0);

	/* 逐拍循环：每拍 getglobal(daemon_tick)+pcall 采样一次；拍间 std::thread sleep。
	 * C-L2：错误限频 + 指数退避——tick 持续抛错（如 daemon_tick 未定义/加载
	 * 失败）原实现按拍频刷 stderr（缺省 5ms ≈ 200 行/秒）且永不退避。前 3 次
	 * 逐条打印定位问题，之后每 100 次一条摘要；拍间 sleep 按 2^n 退避
	 * （上限 ×256，避免长 interval 停机阻塞被进一步放大），成功一拍即清零。 */
	int64_t tickErrors = 0;
	while (!daemon_stop_requested(slot.get())) {
		lua_getglobal(L, "daemon_tick");
		int st2 = lua_pcall(L, 0, LUA_MULTRET, 0);
		if (st2 != LUA_OK) {
			++tickErrors;
			if (tickErrors <= 3 || tickErrors % 100 == 0) {
				std::cerr << "[daemon] tick error #" << tickErrors << ": "
					<< luaErrorString(L) << std::endl;
			}
		} else {
			tickErrors = 0;
		}
		lua_settop(L, 0);
		int64_t sleepMs = intervalMs;
		if (tickErrors > 0) {
			sleepMs = intervalMs * (int64_t(1) << std::min<int64_t>(tickErrors, 8));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
	}

	/* H-07：stop 后、lua_close 前调用 chunk 定义的 daemon_cleanup()（若存在） */
	lua_getglobal(L, "daemon_cleanup");
	if (lua_type(L, -1) == LUA_TFUNCTION) {
		int st3 = lua_pcall(L, 0, LUA_MULTRET, 0);
		if (st3 != LUA_OK) {
			std::cerr << "[daemon] cleanup error: " << luaErrorString(L) << std::endl;
		}
	}
	lua_settop(L, 0);

	/* F-01：count 与 stop 同规——都在槽内 mutex 临界区读取（此前锁外裸读，
	 * 与 daemonvar.hpp 的 stop 纪律不对称） */
	int64_t n;
	pthread_mutex_lock(mutexOf(slot.get()));
	n = slot->count;
	pthread_mutex_unlock(mutexOf(slot.get()));
	lua_close(L);
	/* 2.4：返回即交还本线程的引用副本。弃管路径下这里是最后所有者，
	 * 删除器（mutex_destroy + free）在本线程执行——与旧协议「线程自行
	 * 释放」语义一致，仲裁交给引用计数。 */
	return n;
}

/* ---- start：spawn 守护线程 ---- */
void DaemonRunner::start(const std::string& name, const std::string& chunk,
		const std::string& config, int64_t intervalMs) {
	if (std::atomic_load(&slot_) == nullptr) {
		throw std::runtime_error("Daemon '" + name + "' slot already released, cannot spawn");
	}
	/* self_ 是 weak_ptr（打破自引用环防泄漏）：此处取回强引用交给线程保生命周期。
	 * build 期 registry 必持有强引用，lock() 不会失败；万一取空则显式报错，
	 * 不把空 shared_ptr 交给线程去解引用。 */
	std::shared_ptr<DaemonRunner> keep = self_.lock();
	if (!keep) {
		throw std::runtime_error("Daemon '" + name + "' runner was released before spawn");
	}
	/* D1 + 审计：spawned_ 必须在守护线程可能开始执行**之前**置位。
	 * std::thread 构造即已启动线程，故不能只把 store 挪到 detach() 之前——
	 * 那仍留着「线程已进入 body（读写槽）而 spawned_ 仍为 false」的窗口：
	 * 此刻并发的 waitForExit 读到 false，会走「从未 spawn」分支直接
	 * releaseSlot，线程随后触碰已释放内存（use-after-free）。
	 * 置位前移到线程构造之前；构造抛异常则回滚，保持「未 spawn →
	 * promise 永不 set → 停机直接释放槽」的既有 D1 语义。 */
	spawned_.store(true, std::memory_order_release);
	try {
		std::thread([chunk, config, intervalMs, keep]() {
			int64_t r = keep->body(chunk, config, intervalMs);
			try { keep->result_->set_value(r); } catch (...) { /* future 已被消费/进程退出 */ }
		}).detach();
	} catch (...) {
		spawned_.store(false, std::memory_order_release);
		throw;
	}
	std::cerr << "Daemon '" << name << "' spawned, config: " << config << std::endl;
}

/* ---- requestStop：置停止标志（槽已交出/已释放则无事可做）----
 * 审计 H-3 防线在 2.4 模型下的形态：弃管即成员置空，本地强引用副本
 * 保证检查与上锁之间槽不可能被释放。 */
void DaemonRunner::requestStop() {
	std::shared_ptr<DaemonSlot> slot = std::atomic_load(&slot_);
	if (slot == nullptr) { return; }
	daemon_request_stop(slot.get());
}

/* ---- waitForExit：置停止 + 带超时等待退出 + 交出引用 ----
 * D1：未 spawn（build 中途失败）的守护 promise 永不 set——跳过等待直接释放槽。
 * 2.4：弃管 = 主线程原子交出自己的引用。守护线程的强引用副本保证槽存活到
 * 其退出，引用计数在最后持有者退场时经删除器释放——旧协议的退出检查点 +
 * exchange 唯一胜者裁决（审计 #2：双方互错过则槽 + mutex 永久泄漏）在
 * 计数模型下不需要：任何交错都恰好释放一次。主线程此后 load 恒为 null，
 * requestStop/readSample 的「不得触碰已弃管槽」由判空兜底。 */
void DaemonRunner::waitForExit(const std::string& name) {
	std::shared_ptr<DaemonSlot> slot = std::atomic_load(&slot_);
	/* 已释放（重复销毁）或已弃管（重复等待）——无事可做 */
	if (slot == nullptr) { return; }
	daemon_request_stop(slot.get());
	if (!spawned_.load()) {
		std::cerr << "Daemon '" << name << "' was never spawned, releasing slot directly" << std::endl;
		std::atomic_store(&slot_, std::shared_ptr<DaemonSlot>());
		return;	/* 线程从未存在，本地副本是唯一剩余所有者，出作用域即释放 */
	}
	if (fut_.valid()) {
		std::future_status st = fut_.wait_for(std::chrono::milliseconds(joinTimeoutMs_));
		if (st == std::future_status::ready) {
			int64_t n = fut_.get();
			std::cerr << "Daemon '" << name << "' joined, total publishes=" << n << std::endl;
			std::atomic_store(&slot_, std::shared_ptr<DaemonSlot>());
			return;	/* 线程已退场并交出副本，本地副本是最后所有者 */
		}
		g_abandonedDaemons.fetch_add(1);	/* N-7：进程退出前统一告警 */
		std::atomic_store(&slot_, std::shared_ptr<DaemonSlot>());
		std::cerr << "Daemon '" << name << "' not exited within " << joinTimeoutMs_
			<< "ms, abandoning slot (thread will release it on exit)" << std::endl;
		return;
	}
	std::atomic_store(&slot_, std::shared_ptr<DaemonSlot>());
}

/* ---- readSample：主线程加锁读采样值 → JSON（v0..vN 标量键 + values 数组）---- */
Json DaemonRunner::readSample() {
	/* 2.4：加载本地强引用副本——本次读取期间槽由副本保证存活。原「play/
	 * readSample 与 destroy 同线程顺序」契约（审计中-2）下的 TOCTOU 缺口
	 * （load 到槽之后 destroy 才置空）升格为代码保证：即便 destroy 并发
	 * 置空成员，本副本仍持引用，不存在悬垂解引用。 */
	std::shared_ptr<DaemonSlot> slot = std::atomic_load(&slot_);
	if (slot == nullptr) {
		return makeErrResult("daemon slot already released");
	}
	Json ret = JsonObject();
	Json arr = JsonArray();
	pthread_mutex_lock(mutexOf(slot.get()));
	int32_t nvals = slot->nvals;
	int32_t count = slot->count;
	int32_t slotStatus = slot->status;
	int32_t nv = nvals;
	if (nv > SLOT_MAXVALS) { nv = SLOT_MAXVALS; }
	for (int32_t k = 0; k < nv; ++k) {
		double v = slot->values[k];
		arr.add(JsonFloat(v));
		ret.put("v" + std::to_string(k), JsonFloat(v));
	}
	pthread_mutex_unlock(mutexOf(slot.get()));
	/* N-27：守护 setup 失败绝不伪装成采样值。旧实现把失败码（-2/-8/-9…）发进
	 * v0，下游 $ 引用无从甄别——配置错误会被当作温度做闭环，只有 count 停滞
	 * 这种间接迹象。现在 status ≠ 0 直接抛错：这是**永久性**失败（守护不会
	 * 自愈），必须在动作链上响亮中止，而不是让消费方报告「某字段缺失」这类
	 * 间接症状（Monitor 只会说 no daemon value (v0 missing)）。
	 * 与「尚未出首帧」区分：那是瞬时状态，由消费方（Monitor/Ema/Kalman）
	 * 自己按 ERR 契约表达，不在此抛。 */
	if (slotStatus != 0) {
		throw std::runtime_error("daemon setup failed (slot status "
			+ std::to_string(slotStatus) + "); no samples are published"
			+ " — the failure code and reason are in the daemon log");
	}
	ret.put("status", JsonString("OK"));
	ret.put("count", JsonInt(count));
	ret.put("nvals", JsonInt(nvals));
	ret.put("values", std::move(arr));
	logD([count, nvals] { return "Daemon play: count=" + std::to_string(count) + " nvals=" + std::to_string(nvals); });
	return ret;
}

/* ---- issue #19：多模块路径按序查找第一个存在的文件 ----
 * 全部不存在返回最后一个路径拼接（保持原有"文件不存在"加载报错语义）。
 * N-19：':' 分段/去空/剥尾斜杠统一走 util 的 splitModulePaths。 */
std::string firstExistingModule(const std::string& paths, const std::string& rel) {
	std::string last = paths + "/" + rel;
	for (const std::string& p : splitModulePaths(paths)) {
		std::string cand = p + "/" + rel;
		std::ifstream f(cand, std::ios::binary);
		if (f.good()) { return cand; }
		last = cand;
	}
	return last;
}

std::string resolveDaemonChunk(const std::string& chunk, const std::string& modulePath) {
	if (chunk.rfind("/", 0) == 0) { return chunk; }
	if (chunk.rfind(modulePath + "/", 0) == 0) { return chunk; }
	/* 兼容不带 "./" 的旧写法：缺省 modulePath="./modules" 时 "modules/..." 原样 */
	std::string noDot = modulePath;
	if (modulePath.rfind("./", 0) == 0) { noDot = modulePath.substr(2); }
	if (chunk.rfind(noDot + "/", 0) == 0) { return chunk; }
	return firstExistingModule(modulePath, chunk);
}

/* ---- interval 解析与校验（≤0 会空转占满 CPU，显式报错）----
 * C9：解析经 util 的 parseLongStrict（NOT_INT/OVERFLOW 分型），并钳制合理上限——
 * join 超时按 interval*5 计算，钳制后乘法不溢出。 */
static int64_t parseInterval(const std::string& iv, const std::string& name,
		const std::string& tabName, int64_t row) {
	auto parsed_ = parseLongStrict(iv);
	if (parsed_.first == IntParse::NOT_INT) {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'interval' value '" + iv +
			"' is not a valid integer (in table '" + tabName + "', row " + std::to_string(row) + ").");
	}
	if (parsed_.first == IntParse::OVERFLOW || parsed_.second < 0) {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'interval' value '" + iv +
			"' is out of range (in table '" + tabName + "', row " + std::to_string(row) + ").");
	}
	long long parsed = parsed_.second;
	if (parsed == 0) {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'interval' must be > 0 ms, got " + std::to_string(parsed) +
			" (in table '" + tabName + "', row " + std::to_string(row) +
			"). interval <= 0 causes a busy-loop that pins a CPU core.");
	}
	constexpr int64_t kMaxIntervalMs = 3600000;	/* 1 小时：合理上限，保证 joinTimeout=interval*5 不溢出 */
	if (parsed > kMaxIntervalMs) {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'interval' " + std::to_string(parsed) +
			"ms exceeds the maximum " + std::to_string(kMaxIntervalMs) +
			"ms (in table '" + tabName + "', row " + std::to_string(row) + ").");
	}
	return parsed;
}

/* ---- DAEMON_CONFIG schema 校验（E2 / N-27 尾）----
 * DAEMON_CONFIG 最终由 chunk 解释，但**非法值必须在结构脚本加载期就报错**：
 * 等到守护线程 setup 才失败，调用方只拿得到「守护启动失败 + status 码」，
 * 不知道是哪张表、哪一行、哪个键；而拼错的 proto 此前会静默按 i2c 走、
 * 非法的 fields 模式会静默当浮点透传——都是「跑起来了但结果不对」的隐蔽故障。
 * 只校验框架列（proto/fields/sign，语义见 docs/daemon.md）；其余任意键是驱动
 * 的 build 参数（channel/pga/rshunt/osr…），框架不得拦截。
 * 语法以 modules/lib/sensor_daemon.lua 的解析为准，两处规则改动须同步。
 * 矩阵空占位符 "-" 与空串一律视作「未提供」（与 interval 校验的 `v != "-"` 同）。 */
static bool daemonConfigMissing(const std::string& v) { return v.empty() || v == "-"; }

/* proto 白名单：i2c（缺省）/ spi / uart / virt（virt=无硬件仿真）。
 * 未知值旧实现静默落进 i2c 分支——拼错 "i2" 会真去开 I2C 总线。 */
static void checkDaemonProto(const std::string& v, const std::string& name,
		const std::string& tabName, int64_t row) {
	if (v == "i2c" || v == "spi" || v == "uart" || v == "virt") { return; }
	throw IllegalArgumentException(
		"Daemon '" + name + "': column 'proto' value '" + v +
		"' is not a supported bus (expected i2c|spi|uart|virt) (in table '" + tabName +
		"', row " + std::to_string(row) + ").");
}

/* sign：符号扩展位宽（16 = 16 位补码）。非数值此前被 tonumber 折成 nil，
 * 于是 sign=16x 这类拼错会让符号扩展静默失效（读数全是无符号大数）。 */
static void checkDaemonSign(const std::string& v, const std::string& name,
		const std::string& tabName, int64_t row) {
	if (parseLongStrict(v).first == IntParse::OK) { return; }
	throw IllegalArgumentException(
		"Daemon '" + name + "': column 'sign' value '" + v +
		"' is not a valid integer (expected a bit width such as 16) (in table '" + tabName +
		"', row " + std::to_string(row) + ").");
}

static bool daemonFieldModeChar(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* fields 语法：<名>[:<模式>]，分隔符 ',' 或 '|'，模式 ∈ 缺省（跟随全局 sign）/
 * f / float / s16。空项（重复或首尾分隔符）与未知模式都在此报错——旧实现把
 * 空项 print 后丢弃、把未知模式当浮点透传（语义被悄悄改写，且只落在守护日志里）。 */
static void checkDaemonFields(const std::string& v, const std::string& name,
		const std::string& tabName, int64_t row) {
	auto bad = [&](const std::string& why, const std::string& tok) {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'fields' " + why + ": '" + tok + "' (in table '" +
			tabName + "', row " + std::to_string(row) + ").");
	};
	std::size_t pos = 0;
	while (pos <= v.size()) {
		std::size_t sep = v.find_first_of(",|", pos);
		std::string tok = v.substr(pos, (sep == std::string::npos ? v.size() : sep) - pos);
		if (tok.empty()) {
			bad("has an empty field name (check for doubled or leading/trailing separators)", v);
		}
		std::size_t colon = tok.find(':');
		std::string fname = (colon == std::string::npos) ? tok : tok.substr(0, colon);
		std::string mode = (colon == std::string::npos) ? std::string() : tok.substr(colon + 1);
		if (fname.empty()) { bad("has an empty field name", tok); }
		for (char c : mode) {
			if (!daemonFieldModeChar(c)) { bad("has a malformed field mode", tok); }
		}
		if (!(mode.empty() || mode == "f" || mode == "float" || mode == "s16")) {
			bad("has an unknown field mode ':" + mode + "' (expected :f, :float or :s16)", tok);
		}
		if (sep == std::string::npos) { break; }
		pos = sep + 1;
	}
}

/* ---- 守护硬件变量工厂 ---- */
std::unique_ptr<MtVars_Daemon> MtVars_Daemon::Factory(const mt::MatrixObject& defs, int64_t i,
		std::vector<std::shared_ptr<DaemonRunner>>& registry) {
	int64_t idxName = defs.requireCol(0, "name", "Table");
	int64_t idxChunk = defs.requireCol(0, "chunk", "Table");
	std::string name = defs(i, idxName);
	std::string chunkRaw = defs.inherited(i, idxChunk);

	/* N-19：模块根目录统一走 util::modulePaths（空串也归一为缺省） */
	std::string modulePath = modulePaths();
	std::string chunkPath = resolveDaemonChunk(chunkRaw, modulePath);
	/* N-12：装配期进度走 logD（spawn/join 是运行期生命周期事件，保留 stderr） */
	logD([&] { return "Create a Daemon hard-Var: " + name + ", chunk: " + chunkPath; });

	/* 单遍列扫描：name/chunk 之外收集为配置串 "k=v;k=v"，提取 interval（缺省 5ms），
	 * 并对框架列做加载期 schema 校验（E2：非法 proto/fields/sign 在此早报） */
	std::string cfgAcc;
	int64_t ivAcc = 5;
	for (int64_t j = 0; j < static_cast<int64_t>(defs.cols()); ++j) {
		const std::string& colName = defs(0, j);
		if (colName == "name" || colName == "chunk") { continue; }
		std::string v = defs.inherited(i, j);
		cfgAcc += colName + "=" + v + ";";
		if (colName == "interval" && v != "-") {
			ivAcc = parseInterval(v, name, defs.getName(), i);
		} else if (colName == "proto" && !daemonConfigMissing(v)) {
			checkDaemonProto(v, name, defs.getName(), i);
		} else if (colName == "fields" && !daemonConfigMissing(v)) {
			checkDaemonFields(v, name, defs.getName(), i);
		} else if (colName == "sign" && !daemonConfigMissing(v)) {
			checkDaemonSign(v, name, defs.getName(), i);
		}
	}
	std::string configStr = cfgAcc;
	int64_t intervalMs = ivAcc;

	/* join 超时：interval 5 倍并保底 2000ms */
	int64_t joinTimeoutMs = (intervalMs * 5 > 2000) ? intervalMs * 5 : 2000;

	/* 分配并清零共享槽，初始化 mutex（malloc 失败立即抛错拒绝空指针解引用）。
	 * 2.4：槽所有权入 shared_ptr，删除器 = releaseSlot（mutex_destroy + free）——
	 * 最后一个持有者（主线程 / 守护线程）退场时释放。 */
	DaemonSlot* raw = static_cast<DaemonSlot*>(std::calloc(1, SLOT_SIZE));
	if (raw == nullptr) {
		throw std::runtime_error("Daemon '" + name + "' failed to allocate shared slot ("
			+ std::to_string(SLOT_SIZE) + " bytes)");
	}
	if (pthread_mutex_init(mutexOf(raw), nullptr) != 0) {
		std::free(raw);
		throw std::runtime_error("Daemon '" + name + "' failed to initialize slot mutex");
	}

	auto runner = std::make_shared<DaemonRunner>(
		std::shared_ptr<DaemonSlot>(raw, releaseSlot), joinTimeoutMs);
	runner->bindSelf(runner);	/* 线程捕获 self 保生命周期 */
	registry.push_back(runner);

	return std::unique_ptr<MtVars_Daemon>(new MtVars_Daemon(
		std::move(runner), name, chunkPath, configStr, intervalMs));
}

/* ---- 守护硬件变量：play/build/destroy 直通守护事件（R-3）---- */
Json MtVars_Daemon::play(const Json&) {
	return runner_->readSample();
}

void MtVars_Daemon::build() {
	runner_->start(name_, chunkPath_, configStr_, intervalMs_);
}

void MtVars_Daemon::destroy() {
	runner_->waitForExit(name_);
}

} // namespace bot