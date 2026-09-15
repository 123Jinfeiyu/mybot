#include "daemonvar.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <thread>

#include "util.hpp"

namespace bot {

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

/* ---- 守护线程体：建私有 lua_State，注入槽地址 + 配置，setup 一次 + 逐拍 tick ---- */
int64_t daemon_body(DaemonSlot* slot, const std::string& chunk,
		const std::string& config, int64_t interval_ms) {
	lua_State* L = luaL_newstate();
	if (L == nullptr) { return -1; }
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
		std::cout << "[daemon] chunk load error: " << luaErrorString(L) << std::endl;
		lua_close(L);
		return -1;
	}
	int setupStatus = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (setupStatus != LUA_OK) {
		std::cout << "[daemon] chunk setup error: " << luaErrorString(L) << std::endl;
		lua_close(L);
		return -1;
	}
	lua_settop(L, 0);

	/* 逐拍循环：每拍 getglobal(daemon_tick)+pcall 采样一次；拍间 std::thread sleep */
	while (!daemon_stop_requested(slot)) {
		lua_getglobal(L, "daemon_tick");
		int st2 = lua_pcall(L, 0, LUA_MULTRET, 0);
		if (st2 != LUA_OK) {
			std::cout << "[daemon] tick error: " << luaErrorString(L) << std::endl;
		}
		lua_settop(L, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
	}

	/* H-07：stop 后、lua_close 前调用 chunk 定义的 daemon_cleanup()（若存在） */
	lua_getglobal(L, "daemon_cleanup");
	if (lua_type(L, -1) == LUA_TFUNCTION) {
		int st3 = lua_pcall(L, 0, LUA_MULTRET, 0);
		if (st3 != LUA_OK) {
			std::cout << "[daemon] cleanup error: " << luaErrorString(L) << std::endl;
		}
	}
	lua_settop(L, 0);

	int64_t n = slot->count;
	lua_close(L);
	return n;
}

/* ---- issue #19：多模块路径按序查找第一个存在的文件 ----
 * 全部不存在返回最后一个路径拼接（保持原有"文件不存在"加载报错语义）。 */
std::string firstExistingModule(const std::string& paths, const std::string& rel) {
	std::string last = paths + "/" + rel;
	std::size_t b = 0;
	while (b <= paths.size()) {
		std::size_t e = paths.find(':', b);
		std::string p = (e == std::string::npos) ? paths.substr(b) : paths.substr(b, e - b);
		if (e == std::string::npos) {
			b = paths.size() + 1;
		} else {
			b = e + 1;
		}
		if (p.empty()) { continue; }
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

/* ---- play 事件：主线程加锁读采样值 → JSON（v0..vN 标量键 + values 数组）---- */
static Json daemonPlay(DaemonHolder* holder) {
	Json ret = JsonObject();
	Json arr = JsonArray();
	DaemonSlot* slot = holder->slot;
	pthread_mutex_lock(mutexOf(slot));
	int32_t nvals = slot->nvals;
	int32_t count = slot->count;
	int32_t nv = nvals;
	if (nv > SLOT_MAXVALS) { nv = SLOT_MAXVALS; }
	for (int32_t k = 0; k < nv; ++k) {
		double v = slot->values[k];
		arr.add(JsonFloat(v));
		ret.put("v" + std::to_string(k), JsonFloat(v));
	}
	pthread_mutex_unlock(mutexOf(slot));
	ret.put("status", JsonString("OK"));
	ret.put("count", JsonInt(count));
	ret.put("nvals", JsonInt(nvals));
	ret.put("values", std::move(arr));
	logD([count, nvals] { return "Daemon play: count=" + std::to_string(count) + " nvals=" + std::to_string(nvals); });
	return ret;
}

/* ---- interval 解析与校验（≤0 会空转占满 CPU，显式报错）---- */
static int64_t parseInterval(const std::string& iv, const std::string& name,
		const std::string& tabName, int64_t row) {
	char* end = nullptr;
	long long parsed = std::strtoll(iv.c_str(), &end, 10);
	if (end == iv.c_str() || *end != '\0') {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'interval' value '" + iv +
			"' is not a valid integer (in table '" + tabName + "', row " + std::to_string(row) + ").");
	}
	if (parsed <= 0) {
		throw IllegalArgumentException(
			"Daemon '" + name + "': column 'interval' must be > 0 ms, got " + std::to_string(parsed) +
			" (in table '" + tabName + "', row " + std::to_string(row) +
			"). interval <= 0 causes a busy-loop that pins a CPU core.");
	}
	return parsed;
}

/* ---- 共享槽释放：销毁 mutex 并释放（由加锁线程独占调用）---- */
static void releaseSlot(DaemonSlot* slot) {
	pthread_mutex_destroy(mutexOf(slot));
	std::free(slot);
}

/* ---- rise 事件：spawn 守护线程 ---- */
static void daemonRise(DaemonHolder* holder, const std::string& name,
		const std::string& chunkPath, const std::string& configStr, int64_t intervalMs) {
	DaemonSlot* slot = holder->slot;
	std::shared_ptr<std::promise<int64_t>> prom = holder->result;
	std::thread([slot, chunkPath, configStr, intervalMs, prom]() {
		int64_t r = daemon_body(slot, chunkPath, configStr, intervalMs);
		try { prom->set_value(r); } catch (...) { /* future 已被消费/进程退出 */ }
	}).detach();
	std::cout << "Daemon '" << name << "' spawned, config: " << configStr << std::endl;
}

/* ---- free 事件：置停止 + 带超时等待退出 + 销毁 mutex/槽 ----
 * 超时（守护 tick 长时间阻塞）放弃等待并跳过槽释放（保 UAF），交给进程退出回收。 */
static void daemonFree(DaemonHolder* holder, const std::string& name) {
	daemon_request_stop(holder->slot);
	if (holder->fut.valid()) {
		std::future_status st = holder->fut.wait_for(std::chrono::milliseconds(holder->joinTimeoutMs));
		if (st == std::future_status::ready) {
			int64_t n = holder->fut.get();
			std::cout << "Daemon '" << name << "' joined, total publishes=" << n << std::endl;
			releaseSlot(holder->slot);
			return;
		}
		std::cout << "Daemon '" << name << "' not exited within " << holder->joinTimeoutMs
			<< "ms, abandoning slot (thread still running)" << std::endl;
		return;
	}
	releaseSlot(holder->slot);
}

/* ---- 守护硬件变量工厂 ---- */
std::unique_ptr<MtVars_Daemon> MtVars_Daemon::Factory(const mt::MatrixObject& defs, int64_t i,
		std::vector<std::unique_ptr<DaemonHolder>>& registry) {
	int64_t idxName = defs.indexOfCol(0, "name");
	int64_t idxChunk = defs.indexOfCol(0, "chunk");
	std::string name = defs(i, idxName);
	std::string chunkRaw = MatrixVars_Hardware::getMt(defs, i, idxChunk);

	auto modulePathOpt = getEnvVar("BOT4CJ_MODULE_PATH");
	std::string modulePath = modulePathOpt.value_or("./modules");
	std::string chunkPath = resolveDaemonChunk(chunkRaw, modulePath);
	std::cout << "Create a Daemon hard-Var: " << name << ", chunk: " << chunkPath << std::endl;

	/* 单遍列扫描：name/chunk 之外收集为配置串 "k=v;k=v"，提取 interval（缺省 5ms） */
	std::string cfgAcc;
	int64_t ivAcc = 5;
	for (int64_t j = 0; j < static_cast<int64_t>(defs.cols()); ++j) {
		const std::string& colName = defs(0, j);
		if (colName == "name" || colName == "chunk") { continue; }
		std::string v = MatrixVars_Hardware::getMt(defs, i, j);
		cfgAcc += colName + "=" + v + ";";
		if (colName == "interval" && v != "-") {
			ivAcc = parseInterval(v, name, defs.getName(), i);
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

	auto holder = std::make_unique<DaemonHolder>(slot, joinTimeoutMs);
	DaemonHolder* hp = holder.get();
	registry.push_back(std::move(holder));

	return std::unique_ptr<MtVars_Daemon>(new MtVars_Daemon(
		[hp](const Json&) { return daemonPlay(hp); },
		[hp, name, chunkPath, configStr, intervalMs]() { daemonRise(hp, name, chunkPath, configStr, intervalMs); },
		[hp, name]() { daemonFree(hp, name); }));
}

} // namespace bot