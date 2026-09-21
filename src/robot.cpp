/* robot.cpp —— RobotStruct 编排门面（重构 2026-09-20：按类拆分 TU）
 * 原单文件五类（HwRegistry/MetaProvider/ScriptLoader+HwFactory/ActionRunner/
 * RobotStruct）已拆分：hw_registry.cpp / meta_provider.cpp / hw_factory.cpp /
 * action_runner.cpp；本 TU 只剩装配流程 + 生命周期编排。 */

#include "robot.hpp"

/* 2.2：robot.hpp 不再传导 Lua 头——本 TU 编排守护生命周期，显式引入 */
#include "daemonvar.hpp"
#include "mtinclude.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

namespace bot {

/* ==================== RobotStruct：编排门面 ==================== */

/* ---- 构建硬件：两阶段（build 失败上抛，active 由 finally 清理）---- */
void RobotStruct::active() {
	/* 先标「已上电」再装配：createHardVars / build 中途抛出时，析构仍会补一次
	 * deactivate 收拾已经 build 成功的那部分（deactivate 逐项容错，半成品可安全跑）。 */
	deactivated_ = false;
	factory_.createHardVars(matrices_);
	/* F-05：$ 引用解析本是惰性的（首次 collect 时）——指向未定义变量会在
	 * 驱动 build 的真机事务中途才报错；装配完成后、任何 build 之前统一
	 * 校验存在性（此时全部变量已注册，前向引用合法）。 */
	for (const auto& p : metaProviders_) { p->validate(); }
	for (MatrixVars_Hardware* item : registry_.sequence()) {
		/* build 期同样要看停止标志（2026-09-21 第七轮审计）：多驱动上电时每个
		 * build 内部都有真机延时，整段可能跑很久，而信号处理器在 active() 之前
		 * 一行就装好了（main.cpp 的 installStopSignalHandlers）。旧实现在这里不查
		 * 标志 ⇒ 第一次 Ctrl-C **无任何可观察效果**，运维必然再按一下，而第二次
		 * 是 `signal(SIG_DFL) + raise` 强杀：已 build 成功的器件原地带电、
		 * deactivate 一行不执行——恰好把"断电是生命周期不变式"打穿在它最需要的
		 * 那条路径上。现在第一次按键即中止装配并上抛，由 runRobotAndCleanup 的
		 * catch 走 deactivate，把已 build 的那部分逆序断电。 */
		if (stopRequested()) {
			throw std::runtime_error("上电被中断（收到停止信号）：已装配的器件正在断电");
		}
		item->build();
	}
}

/* ---- 消除硬件：先统一置守护 stop（不逐个 join，避免同步饿死），再逆序 destroy ----
 * 倒序不只是一个习惯，它承担一条资源不变式：接口器件（I2C/SPI/UART）的 fd 只在
 * 它自己的 destroy 里关闭，而挂在它上面的器件 destroy 要用同一个 fd 做去初始化
 * （写寄存器/关断/断电）——倒序保证接口器件最后关闭 fd，器件 destroy 期间句柄有效。
 * 该顺序的源头是 build 期约束：器件 build 要经 `$` 引用拿 fd，就必须让总线先 build，
 * 即总线必须先声明（总线前向引用在 build 期失败，见 robots/test/ref_bus_forward.sql）。 */
void RobotStruct::deactivate() {
	for (const auto& h : daemonHolders_) {
		h->requestStop();
	}
	/* ---- 断电屏障（审计 P2）----
	 * 统一等到守护线程退场，然后才动硬件。原实现把 join 延后到各守护自己的
	 * destroy，于是执行器可能已经断电、而某个守护正在跑的那一拍又写它一次
	 * ——最后一次写随机胜出，进程退出时器件可能重新带电。
	 * deadline 全体共享（取各守护 joinTimeout 的最大值）：逐个累加会让最坏
	 * 退出延迟随守护数量线性增长。等过之后 destroy 里的 waitForExit 幂等直返。
	 * 超时弃管的守护仍可能碰总线——那一条由 waitUntil 的告警如实说明。 */
	if (!daemonHolders_.empty()) {
		int64_t budgetMs = 0;
		for (const auto& h : daemonHolders_) {
			budgetMs = std::max(budgetMs, h->joinTimeoutMs());
		}
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(budgetMs);
		std::vector<DaemonRunner*> abandoned;
		for (const auto& h : daemonHolders_) {
			if (!h->waitUntil(deadline)) { abandoned.push_back(h.get()); }
		}
		/* ---- 二段屏障（审计 P1 修复，2026-09-21）----
		 * 弃管只是交出槽所有权，detached 线程并没有停——下面紧跟着的逆序
		 * destroy 会关闭总线/去初始化器件，与仍在 tick/cleanup 的线程构成
		 * 硬件竞态。这里在动硬件之前给弃管守护一次真正退场的机会（宽限 =
		 * 共享预算，封顶 30s：无法在 join 超时内退场的守护多半卡死在不可
		 * 中断的调用里，更长的等待只是把停机挂死换掉竞态）。宽限内退场的
		 * 线程不再与 destroy 并发；耗尽仍活着的才真正带着竞态进入 destroy，
		 * 由下方告警如实点名。 */
		if (!abandoned.empty()) {
			const int64_t graceMs = std::min<int64_t>(budgetMs, deactivateGraceMs_);
			std::cerr << "Deactivate: " << abandoned.size()
				<< " daemon(s) outlived the stop barrier, granting " << graceMs
				<< "ms grace before hardware de-init" << std::endl;
			std::size_t stillRunning = 0;
			for (auto* d : abandoned) {
				if (!d->awaitExit(graceMs)) { ++stillRunning; }
			}
			if (stillRunning > 0) {
				std::cerr << "Deactivate: " << stillRunning
					<< " daemon(s) STILL running after grace window —"
					<< " hardware de-init will race with them (they may touch the"
					<< " bus after de-init)" << std::endl;
			}
		}
	}
	std::string lastWarn;
	std::size_t failed = 0;
	const auto& seq = registry_.sequence();
	for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
		try {
			(*it)->destroy();
		} catch (const std::exception& e) {
			++failed;
			/* 同因重复失败（如某次 play 已抛同错）只提示一次，避免刷屏 */
			if (std::string(e.what()) != lastWarn) {
				std::cerr << "Deactive warning: " << e.what() << std::endl;
				lastWarn = e.what();
			}
		}
	}
	/* F-04：清理失败显式汇总——静默跳过的去初始化会让器件滞留最后指令状态
	 * （如 MAX30102 未关断、BH1750 未断电），必须让调用方看见 */
	if (failed > 0) {
		std::cerr << "Deactivate: " << failed << " of " << seq.size()
			<< " hardware variable(s) failed to destroy (see warnings above)" << std::endl;
	}
	deactivated_ = true;
}

/* ---- 兜底断电：让「不漏断电」成为类型保证而不是调用点纪律 ----
 * main 的正常路径已显式 deactivate，此处覆盖的是**其它**退出路径：新增的早退
 * return、以及 catch 之外的意外。漏断电的代价是物理的（执行器停在最后指令位置、
 * I2C 从设备滞留中间态、守护线程还在采样），所以宁可多一层幂等调用。
 * 析构里异常不得逃出——逃出即 std::terminate，反而连告警都看不到，故整体吞掉留痕。 */
RobotStruct::~RobotStruct() {
	if (deactivated_) { return; }
	try {
		deactivate();
	} catch (...) {
		std::cerr << "Deactivate: failed during destruction"
			<< " (hardware may remain energized)" << std::endl;
	}
}

/* ---- 执行 @Action 矩阵 + 内联 @Alias ---- */
void RobotStruct::run(bot::Reader& input) {
	std::vector<mt::MatrixObject> actionMatrices;
	mt::readMatrix(input, actionMatrices);
	/* @Include 展开（无 @Include 时逐字不动）：先于 @Alias/@Vars 注册与
	 * @Action 派发——被并入的标量行与动作列与本体的走完全相同的后续
	 * 路径，展开只发生在矩阵层。归并内核与离线合成器 mtmerge 共用
	 * （mtmerge_core），两条入口的冲突裁决不会漂移。 */
	mtinclude::expandIncludes(actionMatrices, actionIncludeBaseDir_);

	/* 阶段 1：会话变量作用域。内联 @Alias/@Vars 是**本次 run 的**局部绑定，
	 * 注册进会话覆盖层，函数出口（含异常路径）由 RAII 一并回滚：
	 *   - 结构定义不再可能被永久遮蔽（旧实现改写 byName_，进程退出前一直生效）；
	 *   - pool_ 不因重复 run() 增长（agent 长驻模式的泄漏源）；
	 *   - 一个以报错收场的脚本不会留下任何注册表副作用。
	 * 作用域起点必须在注册之前、终点在派发之后。 */
	HwRegistry::SessionScope sessionScope(registry_);

	std::set<std::string> session;
	for (auto& matrix : actionMatrices) {
		std::string mtName = trim(matrix.getName());
		if (mtName == "Alias") { factory_.registerAliasMatrix(matrix, &session); }
		else if (mtName == "Vars") { factory_.registerVarsMatrix(matrix, &session); }
	}
	runner_.runAction(actionMatrices);
}

/* ---- agent 模式：变量存在性判定（active() 之前可判定）---- */
bool RobotStruct::declaresVariable(const std::string& name) const {
	for (const auto& matrix : matrices_) {
		std::int64_t idx = matrix.indexOfCol(0, "name");
		if (idx < 0) { continue; }
		for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
			if (trim(matrix(i, idx)) == name) { return true; }
		}
	}
	return false;
}

/* ---- agent 模式：注入 Lua 驱动变量（LLM / 摄像头共用）---- */
void RobotStruct::injectLuaDriver(const std::string& driverTable, const std::string& varName) {
	/* 只接受安全标识符：这两个名字会被拼进合成 SQL，且 driverTable 还会被
	 * HwFactory 当成 modules/<表名>.lua 去解析——放行引号/分号即等于让
	 * 命令行参数改写结构脚本。判据在 util::isSafeIdent 单点（与 agentmode
	 * 的 CLI 预检同一防线）。 */
	if (!isSafeIdent(driverTable) || !isSafeIdent(varName)) {
		throw IllegalArgumentException("驱动表名与变量名必须是安全标识符（字母/数字/下划线）: '"
			+ driverTable + "' / '" + varName + "'");
	}

	/* 顺序不变式（B0-5）：MetaProvider 持有的 matrix_ 是对 matrices_ **元素的
	 * 引用**，而下面这条 push_back 会让 vector 重分配——重分配后旧引用全部悬垂。
	 * 这条不变式此前只靠「注入必须在 active() 之前」的约定维系（MetaProvider 在
	 * active()→createHardVars 里才建）。此处把它变成显式断言：一旦有人在
	 * active() 之后注入，立刻响亮失败，而不是留一个悬垂引用到派发期才炸。 */
	if (!metaProviders_.empty()) {
		throw IllegalArgumentException("injectLuaDriver must be called before active():"
			" metadata providers already hold references into the matrix list");
	}

	std::string sql = "CREATE TABLE " + driverTable + " (name String);\n"
		+ "INSERT INTO " + driverTable + " (name) VALUES ('" + varName + "');\n";
	MemoryReader reader(std::move(sql));
	std::vector<mt::MatrixObject> extra;
	ScriptLoader::readSQL(reader, extra);
	for (auto& m : extra) { matrices_.push_back(std::move(m)); }
}

} // namespace bot
