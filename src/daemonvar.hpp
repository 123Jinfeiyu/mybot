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

/* ---- 守护运行器（R-7：状态机内聚）----
 * 原 DaemonHolder + daemonRise/daemonFree/daemon_body 三个自由函数分散读写
 * spawned/abandoned/slot/promise 五个字段；收敛后对外只留
 * start / requestStop / waitForExit / readSample / slot 五个入口。 */
class DaemonRunner {
	/* F-01：slot 由主线程（start/waitForExit/readSample）与守护线程（退出时自释，
	 * D2 路径）两端读写——必须原子化，否则是形式上的数据竞争（UB）。取用一律
	 * 显式 load()，拿到局部指针后再解引用（解引用期间槽由本线程的调用路径保证存活）。 */
	std::atomic<DaemonSlot*> slot_;
	int64_t joinTimeoutMs_;
	std::shared_ptr<std::promise<int64_t>> result_;
	std::future<int64_t> fut_;
	/* D1：线程是否已 spawn——未 spawn（build 中途失败路径）promise 永不 set，
	 * 停机时不能白等 join 超时，直接释放槽。 */
	std::atomic<bool> spawned_{false};
	/* D2：主线程放弃等待后置位，槽所有权移交守护线程（线程退出时自行释放），
	 * 主线程此后不再触碰槽——防放弃路径的 use-after-free。
	 * 审计 #2：置位与线程退出检查之间存在竞态窗口——线程先查（false，决定
	 * 不自释）、主线程后超时置位（以为线程会自释）→ 双方都不释放，槽 + mutex
	 * 永久泄漏。收口协议：线程退出循环后先公告「退出检查已做」（exitCheckDone_，
	 * seq_cst，保证先于它的全部槽访问可见），再复查 abandoned；主线程超时置位
	 * 后若观察到 exitCheckDone_，以 slot_.exchange(nullptr) 认领——exchange 的
	 * 唯一胜者释放，既无泄漏也无双重释放（详见 daemonvar.cpp 两处注释）。 */
	std::atomic<bool> abandoned_{false};
	/* 退出检查点：守护线程离开 tick 循环、做完全部槽访问后置位（审计 #2 协议）。
	 * 主线程观察到它即可安全认领槽——此刻线程对槽至多只剩一次 exchange（不解引用）。 */
	std::atomic<bool> exitCheckDone_{false};
	/* 弱自引用（Factory 经 bindSelf 置位）：start 经 lock() 取回 shared_ptr
	 * 交给守护线程保生命周期。必须是 weak_ptr——shared_ptr 自引用会让引用
	 * 计数永不归零，每个守护固定泄漏 208B/4 次分配，LeakSanitizer 下所有
	 * 守护 E2E 用例判失败。 */
	std::weak_ptr<DaemonRunner> self_;

	/* 守护线程体：独立 lua_State，注入槽地址 + 配置串，setup 一次 + 逐拍
	 * daemon_tick。退出时若 abandoned 置位则自行释放槽（D2）。 */
	int64_t body(const std::string& chunk, const std::string& config, int64_t intervalMs);

public:
	explicit DaemonRunner(DaemonSlot* s, int64_t timeout)
		: slot_(s), joinTimeoutMs_(timeout), result_(std::make_shared<std::promise<int64_t>>()),
		  fut_(result_->get_future()) {}

	/* RAII 兜底：槽只在 waitForExit（经 destroy()）里释放。若 Factory 分配槽之后、
	 * 变量进入销毁序列之前抛出（make_shared 的 bad_alloc 等），或任何绕过
	 * deactivate 的新出口，都会泄漏 slot + 未销毁的 mutex。未 spawn 时线程不持有
	 * 槽，析构可安全回收；已 spawn 的槽所有权归 waitForExit / abandoned 路径下的
	 * 守护线程，此处绝不触碰（防 use-after-free）。 */
	~DaemonRunner();

	/* 供线程捕获 self 保生命周期（构造期无法取得自身 shared_ptr） */
	void bindSelf(std::shared_ptr<DaemonRunner> self) { self_ = std::move(self); }

	/* spawn 守护线程（槽已释放/自引用缺失时显式报错） */
	void start(const std::string& name, const std::string& chunk,
			const std::string& config, int64_t intervalMs);

	/* 置停止标志（槽内 mutex 临界区；槽已释放则无事可做） */
	void requestStop();

	/* 带超时等待线程退出并释放槽；超时则置 abandoned 移交所有权（D2） */
	void waitForExit(const std::string& name);

	/* 主线程读采样快照；放弃路径下槽所有权已移交，按 ERR 契约返回。
	 * 并发契约（审计中-2，显式化）：play/readSample 与 destroy/waitForExit
	 * 必须同线程顺序执行（生产路径均在主线程）。本方法只对「abandoned 已
	 * 置位」按 ERR 返回，不防御与 destroy 并发时的 TOCTOU（load 到槽之后
	 * abandoned 才置位）——那需要完整的所有权仲裁。防线是本条契约而非
	 * 代码；TSan 实测记录见 test/cpp/test_daemon_concurrency.cpp。 */
	Json readSample();

	DaemonSlot* slot() const { return slot_.load(); }
};

/* ---- 守护硬件变量（R-3：真实子类，持 runner 的 shared_ptr）----
 * build/destroy 的参数（chunk 路径/配置串/interval）在 Factory 时定型为成员。 */
class MtVars_Daemon : public MatrixVars_Hardware {
	std::shared_ptr<DaemonRunner> runner_;
	std::string name_;
	std::string chunkPath_;
	std::string configStr_;
	int64_t intervalMs_;

	explicit MtVars_Daemon(std::shared_ptr<DaemonRunner> runner, std::string name,
			std::string chunkPath, std::string configStr, int64_t intervalMs)
		: runner_(std::move(runner)), name_(std::move(name)),
		  chunkPath_(std::move(chunkPath)), configStr_(std::move(configStr)),
		  intervalMs_(intervalMs) {}

public:
	Json play(const Json&) override;
	void build() override;
	void destroy() override;

	static std::unique_ptr<MtVars_Daemon> Factory(const mt::MatrixObject& defs, std::int64_t i,
			std::vector<std::shared_ptr<DaemonRunner>>& registry);
};

} // namespace bot