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

/* ---- 共享槽释放：销毁 mutex 并释放（由加锁线程独占调用）---- */
static void releaseSlot(DaemonSlot* slot) {
	pthread_mutex_destroy(mutexOf(slot));
	std::free(slot);
}

/* ---- DaemonRunner RAII 兜底（见头文件注释）：仅回收线程尚未接管所有权（未 spawn）的槽 ---- */
DaemonRunner::~DaemonRunner() {
	DaemonSlot* s = slot_.load();
	if (s != nullptr && !spawned_.load()) {
		releaseSlot(s);
		slot_.store(nullptr);
	}
}

/* ---- D2 收尾唯一出口（C-D3，详见 daemonvar.hpp）---- */
int64_t DaemonRunner::exitClaim(int64_t n) {
	if (!abandoned_.load()) {
		exitCheckDone_.store(true);
		if (!abandoned_.load()) { return n; }
	}
	/* 弃管路径：槽所有权归本线程。以 exchange 认领——若主线程超时后已先行
	 * 认领，此处拿到 nullptr，零接触返回；唯一胜者释放，无双重释放。 */
	DaemonSlot* mine = slot_.exchange(nullptr);
	if (mine != nullptr) { releaseSlot(mine); }
	return n;
}

/* ---- 守护线程体：建私有 lua_State，注入槽地址 + 配置，setup 一次 + 逐拍 tick ----
 * 线程持 runner 的 shared_ptr 保生命周期；退出时若 abandoned 置位（主线程已放弃等待），
 * 槽所有权归本线程——以 exchange 认领后释放（D2）。主线程超时置位可能落在
 * 本线程首次检查之后（审计 #2），故先公告退出检查点再复查，双方以 exchange
 * 的唯一胜者裁决释放，竞态窗口归零。 */
int64_t DaemonRunner::body(const std::string& chunk, const std::string& config, int64_t intervalMs) {
	DaemonSlot* slot = slot_.load();
	lua_State* L = luaL_newstate();
	if (L == nullptr) { return exitClaim(-1); }
	luaL_openlibs(L);
	sandbox_lua(L);

	/* 注入共享槽地址（64 位整数 tag，任意值逐位保真）与配置串为 Lua 全局 */
	lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(slot)));
	lua_setfield(L, LUA_GLOBALSINDEX, "DAEMON_SLOT_ADDR");
	lua_pushstring(L, config.c_str());
	lua_setfield(L, LUA_GLOBALSINDEX, "DAEMON_CONFIG");

	/* 跑 chunk 一次完成 setup 并定义全局 daemon_tick */
	int st = luaL_loadfile(L, chunk.c_str());
	if (st != LUA_OK) {
		std::cerr << "[daemon] chunk load error: " << luaErrorString(L) << std::endl;
		lua_close(L);
		return exitClaim(-1);
	}
	int setupStatus = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (setupStatus != LUA_OK) {
		std::cerr << "[daemon] chunk setup error: " << luaErrorString(L) << std::endl;
		lua_close(L);
		return exitClaim(-1);
	}
	lua_settop(L, 0);

	/* 逐拍循环：每拍 getglobal(daemon_tick)+pcall 采样一次；拍间 std::thread sleep。
	 * C-L2：错误限频 + 指数退避——tick 持续抛错（如 daemon_tick 未定义/加载
	 * 失败）原实现按拍频刷 stderr（缺省 5ms ≈ 200 行/秒）且永不退避。前 3 次
	 * 逐条打印定位问题，之后每 100 次一条摘要；拍间 sleep 按 2^n 退避
	 * （上限 ×256，避免长 interval 停机阻塞被进一步放大），成功一拍即清零。 */
	int64_t tickErrors = 0;
	while (!daemon_stop_requested(slot)) {
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
	pthread_mutex_lock(mutexOf(slot));
	n = slot->count;
	pthread_mutex_unlock(mutexOf(slot));
	lua_close(L);
	/* D2 + 审计 #2：主线程超时弃管（置 abandoned）可能恰好落在本线程首次
	 * 检查之后——先公告退出检查已做（此后本线程对槽至多剩一次 exchange，
	 * 不再解引用），再复查一次；复查仍为 false 则槽留给主线程（joined 路径
	 * 或超时后的 exchange 认领）释放，本线程直接返回。C-D3：收尾统一走
	 * exitClaim（与早期失败路径同一出口）。 */
	return exitClaim(n);
}

/* ---- start：spawn 守护线程 ---- */
void DaemonRunner::start(const std::string& name, const std::string& chunk,
		const std::string& config, int64_t intervalMs) {
	if (slot_.load() == nullptr) {
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

/* ---- requestStop：置停止标志（槽已释放或已放弃所有权则无事可做）----
 * 审计 H-3 防线：abandon 路径下槽所有权已移交守护线程（waitForExit 只置
 * abandoned_ 不清 slot_），此后任何 requestStop 都可能触碰已被守护线程
 * pthread_mutex_destroy + free 的槽——与 readSample 同一判据，不得只判空。 */
void DaemonRunner::requestStop() {
	DaemonSlot* slot = slot_.load();
	if (slot == nullptr || abandoned_.load()) { return; }
	daemon_request_stop(slot);
}

/* ---- waitForExit：置停止 + 带超时等待退出 + 销毁 mutex/槽 ----
 * D1：未 spawn（build 中途失败）的守护 promise 永不 set——跳过等待直接释放槽。
 * D2：spawn 过但 join 超时，置 abandoned 把槽所有权移交守护线程自行释放，
 * 主线程此后不再触碰槽（readSample 对 slot==nullptr 返回 ERR 契约）。 */
void DaemonRunner::waitForExit(const std::string& name) {
	DaemonSlot* slot = slot_.load();
	/* 已释放（重复销毁）或已放弃所有权（重复等待）——无事可做，不得触碰槽 */
	if (slot == nullptr || abandoned_.load()) { return; }
	daemon_request_stop(slot);
	if (!spawned_.load()) {
		std::cerr << "Daemon '" << name << "' was never spawned, releasing slot directly" << std::endl;
		releaseSlot(slot);
		slot_.store(nullptr);
		return;
	}
	if (fut_.valid()) {
		std::future_status st = fut_.wait_for(std::chrono::milliseconds(joinTimeoutMs_));
		if (st == std::future_status::ready) {
			int64_t n = fut_.get();
			std::cerr << "Daemon '" << name << "' joined, total publishes=" << n << std::endl;
			releaseSlot(slot);
			slot_.store(nullptr);
			return;
		}
		abandoned_.store(true);
		g_abandonedDaemons.fetch_add(1);	/* N-7：进程退出前统一告警 */
		/* 审计 #2：置弃管后检查线程是否已过「退出检查点」。
		 * - 未过：线程离开 tick 循环后必在复查中看到 abandoned 并自行
		 *   exchange 认领释放——本方不得触碰槽（线程可能仍在拍内）。
		 * - 已过：线程的全部槽访问已结束（seq_cst 检查点保证可见），
		 *   且其弃管路径的 exchange 可能尚未执行——由本方 exchange 认领。
		 *   exchange 的唯一胜者释放：线程先认领则本方拿到 nullptr，
		 *   本方先认领则线程拿到 nullptr——旧实现此处直接 return，与
		 *   线程退出检查互错过，槽 + mutex 永久泄漏。 */
		if (exitCheckDone_.load()) {
			DaemonSlot* mine = slot_.exchange(nullptr);
			if (mine != nullptr) { releaseSlot(mine); }
		}
		std::cerr << "Daemon '" << name << "' not exited within " << joinTimeoutMs_
			<< "ms, abandoning slot (thread will release it on exit)" << std::endl;
		return;
	}
	releaseSlot(slot);
	slot_.store(nullptr);
}

/* ---- readSample：主线程加锁读采样值 → JSON（v0..vN 标量键 + values 数组）---- */
Json DaemonRunner::readSample() {
	/* D2：放弃路径下槽所有权已移交守护线程，主线程不再触碰——
	 * abandoned 置位或槽已置空时按 ERR 契约返回，不碰悬垂指针。
	 * F-01：槽指针原子读取，取回局部指针后再解引用。 */
	DaemonSlot* slot = slot_.load();
	if (slot == nullptr || abandoned_.load()) {
		return makeErrResult("daemon slot already released");
	}
	Json ret = JsonObject();
	Json arr = JsonArray();
	pthread_mutex_lock(mutexOf(slot));
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
	pthread_mutex_unlock(mutexOf(slot));
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

	/* 分配并清零共享槽，初始化 mutex（malloc 失败立即抛错拒绝空指针解引用） */
	DaemonSlot* slot = static_cast<DaemonSlot*>(std::calloc(1, SLOT_SIZE));
	if (slot == nullptr) {
		throw std::runtime_error("Daemon '" + name + "' failed to allocate shared slot ("
			+ std::to_string(SLOT_SIZE) + " bytes)");
	}
	if (pthread_mutex_init(mutexOf(slot), nullptr) != 0) {
		std::free(slot);
		throw std::runtime_error("Daemon '" + name + "' failed to initialize slot mutex");
	}

	auto runner = std::make_shared<DaemonRunner>(slot, joinTimeoutMs);
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