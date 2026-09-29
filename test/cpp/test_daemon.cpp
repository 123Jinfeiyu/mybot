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
#include "robot.hpp"
#include "util_except.hpp"

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

/* ---------------- D/C4：parseInterval 边界表驱动（2026-09-21 补） ----------------
 * 此前 parseInterval 无任何直接单测（仅 E2E 偶发碰到），interval=0/负/溢出/
 * >1h/非整数 五类边界无回归。提为 bot::parseInterval 后可白盒表驱动覆盖。 */
TH_TEST(parse_interval_accepts_valid_bounds) {
	TH_CHECK(bot::parseInterval("1", "d", "Daemon", 1) == 1);
	TH_CHECK(bot::parseInterval("3600000", "d", "Daemon", 1) == 3600000);	/* 1h 上限 */
}

TH_TEST(parse_interval_rejects_non_integer) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not a valid integer",
		bot::parseInterval("abc", "d", "Daemon", 2));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not a valid integer",
		bot::parseInterval("1.5", "d", "Daemon", 2));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not a valid integer",
		bot::parseInterval("", "d", "Daemon", 2));
	/* 带空白未 trim：parseLongStrict 全串消费，前导/尾随空白即拒绝 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "not a valid integer",
		bot::parseInterval("  50  ", "d", "Daemon", 2));
}

TH_TEST(parse_interval_rejects_non_positive) {
	/* 0 走「must be > 0」专用分支（防 busy-loop 解释最清晰） */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "must be > 0 ms",
		bot::parseInterval("0", "d", "Daemon", 3));
	/* 负数在 OVERFLOW/负值分支先命中 → 归 out of range */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "out of range",
		bot::parseInterval("-5", "d", "Daemon", 3));
}

TH_TEST(parse_interval_rejects_overflow_and_over_an_hour) {
	/* 溢出（超 int64）→ parseLongStrict OVERFLOW → out of range */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "out of range",
		bot::parseInterval("99999999999999999999999999", "d", "Daemon", 4));
	/* 超过 1h 上限（3600000ms）显式拒绝，保证 joinTimeout=interval*5 不溢出 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "exceeds the maximum",
		bot::parseInterval("3600001", "d", "Daemon", 4));
}

TH_TEST(parse_interval_error_carries_table_and_row) {
	/* 报错需点名表名与行号（N-27/E2：非法值在结构期定位到源头） */
	try {
		bot::parseInterval("-1", "my-daemon", "Daemon", 7);
		TH_CHECK(false && "should have thrown");
	} catch (const bot::IllegalArgumentException& e) {
		const std::string what = e.what();
		TH_CHECK(what.find("my-daemon") != std::string::npos);
		TH_CHECK(what.find("Daemon") != std::string::npos);
		TH_CHECK(what.find("row 7") != std::string::npos);
	}
}


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

/* 审计 P1 修复（2026-09-21）：awaitExit——弃管后的第二等待窗口。
 * 弃管（waitUntil 超时）只交出槽所有权、不改变任何既有观测量（槽保持已
 * 交出、读按 ERR 契约返回），但 awaitExit 必须能在宽限内等到线程真正退场
 * ——RobotStruct::deactivate 的二段屏障据此在硬件销毁前收窄
 * 「daemon 活着 + 总线已销毁」的竞态窗口。幂等：future 已消费后再等直接
 * 返回 true，不重复消费。 */
TH_TEST(daemon_await_exit_waits_thread_exit_after_abandon) {
	setenv("BOT4CJ_MODULE_PATH", ".tmp", 1);
	writeSlowTickChunk();
	mt::MatrixObject defs = daemonDefs(kSlowTickRel);
	std::vector<std::shared_ptr<DaemonRunner>> registry;
	std::unique_ptr<MtVars_Daemon> var = MtVars_Daemon::Factory(defs, 1, registry);
	var->build();
	/* 先让线程进入慢拍（3s），再主动走弃管：500ms << 剩余慢拍 */
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	TH_CHECK(!registry[0]->waitUntil(
		std::chrono::steady_clock::now() + std::chrono::milliseconds(500)));
	TH_CHECK(!registry[0]->slotAlive());
	bot::Json err = var->play(bot::JsonObject());
	TH_CHECK(err.get("result")->getValue() == "ERR");
	/* 二段等待：累计已等 ~0.7s，慢拍 ~3s 结束，宽限 5s 覆盖余量 */
	TH_CHECK(registry[0]->awaitExit(5000));
	/* 幂等：线程已退场（future 已消费），再等立即返回 true */
	TH_CHECK(registry[0]->awaitExit(0));
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

/* ==================== 配置串字符校验（审计 P4）====================
 * DAEMON_CONFIG 是 "k=v;k=v" 串：注入用 lua_pushstring（C 字符串语义），Lua 侧按
 * gmatch("([^;=]+)=([^;]*)") 解析（modules/lib/sensor_daemon.lua）。于是列值里——
 *   NUL  → 其后**所有列**（含 proto/sign）在 Lua 侧静默消失，而加载期的
 *          checkDaemonProto/Fields/Sign 跑在逐列原始串上，全部通过；
 *   ';'  → 该值被提前截断，且若其后片段形如 `key=` 就会注入/覆盖守护真正
 *          理解的键（proto/path/addr/fields/sign…），schema 校验看不到这次注入。
 * 放过之后运行期只剩 "no daemon value" 这类间接症状，看不出是配置串被截断。
 * '=' 是**允许**的一侧（键不含 '='、值可含 '='，能原样往返），另有反向用例钉住。
 *
 * 走 SQL 路径而非 fx::oneMatrix 的 .mt 文本：';' 在矩阵格式里本身就是行分隔符、
 * NUL 是空白，都表达不出来；SQL 的引号态则一律字面保留（src/sqlparse.cpp）。
 * 校验点在 Factory（assembleDaemonConfig），早于 chunk 加载，故 chunk 路径
 * 写成不存在的 'x.lua' 也能精确命中本判据。 */
namespace {

void expectConfigRejected(const std::string& fieldValue) {
	std::string sql =
		"CREATE TABLE Daemon (name String, chunk String, fields String, proto String);\n"
		"INSERT INTO Daemon (name, chunk, fields, proto) VALUES ('d1', 'x.lua', '";
	sql += fieldValue;
	sql += "', 'i2c');\n";
	bot::MemoryReader sr(sql);
	bot::RobotStruct robot(sr);
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "DAEMON_CONFIG", robot.active());
}

} // namespace

TH_TEST(daemon_config_rejects_nul_in_column_value) {
	/* 校验域与 Lua 可见域不一致的那一条：C++ 侧 fields 合法、proto 合法，
	 * 拼出的串却在 push 时被截断成 "name=d1;chunk=x.lua;fields=temp" */
	expectConfigRejected(std::string("temp") + '\0' + "h,vin:s16");
}

TH_TEST(daemon_config_rejects_semicolon_in_column_value) {
	expectConfigRejected("temp;proto=uart");
}

TH_TEST(daemon_config_allows_equals_in_column_value) {
	/* '=' 不拒：Lua 侧 gmatch("([^;=]+)=([^;]*)") 的键不含 '='、值可含 '='，
	 * "extra=a=b" 能原样往返。拒它会白伤合法驱动参数（如某些配置串写法），
	 * 故这里钉住"允许"这一侧，防止将来把判据顺手放宽成三种字符全拒。 */
	setenv("BOT4CJ_MODULE_PATH", ".tmp", 1);
	writeNoTickChunk();
	mt::MatrixObject defs = fx::oneMatrix(
		"@Daemon\nname chunk extra;\nd1 " + std::string(kNoTickRel) + " a=b;\n@\n");
	std::vector<std::shared_ptr<DaemonRunner>> registry;
	std::unique_ptr<MtVars_Daemon> var = MtVars_Daemon::Factory(defs, 1, registry);
	TH_CHECK(var != nullptr);
	TH_CHECK(registry.size() == 1);
}
