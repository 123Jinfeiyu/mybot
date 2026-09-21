#pragma once
/* daemonvar.hpp —— 守护线程硬件变量（移植 bot4cj/src/HV/Daemon.cj）
 * 共享槽 daemon_slot_t 布局（aarch64/x86_64 与 modules/lib/pthread.lua 严格一致）：
 *   offset 0   char   mutex[64]      pthread_mutex_t（aarch64=48B / x86_64=40B，预留 64）
 *   offset 64  int32  count          已 publish 次数
 *   offset 68  int32  nvals          本次值个数
 *   offset 72  double values[32]     采样值
 *   offset 328 uint8  stop           停止标志
 *   offset 332 int32  status         守护 setup 状态（N-27：0=正常；负=失败码）
 *
 * status 与 stop 之间那 3 字节是 stop 之后的既有对齐填充——status 落在这里
 * **不改变槽尺寸**（仍 336B），也保持 count/nvals/values/stop 偏移不变；
 * 具名 reserved_ 只为显式占位，避免将来插字段时把 status 挤进隐式填充区。
 */
#include <atomic>
#include <chrono>
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
constexpr int32_t SLOT_STATUS_OFF = 332;
constexpr int32_t SLOT_SIZE = 336;
constexpr int32_t SLOT_MAXVALS = 32;

struct DaemonSlot {
	char mutex[64];
	std::int32_t count;
	std::int32_t nvals;
	double values[SLOT_MAXVALS];
	std::uint8_t stop;
	std::uint8_t reserved_[3];	/* stop 之后的既有填充，显式具名 */
	std::int32_t status;
};
static_assert(sizeof(DaemonSlot) == SLOT_SIZE, "DaemonSlot size must be 336 bytes");
static_assert(offsetof(DaemonSlot, count) == SLOT_COUNT_OFF, "count offset");
static_assert(offsetof(DaemonSlot, nvals) == SLOT_NVALS_OFF, "nvals offset");
static_assert(offsetof(DaemonSlot, values) == SLOT_VALUES_OFF, "values offset");
static_assert(offsetof(DaemonSlot, stop) == SLOT_STOP_OFF, "stop offset");
static_assert(offsetof(DaemonSlot, status) == SLOT_STATUS_OFF, "status offset");

/* stop 标志读写统一经槽内 mutex 临界区（P2 数据竞争防线）。
 * Lua 侧 pthread.lua 的 should_stop/signal_stop 同样持锁——两侧契约一致，
 * TSan 视角无裸读写竞态。status 同理（get/set_status 持锁）。 */
void daemon_request_stop(DaemonSlot* slot);
bool daemon_stop_requested(DaemonSlot* slot);

/* ---- chunk 路径寻址（issue #19 多路径）---- */
std::string firstExistingModule(const std::string& paths, const std::string& rel);
std::string resolveDaemonChunk(const std::string& chunk, const std::string& modulePath);

/* N-7：本次进程内被弃管（join 超时 abandoned）的守护线程数——
 * 供 main 退出前统一告警（弃管线程的 lua_State 在 exit 路径上不可回收） */
int abandonedDaemonCount();

/* ---- 守护运行器（R-7：状态机内聚）----
 * 原 DaemonHolder + daemonRise/daemonFree/daemon_body 三个自由函数分散读写
 * spawned/abandoned/slot/promise 五个字段；收敛后对外只留
 * start / requestStop / waitForExit / readSample / slot 五个入口。 */
class DaemonRunner {
	/* F-01→2.4：槽所有权改 shared_ptr。跨线程读写成员经 std::atomic_load/
	 * atomic_store（C++17 自由函数，seq_cst）；所有权语义由引用计数承担：
	 * 守护线程体入口取一份强引用副本（整个 tick 循环期间持有），主线程
	 * waitForExit 弃管时原子交出自己的引用——槽在最后一个持有者退场时
	 * 由自定义删除器（mutex_destroy + free）释放。
	 * 取代原 abandoned_/exitCheckDone_ 双原子 + slot 指针 exchange 的
	 * 「唯一胜者」协议（D2/审计 #2）：引用计数下任何交错都恰好释放一次，
	 * 无需仲裁。 */
	std::shared_ptr<DaemonSlot> slot_;
	int64_t joinTimeoutMs_;
	/* 守护名：start() 定型。停机日志要用它，而断电屏障是在 RobotStruct 里
	 * 逐个等待（那时拿不到 MtVars_Daemon::name_），故名字归 runner 自己持有。 */
	std::string name_;
	std::shared_ptr<std::promise<int64_t>> result_;
	std::future<int64_t> fut_;
	/* D1：线程是否已 spawn——未 spawn（build 中途失败路径）promise 永不 set，
	 * 停机时不能白等 join 超时，直接释放槽。
	 * 不变式：置位必须先于守护线程构造（start 实现），否则线程已读写槽而本
	 * 标志仍为 false 的窗口内，waitForExit 会误判「未 spawn」提前释放槽。 */
	std::atomic<bool> spawned_{false};
	/* 弱自引用（Factory 经 bindSelf 置位）：start 经 lock() 取回 shared_ptr
	 * 交给守护线程保生命周期。必须是 weak_ptr——shared_ptr 自引用会让引用
	 * 计数永不归零，每个守护固定泄漏 208B/4 次分配，LeakSanitizer 下所有
	 * 守护 E2E 用例判失败。 */
	std::weak_ptr<DaemonRunner> self_;

	/* 守护线程体：独立 lua_State，注入槽地址 + 配置串，setup 一次 + 逐拍
	 * daemon_tick。入口取槽的强引用副本保全程存活（2.4）。 */
	int64_t body(const std::string& chunk, const std::string& config, int64_t intervalMs);

public:
	explicit DaemonRunner(std::shared_ptr<DaemonSlot> s, int64_t timeout)
		: slot_(std::move(s)), joinTimeoutMs_(timeout),
		  result_(std::make_shared<std::promise<int64_t>>()),
		  fut_(result_->get_future()) {}

	/* RAII 兜底：只原子交出自己的引用，不触碰槽本身——未 spawn 时本成员是
	 * 唯一所有权（线程从未取副本），交出即由删除器释放；已 spawn 的场景下
	 * 线程副本保证槽存活到其退出，析构交出引用既不悬垂也不泄漏。 */
	~DaemonRunner();

	/* 供线程捕获 self 保生命周期（构造期无法取得自身 shared_ptr） */
	void bindSelf(std::shared_ptr<DaemonRunner> self) { self_ = std::move(self); }

	/* spawn 守护线程（槽已释放/自引用缺失时显式报错） */
	void start(const std::string& name, const std::string& chunk,
			const std::string& config, int64_t intervalMs);

	/* 置停止标志（槽已交出/已释放则无事可做） */
	void requestStop();

	/* 停机等待核心：置停止 + 等到 deadline + 交出引用。
	 * 返回 true = 线程已退场；false = 超时弃管。
	 * deadline 由调用方给：单个守护停机（destroy）传 now+joinTimeoutMs_；
	 * 断电屏障（RobotStruct::deactivate）传**全体共享**的同一个 deadline——
	 * 逐个累加会让 N 个守护的最坏退出延迟变成 N×joinTimeout（审计 P2）。 */
	bool waitUntil(const std::chrono::steady_clock::time_point& deadline);

	/* 带自身超时等待线程退出并释放槽；超时则弃管——主线程交出自己的引用，
	 * 槽由守护线程副本兜底到最后（2.4：所有权移交即引用计数自然交接）。
	 * 幂等：槽已交出（含被断电屏障先等到/先弃管）即无事可做。 */
	void waitForExit();

	/* 弃管后的第二等待窗口（审计 P1 修复，2026-09-21）：
	 * waitUntil 超时弃管只交出所有权、不会让线程停下——detached 线程可能
	 * 仍在 tick/cleanup 中触碰总线。本方法**不改变弃管语义**（槽仍保持
	 * 已交出，readSample 照旧按 ERR 契约返回），只是让调用方在动硬件之前
	 * 再等一段宽限，给「慢拍/长阻塞但终会返回」的守护一次真正退场的机会，
	 * 把「daemon 活着 + 总线已销毁」的竞态窗口收敛到确实卡死的线程。
	 * 返回 true = 线程已确认退场（或从未 spawn / future 已消费）；false =
	 * 宽限耗尽仍未退场。幂等：已 joined（future 已消费）直接返回 true。 */
	bool awaitExit(int64_t graceMs);

	/* 断电屏障用：算全体共享预算时要取各守护 joinTimeout 的最大值 */
	int64_t joinTimeoutMs() const { return joinTimeoutMs_; }

	/* 守护名（start 时定型），停机日志用 */
	const std::string& name() const { return name_; }

	/* 主线程读采样快照；槽已交出/已释放时按 ERR 契约返回。
	 * 2.4：本方法加载本地强引用副本，读取期间槽由副本保证存活——
	 * 「play/readSample 与 destroy/waitForExit 同线程顺序执行」契约
	 * （审计中-2）下的 TOCTOU 缺口（load 到槽之后 destroy 才置空）由
	 * 代码保证堵死，不再依赖防线注释。TSan 实测记录见
	 * test/cpp/test_daemon_concurrency.cpp。 */
	Json readSample();

	/* 槽是否仍在位（2.4：原裸指针 slot() 入口收窄为布尔，避免调用方
	 * 拿到绕过所有权模型的裸指针） */
	bool slotAlive() const { return std::atomic_load(&slot_) != nullptr; }
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
	const char* kindName() const override { return "daemon"; }
	void build() override;
	void destroy() override;

	static std::unique_ptr<MtVars_Daemon> Factory(const mt::MatrixObject& defs, std::int64_t i,
			std::vector<std::shared_ptr<DaemonRunner>>& registry);
};

} // namespace bot