#pragma once
/* daemonvar.hpp —— 守护线程硬件变量（移植 bot4cj/src/HV/Daemon.cj）
 * 共享槽 daemon_slot_t 布局（aarch64/x86_64 与 modules/lib/pthread.lua 严格一致）：
 *   offset 0   char   mutex[64]      pthread_mutex_t（aarch64=48B / x86_64=40B，预留 64）
 *   offset 64  int32  count          已 publish 次数
 *   offset 68  int32  nvals          本次值个数
 *   offset 72  double values[32]     采样值
 *   offset 328 uint8  stop           停止标志
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "hardware.hpp"
#include "luaend.hpp"

namespace bot {

constexpr int32_t SLOT_COUNT_OFF = 64;
constexpr int32_t SLOT_NVALS_OFF = 68;
constexpr int32_t SLOT_VALUES_OFF = 72;
constexpr int32_t SLOT_STOP_OFF = 328;
constexpr int32_t SLOT_SIZE = 336;
constexpr int32_t SLOT_MAXVALS = 32;

struct DaemonSlot {
	char mutex[64];
	std::int32_t count;
	std::int32_t nvals;
	double values[SLOT_MAXVALS];
	std::uint8_t stop;
};
static_assert(sizeof(DaemonSlot) == SLOT_SIZE, "DaemonSlot size must be 336 bytes");
static_assert(offsetof(DaemonSlot, count) == SLOT_COUNT_OFF, "count offset");
static_assert(offsetof(DaemonSlot, nvals) == SLOT_NVALS_OFF, "nvals offset");
static_assert(offsetof(DaemonSlot, values) == SLOT_VALUES_OFF, "values offset");
static_assert(offsetof(DaemonSlot, stop) == SLOT_STOP_OFF, "stop offset");

/* stop 标志读写统一经槽内 mutex 临界区（P2 数据竞争防线） */
void daemon_request_stop(DaemonSlot* slot);
bool daemon_stop_requested(DaemonSlot* slot);

/* ---- chunk 路径寻址（issue #19 多路径）---- */
std::string firstExistingModule(const std::string& paths, const std::string& rel);
std::string resolveDaemonChunk(const std::string& chunk, const std::string& modulePath);

/* ---- 可变状态持有者：持有 slot、超时与 promise（守护线程经 shared_ptr 回写）---- */
class DaemonHolder {
public:
	DaemonSlot* slot;
	int64_t joinTimeoutMs;
	std::shared_ptr<std::promise<int64_t>> result;
	std::future<int64_t> fut;
	/* D1：线程是否已 spawn——未 spawn（build 中途失败路径）promise 永不 set，
	 * 停机时不能白等 join 超时，直接释放槽。 */
	std::atomic<bool> spawned{false};
	/* D2：主线程放弃等待后置位，槽所有权移交守护线程（线程退出时自行释放），
	 * 主线程此后不再触碰槽——防放弃路径的 use-after-free。 */
	std::atomic<bool> abandoned{false};
	/* 弱自引用（Factory 置位）：daemonRise 经 lock() 取回 shared_ptr 交给守护线程保生命周期。
	 * 必须是 weak_ptr——shared_ptr 自引用会让引用计数永不归零，每个守护固定泄漏
	 * 208B/4 次分配，LeakSanitizer 下所有守护 E2E 用例判失败。
	 * 改 weak 后所有权语义不变：线程持有 lock() 得到的强引用，abandoned 路径下
	 * 线程退出时仍是槽的唯一持有者并自行释放（D2）。 */
	std::weak_ptr<DaemonHolder> self;

	explicit DaemonHolder(DaemonSlot* s, int64_t timeout)
		: slot(s), joinTimeoutMs(timeout), result(std::make_shared<std::promise<int64_t>>()),
		  fut(result->get_future()) {}
};

/* 守护线程体：独立 lua_State，注入槽地址 + 配置串，setup 一次 + 逐拍 daemon_tick。
 * 持 holder 的 shared_ptr 保生命周期；退出时若 abandoned 置位则自行释放槽（D2）。 */
int64_t daemon_body(std::shared_ptr<DaemonHolder> holder, const std::string& chunk,
		const std::string& config, int64_t interval_ms);

/* ---- 守护硬件变量 ---- */
class MtVars_Daemon : public MatrixVars_Hardware {
	explicit MtVars_Daemon(PlayFn things, std::optional<NoArg> rise, std::optional<NoArg> free)
		: MatrixVars_Hardware(std::move(things), std::move(rise), std::move(free)) {}

public:
	static std::unique_ptr<MtVars_Daemon> Factory(const mt::MatrixObject& defs, std::int64_t i,
			std::vector<std::shared_ptr<DaemonHolder>>& registry);
};

} // namespace bot