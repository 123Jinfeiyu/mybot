/* test_daemon.cpp —— DaemonRunner 状态机（R-7 / F-08 尾项）
 *
 * 此前两条守护分支没有任何确定性触发手段：
 *   never-spawn：Factory 成功但 build 未调用（active 中途失败路径）→
 *                destroy 直接释放槽，不白等 join 超时
 *   abandon：   守护 tick 单拍耗时超过 join 超时 → 主线程弃槽（D2），
 *               槽所有权移交守护线程，线程退出时自行释放
 * 夹具：慢 tick chunk（ffi.C.usleep 3s/拍），interval=10ms，
 * join 超时 = max(interval*5, 2000) = 2000ms → 必然超时。
 */

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "daemonvar.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using bot::DaemonRunner;
using bot::MtVars_Daemon;

namespace {

const char* const kSlowTickRel = "test_daemon_slow_tick.lua";
const char* const kNoTickRel = "test_daemon_no_tick.lua";

void writeSlowTickChunk() {
	std::ofstream out(std::string(".tmp/") + kSlowTickRel, std::ios::binary | std::ios::trunc);
	out << "local ffi = require(\"ffi\")\n"
	    << "ffi.cdef(\"int usleep(unsigned int usec);\")\n"
	    << "function daemon_tick()\n"
	    << "  ffi.C.usleep(3000000)\n"
	    << "  return 1\n"
	    << "end\n";
}

/* chunk 本体加载/setup 成功，但不定义 daemon_tick——每拍 pcall(nil) 必错，
 * 让逐拍循环进入 C-L2 指数退避（这是退避路径唯一的确定性触发手段） */
void writeNoTickChunk() {
	std::ofstream out(std::string(".tmp/") + kNoTickRel, std::ios::binary | std::ios::trunc);
	out << "-- daemon_tick intentionally undefined\n"
	    << "return\n";
}

mt::MatrixObject daemonDefs(const std::string& chunk) {
	return fx::oneMatrix("@Daemon\nname chunk interval;\nd1 " + chunk + " 10;\n@\n");
}

} // namespace

TH_TEST(daemon_never_spawned_releases_slot_directly) {
	setenv("BOT4CJ_MODULE_PATH", ".tmp", 1);
	mt::MatrixObject defs = daemonDefs(kSlowTickRel);
	std::vector<std::shared_ptr<DaemonRunner>> registry;
	std::unique_ptr<MtVars_Daemon> var = MtVars_Daemon::Factory(defs, 1, registry);
	TH_CHECK(registry.size() == 1);
	TH_CHECK(registry[0]->slotAlive());
	var->destroy();	/* 未 spawn：直接释放槽，不白等 join 超时 */
	TH_CHECK(!registry[0]->slotAlive());
	/* 槽已释放后的读按 ERR 契约返回，不碰悬垂指针 */
	bot::Json err = var->play(bot::JsonObject());
	TH_CHECK(err.get("result")->getValue() == "ERR");
}

TH_TEST(daemon_slow_tick_abandons_slot_and_thread_releases_it) {
	setenv("BOT4CJ_MODULE_PATH", ".tmp", 1);
	writeSlowTickChunk();
	mt::MatrixObject defs = daemonDefs(kSlowTickRel);
	std::vector<std::shared_ptr<DaemonRunner>> registry;
	std::unique_ptr<MtVars_Daemon> var = MtVars_Daemon::Factory(defs, 1, registry);
	var->build();
	/* 先让守护线程进入慢拍（3s），stop 只能在下拍之间被看见——
	 * 否则线程可能在首个 tick 前就退出，测试落到 joined 路径上。 */
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	/* join 超时 2000ms < 剩余慢拍 ~2800ms → destroy 必然走 abandoned 路径 */
	var->destroy();
	/* 2.4：弃管 = 主线程原子交出自己的引用，成员随即置空——「槽由线程副本
	 * 持有、尚未释放」的中间态不再有外部可观测量（旧模型的槽指针非空断言
	 * 随所有权模型一同退役）。弃管后的读按 ERR 契约返回，不触碰已移交的槽。 */
	TH_CHECK(!registry[0]->slotAlive());
	bot::Json err = var->play(bot::JsonObject());
	TH_CHECK(err.get("result")->getValue() == "ERR");
	/* 守护线程完成当前拍（~3s）后作为最后所有者触发删除器（2.4）。
	 * 「线程侧释放无 UAF/泄漏」在普通运行下无外部可观测量，由 ASan/LSan
	 * 守门（run_san.sh）验证；此处保留等待让线程自然退场。
	 * 余量取 4s：3s 慢拍 + 调度延迟——负载高（并行跑测试）时 2.5s 的旧余量
	 * 曾被吃穿导致偶发失败（审计 L1）。 */
	std::this_thread::sleep_for(std::chrono::milliseconds(4000));
	TH_CHECK(!registry[0]->slotAlive());
}

/* C-L2 尾：错误退避最长 interval×256，退避睡眠若一次性睡满会吞掉 stop 请求
 * ——stop 落在一次超过 join 超时（保底 2000ms）的深睡中段时必然走弃管路径，
 * 停止语义被架空。睡眠分段查 stop 后，退避总时长不变而 stop 响应收敛到
 * 单片时长（100ms）。
 * 窗口推演（interval=10ms，join 超时 2000ms）：第 n 次错误退避 10×2^n ms，
 * 单次超过 join 超时自第 8 错起（2560ms），该次睡眠覆盖墙钟 [2540, 5100]ms；
 * 在其中段（~2800ms）请求 stop，旧行为要睡满 2560ms 才回头查标志
 * （剩余 >2000ms → 弃管），新行为 100ms 内退出（≈30 倍余量，不受负载影响）。 */
TH_TEST(daemon_backoff_sleep_interruptible_by_stop) {
	setenv("BOT4CJ_MODULE_PATH", ".tmp", 1);
	writeNoTickChunk();
	mt::MatrixObject defs = daemonDefs(kNoTickRel);
	std::vector<std::shared_ptr<DaemonRunner>> registry;
	std::unique_ptr<MtVars_Daemon> var = MtVars_Daemon::Factory(defs, 1, registry);
	var->build();
	/* 等到线程已进入单次超过 join 超时的深退避（第 8 错的 2560ms 睡眠中段） */
	std::this_thread::sleep_for(std::chrono::milliseconds(2800));
	int abandonedBefore = bot::abandonedDaemonCount();
	var->destroy();
	TH_CHECK(!registry[0]->slotAlive());
	/* 必须走 joined 路径：弃管计数不增长（旧行为此处 +1） */
	TH_CHECK(bot::abandonedDaemonCount() == abandonedBefore);
}
