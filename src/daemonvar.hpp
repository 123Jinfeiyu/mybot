#pragma once
/* daemonvar.hpp —— 守护线程硬件变量（移植 bot4cj/src/HV/Daemon.cj）
 * 共享槽 daemon_slot_t 布局（aarch64/x86_64 与 modules/lib/pthread.lua 严格一致）：
 *   offset 0   char   mutex[64]      pthread_mutex_t（aarch64=48B / x86_64=40B，预留 64）
 *   offset 64  int32  count          已 publish 次数
 *   offset 68  int32  nvals          本次值个数
 *   offset 72  double values[64]     采样值（TD-3：32 → 64，支持高通道数传感器）
 *   offset 584 uint8  stop           停止标志
 *   offset 588 int32  status         守护 setup 状态（N-27：0=正常；负=失败码）
 *   offset 592 int32  quiesce        总线灭火位（BQ-1：仅弃管守护会被置位，见下）
 *
 * status 与 stop 之间那 3 字节是 stop 之后的既有对齐填充——status 落在这里
 * **不改变字段布局语义**，保持 count/nvals/values/stop 相对顺序；
 * 具名 reserved_ 只为显式占位，避免将来插字段时把 status 挤进隐式填充区。
 *
 * quiesce（2026-09-29，审计 P2-3 收口）：「停机灭火」协议位。宽限耗尽仍存活
 * 的守护线程在弃管**之前**由主线程置位；此后该守护 lua_State 里经 HAL.lua 走的
 * 一切真实总线传输（i2c/spi/uart）在进内核前被拒绝（modules/lib/HAL.lua 的
 * bus_quiesced 检查，持同一把槽锁读）。语义与 stop 分开：stop 是「请退出」
 * （宽限内退场的守护还要跑 daemon_cleanup 做去初始化，传输必须放行），
 * quiesce 是「物理隔离」——只给确定要与硬件销毁并发的那批线程（弃管者）。
 * 只置位、永不清零：deactivate 是进程终态；热删除的弃管者对应的器件已被
 * 摘除，其槽随引用计数消亡，没有「复位再用」的场景。
 *
 * TD-3 变更记录（2026-09-22）：
 * - values 从 32 增至 64 个 double（256B → 512B）
 * - 槽总大小从 336B 增至 592B
 * - stop 偏移从 328 → 584
 * - status 偏移从 332 → 588
 * - 向后兼容：旧代码使用 ≤32 值的槽仍然正常工作
 * BQ-1 变更记录（2026-09-29）：
 * - 追加 quiesce（offset 592），槽总大小 592 → 600（values 的 8 字节对齐
 *   使 struct 尾部填充到 600；旧 HAL.lua（无 quiesce 检查）在新槽上照常
 *   工作：它只读既有偏移）
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
constexpr int32_t SLOT_STOP_OFF = 584;   /* TD-3: 328 → 584 (64 values × 8 bytes = 512B) */
constexpr int32_t SLOT_STATUS_OFF = 588; /* TD-3: 332 → 588 */
constexpr int32_t SLOT_QUIESCE_OFF = 592;/* BQ-1: 592（status 之后原样追加） */
constexpr int32_t SLOT_SIZE = 600;       /* BQ-1: 592 → 600（quiesce 后 4B 尾部对齐填充） */
constexpr int32_t SLOT_MAXVALS = 64;     /* TD-3: 32 → 64 */

struct DaemonSlot {
	char mutex[64];
	std::int32_t count;
	std::int32_t nvals;
	double values[SLOT_MAXVALS];
	std::uint8_t stop;
	std::uint8_t reserved_[3];	/* stop 之后的既有填充，显式具名 */
	std::int32_t status;
	std::int32_t quiesce;
};
static_assert(sizeof(DaemonSlot) == SLOT_SIZE, "DaemonSlot size must match SLOT_SIZE");
static_assert(offsetof(DaemonSlot, count) == SLOT_COUNT_OFF, "count offset");
static_assert(offsetof(DaemonSlot, nvals) == SLOT_NVALS_OFF, "nvals offset");
static_assert(offsetof(DaemonSlot, values) == SLOT_VALUES_OFF, "values offset");
static_assert(offsetof(DaemonSlot, stop) == SLOT_STOP_OFF, "stop offset");
static_assert(offsetof(DaemonSlot, status) == SLOT_STATUS_OFF, "status offset");
static_assert(offsetof(DaemonSlot, quiesce) == SLOT_QUIESCE_OFF, "quiesce offset");

/* stop 标志读写统一经槽内 mutex 临界区（P2 数据竞争防线）。
 * Lua 侧 pthread.lua 的 should_stop/signal_stop 同样持锁——两侧契约一致，
 * TSan 视角无裸读写竞态。status 同理（get/set_status 持锁）。
 * quiesce 同一纪律（daemon_quiesce / HAL.lua 的 bus_quiesced 均持锁）。 */
void daemon_request_stop(DaemonSlot* slot);
bool daemon_stop_requested(DaemonSlot* slot);
void daemon_quiesce(DaemonSlot* slot);
bool daemon_quiesced(DaemonSlot* slot);

/* ---- 共享槽的模块内接缝（定义在 daemonvar.cpp 线程机 TU；daemonconf.cpp
 * 的 allocateSlot 装配时要用同一份 mutex 定位与释放例程）---- */
pthread_mutex_t* mutexOf(DaemonSlot* slot);
void releaseSlot(DaemonSlot* slot);

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
	/* BQ-1：finishTeardown 是否已对本守护执行过 daemon_quiesce——弃管后槽
	 * 引用已移交线程，外部无从再读槽内位，此镜像供日志断言与单测观测。 */
	std::atomic<bool> quiesced_{false};
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

	/* 供线程捕获 self 保生命周期（构造期无法取得自身 shared_ptr）；weak_ptr 的
	 * operator= 只有 const shared_ptr& 重载，按值接参 + std::move 不产生移动 */
	void bindSelf(const std::shared_ptr<DaemonRunner>& self) { self_ = self; }

	/* spawn 守护线程（槽已释放/自引用缺失时显式报错） */
	void start(const std::string& name, const std::string& chunk,
			const std::string& config, int64_t intervalMs);

	/* 置停止标志（槽已交出/已释放则无事可做） */
	void requestStop();

	/* ---- 停机序列两段（BQ-2，2026-09-29 审计 P2-3 收口）----
	 * 原 waitUntil（等待即可能弃管）+ awaitExit（弃管后再宽限）合并重排为：
	 *   ① requestStopAndWait(deadline)：置 stop + 等退场，**不弃管**——
	 *      deadline 内未退场也保持槽在手（返回 false），灭火与弃管都还来得及；
	 *   ② finishTeardown(graceMs, waitStart)：给宽限（graceMs<=0 跳过），
	 *      宽限内退场 → true；仍未退场 → **先置 quiesce（灭火）再弃管**。
	 * 重排的动机：旧序在等待超时点立即弃管、之后才给宽限，灭火位若要在弃管
	 * 前置上就只能设在宽限之前——那会误伤「宽限内退场」的守护：它们的
	 * daemon_cleanup 还要做去初始化传输，被拒即器件滞留采样态。新序把
	 * quiesce 收敛到「宽限也耗尽」的最后一刻，只命中确定要与硬件销毁并发
	 * 的线程（审计 P2-3：弃管守护触总线的竞态窗口从「无界」收敛到「灭火
	 * 置位时刻的在途单次传输」）。
	 * deadline/宽限语义与旧 waitUntil/awaitExit 一致：单守护 destroy 传
	 * joinTimeout + 零宽限；断电屏障传全体共享 deadline（逐个累加会让 N 个
	 * 守护的最坏退出延迟变成 N×joinTimeout）+ 封顶宽限。waitStart 仅用于
	 * 弃管告警里的真实耗时统计。 */
	bool requestStopAndWait(const std::chrono::steady_clock::time_point& deadline);
	bool finishTeardown(int64_t graceMs,
			const std::chrono::steady_clock::time_point& waitStart);

	/* 带自身超时等待线程退出并释放槽；超时则灭火 + 弃管——主线程交出自己的
	 * 引用，槽由守护线程副本兜底到最后（2.4：所有权移交即引用计数自然交接）。
	 * 幂等：槽已交出（含被断电屏障先等到/先弃管）即无事可做。 */
	void waitForExit();

	/* 断电屏障用：算全体共享预算时要取各守护 joinTimeout 的最大值 */
	int64_t joinTimeoutMs() const { return joinTimeoutMs_; }

	/* BQ-1：本守护是否已被灭火（finishTeardown 的宽限耗尽路径置位） */
	bool quiesced() const { return quiesced_.load(); }

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