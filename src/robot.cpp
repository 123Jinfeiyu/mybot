/* robot.cpp —— RobotStruct 编排门面：生命周期 + 热插提交段
 * 原单文件五类（HwRegistry/MetaProvider/ScriptLoader+HwFactory/ActionRunner/
 * RobotStruct）已拆分：hw_registry.cpp / meta_provider.cpp / hw_factory.cpp /
 * action_runner.cpp；2026-10-01 起 HotSqlPlanner 只读计划层也在自己的 TU
 * （hotsql_plan.cpp）——本 TU 只剩装配流程、生命周期编排与热插提交
 * （commitHotBatch / executeHotDeletes / dropTableShells）。 */

#include "robot.hpp"

/* 2.2：robot.hpp 不再传导 Lua 头——本 TU 编排守护生命周期，显式引入 */
#include "daemonvar.hpp"
#include "mtinclude.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

#include "util_env.hpp"
#include "util_str.hpp"		/* joinNames（点名/成因列表拼接单点） */

namespace bot {

namespace {

/* ---- 守护退场屏障：deactivate（断电前）与热删除（摘件前）共用的同一套安全序列 ----
 * 原先在两处各手抄一份，属安全序列的分叉温床（2026-09-25 去重），机械步骤：
 * requestStop 全体 → 全体共享一个 deadline 等待（取各守护 joinTimeout 的最大值；
 * 逐个累加会让最坏退出延迟随守护数量线性增长）→ 超时者给二段宽限（封顶
 * graceCapMs）→ 宽限耗尽仍存活者灭火（quiesce）+ 弃管，如实点名。审计出处与理由：
 *  屏障本体（审计 P2）：原实现把 join 延后到各守护自己的 destroy，于是执行器
 *  可能已经断电、而某个守护正在跑的那一拍又写它一次——最后一次写随机胜出，
 *  进程退出时器件可能重新带电。等过之后 destroy 里的 waitForExit 幂等直返。
 *  二段宽限（审计 P1 修复，2026-09-21）：弃管只是交出槽所有权，detached 线程
 *  并没有停——调用点紧跟着的逆序 destroy 会关闭总线/去初始化器件，与仍在
 *  tick/cleanup 的线程构成硬件竞态。这里在动硬件之前给弃管守护一次真正退场的
 *  机会（宽限 = 共享预算，封顶 graceCapMs：无法在 join 超时内退场的守护多半
 *  卡死在不可中断的调用里，更长的等待只是把停机挂死换掉竞态）。宽限内退场的
 *  线程不再与 destroy 并发；耗尽仍活着的经 finishTeardown 灭火后弃管——此后
 *  其 HAL 传输在进内核前被拒（BQ-1），竞态窗口收敛到灭火时刻的在途单次传输。
 * 调用点契约：屏障返回后才许动硬件。stillNote 承载两处的后果差异（断电后
 * 器件可被重新写亮 vs 采样对象即将被摘除），由调用点如实描述。
 * 文案单点（同 agent_subprocess 的 reapGiveUpWarning 先例）：Deactivate 一侧
 * 被 test_robot 的 daemon_grace_ms 用例钉住（"granting 100ms grace"，数字与
 * "ms" 之间无空格），改文案须过那条用例。 */
void daemonStopBarrier(const std::vector<std::shared_ptr<DaemonRunner>>& holders,
		std::int64_t graceCapMs, const char* label, const char* stillNote) {
	for (const auto& h : holders) { h->requestStop(); }
	if (holders.empty()) { return; }
	int64_t budgetMs = 0;
	for (const auto& h : holders) {
		budgetMs = std::max(budgetMs, h->joinTimeoutMs());
	}
	const auto waitStart = std::chrono::steady_clock::now();
	const auto deadline = waitStart + std::chrono::milliseconds(budgetMs);
	std::vector<DaemonRunner*> missed;
	for (const auto& h : holders) {
		if (!h->requestStopAndWait(deadline)) { missed.push_back(h.get()); }
	}
	if (missed.empty()) { return; }
	const int64_t graceMs = std::min<int64_t>(budgetMs, graceCapMs);
	std::cerr << label << ": " << missed.size()
		<< " daemon(s) outlived the stop barrier, granting " << graceMs
		<< "ms grace before hardware de-init" << std::endl;
	std::size_t stillRunning = 0;
	for (auto* d : missed) {
		if (!d->finishTeardown(graceMs, waitStart)) { ++stillRunning; }
	}
	if (stillRunning > 0) {
		std::cerr << label << ": " << stillRunning
			<< " daemon(s) STILL running after grace window — " << stillNote
			<< " (their bus access has been quiesced)" << std::endl;
	}
}

/* 把列头行 + [rowBegin, rowEnd) 的数据行复制成一张独立矩阵（copyRows）——
 * 影子构建与瞬态 Action 提取共用——已随 HotSqlPlanner 迁至 hotsql_plan.cpp
 * （2026-10-01），本 TU 不再需要。 */

/* 热删除中途失败的报错文本拼装（拆自 executeHotDeletes，2026-09-26 批次 5）。
 * 口径：如实报「已删 N 个、M 个未删」+ 逐个点名 + 成因，并说明结构与注册表
 * 只反映已成功摘除的部分（不假装回滚——destroy 是物理断电，不可逆）。 */
std::string hotDeleteFailureMessage(std::size_t orderSize, std::size_t deletedVars,
		const std::vector<std::string>& deletedNames,
		const std::vector<std::string>& failures) {
	std::string msg = "热删除中途失败：已删除 " + std::to_string(deletedVars)
		+ " 个硬件变量";
	if (!deletedNames.empty()) {
		msg += "（";
		msg += joinNames(deletedNames);
		msg += "）";
	}
	msg += "，" + std::to_string(orderSize - deletedVars) + " 个未删除。成因：";
	msg += joinNames(failures, "; ");
	msg += "。结构与注册表只反映已成功摘除的部分，未删的行保持原状。"
		"（destroy 是物理断电，已断电的器件不会因报错而复电——修好成因后"
		"对剩余项重发 DELETE 即可。）";
	return msg;
}

/* 阶段 2 的表级/逐行只读校验（validateNewTableDecl / validateHotNewRow）已随
 * HotSqlPlanner 迁至 hotsql_plan.cpp（2026-10-01）。 */

} // namespace

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
	/* ---- 断电屏障 ----统一等到守护线程退场，然后才动硬件；序列理由、二段
	 * 宽限与告警文案都在单点 daemonStopBarrier（本文件顶部）。 */
	daemonStopBarrier(daemonHolders_, deactivateGraceMs_, "Deactivate",
		"hardware de-init will race with them (they may touch the"
		" bus after de-init)");
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
void RobotStruct::run(bot::Reader& input, const std::map<std::string, std::string>& injectVars) {
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
	/* 调用方注入的临时别名（#34 条件反射的工具侧「函数实参」入口）：在内联
	 * @Alias/@Vars 注册**之后**绑定，故显式实参覆盖脚本内同名默认值。键须为安全
	 * 标识符（否则拼进表达式/别名查找会出歧义），值按文本注册为 MtVars_Alias，
	 * 脚本里的 $(键) 经表达式解析器就地取值。override 自带 N-6 判据：被结构引用
	 * 为 controller/trigger 的名字拒绝覆盖。绑定同样落在 SessionScope 内，出口
	 * 随作用域整体回滚。 */
	for (const auto& kv : injectVars) {
		if (!isSafeIdent(kv.first)) {
			throw IllegalArgumentException("注入变量名不是安全标识符：'" + kv.first + "'");
		}
		registry_.override(kv.first, MtVars_Alias::Make(kv.second));
	}
	runner_.runAction(actionMatrices);
	runner_.runReflex(actionMatrices);
}

/* ==================== 热插表：结构 SQL 带电生效（2026-09-23）====================
 * 目标：上电常驻之后，用结构 SQL（CREATE TABLE / INSERT INTO）真正长出硬件
 * 变量——装配、开 fd、上电、断电一并纳入本进程的生命周期。
 *
 * 三阶段（解析 → 校验 → 提交）之所以必须**分阶段**，是因为每个阶段的失败
 * 形态不同：解析失败必须「活体一字未动」，校验失败必须「报错而不是跑出半
 * 成品」，提交阶段才允许触碰活体（且由 rollback 兜住任何中途失败）。
 *
 * 几个关键设计（每条都对应一个「不做就静默出错」的坑）：
 *  ① 影子容器：readSQL 的 INSERT 要求目标表已在同一容器里（C3 按列名落位、
 *     漏列/未知列报错全靠它）。为让「插进结构里既有表」也走同一条解析路径，
 *     先往影子容器填「每张既有表的表名 + 列头」，再让解析器往影子上追加行。
 *  ② 同名判定：新表名与结构里既有表重名（或本批次内 CREATE 两遍）必须报错
 *     ——否则 envelope 里会出现两张同名表，findByName 只会命中第一张，
 *     后一张静默作废。
 *  ③ 提交顺序＝声明序：既有表先 appendRow（矩阵是装配的输入，顺序不能反）、
 *     再 createHardVarsFrom，最后统一 build。$ 引用校验只对本批次新增的
 *     MetaProvider 做（既有变量在启动期已校验过），批内前向引用合法。
 *  ④ Action 表瞬态（见下方 4.5）：不入结构矩阵、不注册变量，硬件全部
 *     build 完成后执行一次即丢弃——排序正是为了让动作能引用本批次新建的
 *     变量；动作已下发的物理副作用不回退（与仓库既有口径一致）。
 *  ⑤ 回滚：逆序 destroy（断电）→ 撤注册（HwRegistry::rollbackTo）→ 弹
 *     MetaProvider/守护登记 → 恢复矩阵行/表。**先断电再撤所有权**：注册表
 *     只认硬件变量，断电是物理动作，顺序反了会让器件带电却已无人认识。
 */
std::string RobotStruct::applySql(const std::string& sql) {
	/* ---- 阶段地图（2026-09-25 拆分：每段的判据与理由在对应阶段函数实现处）----
	 *  0   前置条件（须已上电常驻）；
	 *  1   解析到影子容器（活体一字未动；readSQL 直接在影子上落删除）；
	 *  1.5 备删计划（planHotDeletes——先删后建的两条硬前提都要求它在插入之前）；
	 *  2   建计划 + 只读全量校验（planHotInsertions）；
	 *  2.4 批次新建表的删除对账（reconcileNewTableDels）；
	 *  3   合并视图判据：全结构消费判据 + 引用完整性（checkMergedView）；
	 *  4   提交 + 增量装配 + 上电（commitHotBatch——插入段任何一步失败 ⇒ 全回滚）；
	 *  4.6 热删除物理执行（executeHotDeletes，两个执行序按 deleteFirst 分派）；
	 *  4.7 整表撤下（dropTableShells）；
	 *  尾  生效摘要（REPL /sql 与 agent apply_sql 共用同一句话）。 */
	if (deactivated_) {
		throw IllegalArgumentException("热插表需要机器人已上电常驻（active() 之后）；"
			"未上电时请把语句写进结构脚本");
	}

	/* 阶段 1—3 只读：完整预检通过后才返回计划，以下才允许动活体。 */
	HotSqlPlan plan = HotSqlPlanner(matrices_, registry_).build(sql);
	const auto& muts = plan.mutations;
	const auto& delPlan = plan.deletions;
	auto& insPlan = plan.insertions;

	/* ---- 阶段 4 + 4.6：提交，删除按执行序分派 ----
	 * 执行序（2026-09-23 热更新起有两种，按批次是否含 UPDATE 分派）：
	 *  * 先插入后删除（既有次序；非 UPDATE 批次行为逐字保持）：插入段全程
	 *    可回滚，物理删除排**最后**（本函数 4.6 段）——「DELETE + INSERT 同批
	 *    且插入失败」时删除从未发生，整批回到原点；
	 *  * 先删后建（deleteFirst，批次含 UPDATE——改写行同名重建，注册表重名
	 *    即抛，必须先摘旧件再装新件）：删除的不可逆点提前到插入之前（此处），
	 *    插入段失败时新件全数回滚断电、已摘旧件**不复原**（destroy 是物理断电，
	 *    回滚 ≠ 复活器件），commitHotBatch 的 catch 块如实点名丢失的变量。收益
	 *    有二：新旧器件不再短暂并存抢同一物理器件；新行 MetaProvider 的行号在
	 *    创建时就已是终值，不会被随后的删行前移打乱。 */
	HotDeleteOutcome delOutcome;
	if (delPlan.deleteFirst) {
		delOutcome = executeHotDeletes(delPlan.dels, delPlan.daemonNameIndex);
	}
	const CommitOutcome committed = commitHotBatch(delPlan, insPlan, delOutcome);

	/* 4.6（非 UPDATE 批次的删除）：排在这里 = 全部可回滚工作之后——插入段失败
	 * 时删除从未发生，整批回到原点（「一旦开始删，前面新增的部分都已成功」，
	 * 不存在「新器件要回滚、旧器件已删掉」的混合状态）；UPDATE 批次已在提交前
	 * 执行过，此处跳过。 */
	if (!delPlan.deleteFirst) {
		delOutcome = executeHotDeletes(delPlan.dels, delPlan.daemonNameIndex);
	}

	/* ---- 阶段 4.7：整表撤下（DROP TABLE 空壳，不可逆）——位置约束与重锚定
	 * 的理由在实现处 ---- */
	const DropOutcome dropped = dropTableShells(muts);

	return summarizeHotOutcome(committed, muts, delOutcome, insPlan, dropped);
}

/* ---- 生效摘要（applySql 尾段，2026-10-01 拆出为纯拼装）----
 * REPL /sql 与 agent apply_sql 共用同一句话；热更新行数（G2 对账）：改写行
 * 的物理事实同时体现在「新增」与「删除」两个计数里（断电重建），本行是
 * 改写事实的独立记账——复盘时三个数能相互对上。 */
std::string RobotStruct::summarizeHotOutcome(const CommitOutcome& committed,
		const ScriptLoader::Mutations& muts, const HotDeleteOutcome& delOutcome,
		const HotSqlPlan::Insertions& insPlan, const DropOutcome& dropped) const {
	std::string msg = "已生效：新增硬件变量 " + std::to_string(committed.addedVars) + " 个";
	if (committed.addedTables > 0) { msg += "，新增表 " + std::to_string(committed.addedTables) + " 张"; }
	if (committed.addedRows > 0) { msg += "，既有表追加 " + std::to_string(committed.addedRows) + " 行"; }
	std::size_t updatedTotal = 0;
	for (const auto& [t, n] : muts.updatedRows) { (void)t; updatedTotal += n; }
	if (updatedTotal > 0) {
		msg += "，已热更新 " + std::to_string(updatedTotal) + " 行（断电重建）";
	}
	if (delOutcome.deletedVars > 0) {
		msg += "，已删除硬件变量 " + std::to_string(delOutcome.deletedVars) + " 个（";
		msg += joinNames(delOutcome.deletedNames);
		msg += "）";
		if (delOutcome.deletedRows > delOutcome.deletedVars) {
			msg += "、结构表删除 " + std::to_string(delOutcome.deletedRows) + " 行";
		}
	}
	if (!insPlan.actionMats.empty()) {
		std::size_t n = 0;
		for (const auto& m : insPlan.actionMats) { n += m.rows() > 0 ? m.rows() - 1 : 0; }
		msg += "，瞬态动作已执行 " + std::to_string(n) + " 行（未注册，已丢弃）";
	}
	if (dropped.count > 0) {
		msg += "，已撤下表 " + std::to_string(dropped.count) + " 张（";
		msg += joinNames(dropped.names);
		msg += "）";
	}
	return msg;
}

/* ==================== HotSqlPlanner：只读计划层 ====================
 * build / planHotDeletes / planHotInsertions / reconcileNewTableDels /
 * checkMergedView 的实现已整体迁至 hotsql_plan.cpp（2026-10-01 TU 拆分），
 * 阶段编号（1/1.5/2/2.4/3）的判据与理由注释随实现迁移。 */

/* ---- 阶段 4：提交 + 增量装配 + 上电（插入段任何一步失败 ⇒ 全回滚）----
 * deleteFirst 批次的物理删除已在 applySql（本函数之前）执行，其结果经
 * alreadyDeleted 传入——catch 的失败口径要据它点名已丢失的变量。
 * mark 取在（可能已发生的）删除之后：先删后建下 mark.seq 即删后规模，
 * 回滚只回滚插入段——已执行的物理删除不在任何回滚范围内。 */
RobotStruct::CommitOutcome RobotStruct::commitHotBatch(const HotSqlPlan::Deletes& delPlan,
		HotSqlPlan::Insertions& insPlan, const HotDeleteOutcome& alreadyDeleted) {
	CommitOutcome out;
	const HwRegistry::Mark mark = registry_.mark();
	const std::size_t metaMark = metaProviders_.size();
	const std::size_t daemonMark = daemonHolders_.size();
	std::vector<std::pair<std::size_t, std::size_t>> appended;	/* (矩阵下标, 追加行数) */
	std::size_t addedTables = 0;

	try {
		/* 4.1 既有表：先落行（矩阵是装配的输入），再按旧行数增量装配 */
		for (const auto& [idx, rows] : insPlan.appends) {
			mt::MatrixObject& live = matrices_[idx];
			const std::int64_t from = static_cast<std::int64_t>(live.rows());
			for (const auto& row : rows) {
				live.appendRow(row, static_cast<std::int64_t>(live.rows()));
			}
			appended.emplace_back(idx, rows.size());
			factory_.createHardVarsFrom(live, from);
		}
		/* 4.2 新表：整表入列后按 fromRow=1 装配 */
		for (auto& t : insPlan.newTables) {
			matrices_.push_back(std::move(t));
			++addedTables;
			factory_.createHardVarsFrom(matrices_.back(), 1);
		}
		/* 4.3 $ 引用存在性：只查本批次新增的 MetaProvider（此时批内变量已全部注册，
		 *     故批内前向引用合法；既有提供者启动期已校验） */
		for (std::size_t i = metaMark; i < metaProviders_.size(); ++i) {
			metaProviders_[i]->validate();
		}
		/* 4.4 上电：只 build 本批次新增的（声明序），与 active() 同查停止标志
		 *     ——多驱动逐个 build 期间可能有真机延时，Ctrl-C 必须能中止并回滚。 */
		const auto& seq = registry_.sequence();
		for (std::size_t i = mark.seq; i < seq.size(); ++i) {
			if (stopRequested()) {
				throw std::runtime_error("热插上电被中断（收到停止信号）：本批次变更正在回滚");
			}
			seq[i]->build();
		}
		/* 4.5 瞬态 Action：排在硬件之后执行（动作可引用本批次新建变量），
		 *     执行完即丢弃——不入 matrices_、不注册。执行失败同样回滚本批次硬件
		 *     （结构回到批次前状态），但动作已下发的物理副作用不回退。 */
		if (!insPlan.actionMats.empty()) { runner_.runAction(insPlan.actionMats); }
	} catch (...) {
		try {
			rollbackHotBatch(mark, metaMark, daemonMark, appended, addedTables);
		} catch (const std::exception& e) {
			/* 回滚本身失败已超出手头能修的范畴：如实留痕，但不掩盖原错误
			 * （原错误才是调用方要看的成因）。 */
			std::cerr << "Hot SQL rollback FAILED: " << e.what() << std::endl;
		}
		/* 先删后建的失败口径（区别于既有批次的「原样上抛」）：本批次含 UPDATE
		 * 且已摘除过旧件时，活体**已经动过**——插入段失败只是「新件未建成」，
		 * 旧件不会回来（重包装的理由见 rethrowWithLostVars 注释）。 */
		if (delPlan.deleteFirst && alreadyDeleted.deletedVars > 0) {
			rethrowWithLostVars(alreadyDeleted);
		}
		throw;
	}

	/* 「新增了几个变量」必须在**动手删之前**定下来：删除会从同一份注册表里摘
	 * 条目，用「事后总数 - 批次前总数」会把两边相互抵掉（同批删一插一时净增量
	 * 为 0，摘要就会谎报「新增 0 个」）。新增是「装配阶段登记了多少」这个
	 * 绝对值（此刻的注册表规模 = deleteFirst 批次的「删后 + 插后」，与删后
	 * mark.seq 相减同样得出纯新增数），删除是另一个维度，两者各自如实报数。 */
	const std::size_t varsAfterInsert = registry_.sequence().size();
	out.addedVars = varsAfterInsert - mark.seq;
	for (const auto& [idx, n] : appended) { (void)idx; out.addedRows += n; }
	out.addedTables = addedTables;
	return out;
}

/* 插入段失败的回滚（原 commitHotBatch 内 45 行 rollback lambda，2026-09-29
 * A3 拆出；四步语义逐字保持）：
 * ① 逆序断电：destroy 逐项容错（半装配/未 build 的项也要能安全 destroy，
 *    与 deactivate 同口径），单个失败不能中断整轮回滚。
 * ② 撤注册（含流引用快照还原）→ ③ 弹登记表 → ④ 恢复矩阵。 */
void RobotStruct::rollbackHotBatch(const HwRegistry::Mark& mark, std::size_t metaMark,
		std::size_t daemonMark,
		const std::vector<std::pair<std::size_t, std::size_t>>& appended,
		std::size_t addedTables) {
	const auto& seq = registry_.sequence();
	for (std::size_t i = seq.size(); i > mark.seq; --i) {
		try {
			seq[i - 1]->destroy();
		} catch (const std::exception& e) {
			std::cerr << "Hot SQL rollback warning: " << e.what() << std::endl;
		}
	}
	registry_.rollbackTo(mark);
	metaProviders_.resize(metaMark);
	daemonHolders_.resize(daemonMark);
	for (const auto& [idx, n] : appended) { matrices_[idx].popRows(n); }
	matrices_.resize(matrices_.size() - addedTables);
}

/* 先删后建批次插入段失败的重包装：本批次含 UPDATE 且已摘除过旧件时，活体
 * **已经动过**——插入段失败只是「新件未建成」，旧件不会回来。把成因包装成
 * runtime_error 并点名丢失的变量：
 *  - 不谎报：模型/操作者必须知道「哪些变量已经没了」；
 *  - 类型必须是 runtime_error 而非原样上抛——若成因本身是
 *    IllegalArgumentException（如装配期重名），apply_sql 的派发漏斗
 *    会按异常类型把它归类成「校验拒绝、活体一字未动」（paramError /
 *    executed=false），对本口径是谎报：删旧件是真实发生过的副作用。 */
void RobotStruct::rethrowWithLostVars(const HotDeleteOutcome& alreadyDeleted) {
	const std::string names = joinNames(alreadyDeleted.deletedNames);
	const std::string tail =
		"；注意：本批次含 UPDATE，其命中旧行已断电摘除且未能重建（"
		+ names + "）——硬件不会自动复原，请核对状态后按需要的值重新 INSERT";
	try {
		throw;
	} catch (const std::exception& e) {
		throw std::runtime_error(std::string(e.what()) + tail);
	} catch (...) {
		throw std::runtime_error(std::string(kNonStdExceptionNote) + tail);
	}
}

/* ==================== 阶段 4.7：整表撤下（DROP TABLE，不可逆）====================
 * 位置在 4.6 之后：DROP 的**全部数据行**已由 4.6 走完与 DELETE 完全相同的
 * 「逆序断电 → 擦登记表 → 擦结构行 → 撤注册」路径（DROP 就是把整表行全删
 * 再加一步摘表壳），故这里只剩**空表壳**要摘。
 *
 * 为什么表壳必须最后摘：4.6.4 用「矩阵地址 + 行号」定位被删行的 MetaProvider，
 * 而矩阵地址是 `&matrices_[mi]`——一旦先 erase 表壳，其后所有表的下标与地址
 * 都会变，4.6 的判据就全错位。故顺序是死的：先按行删（4.6），再摘壳（本节）。
 *
 * deque 中间 erase 的引用失效：MetaProvider::matrix_ 存的是指向 matrices_
 * 元素的指针；erase 会让其后元素引用失效。**必须先把存活提供者的表名抓下来
 * 再 erase**（表名唯一且是稳定身份），erase 后按名把每个提供者重锚定回新地址
 * ——否则后续 readSample/collect 会踩悬垂指针。这也正是 matrix.hpp 把矩阵
 * 容器从 vector 换成 deque 之外、又为整表撤下专门保留 rebase 的原因。 */
RobotStruct::DropOutcome RobotStruct::dropTableShells(const ScriptLoader::Mutations& muts) {
	DropOutcome out;
	if (muts.droppedTables.empty()) { return out; }
	/* ① 抓名字（erase 之前，此时 matrix_ 仍有效） */
	std::vector<std::pair<MetaProvider*, std::string>> provNames;
	provNames.reserve(metaProviders_.size());
	for (const auto& p : metaProviders_) {
		provNames.emplace_back(p.get(), p->matrix().getName());
	}
	/* ② 定位要摘的表壳（本批次 DROP 的表名 ∩ 活体 matrices_）。DROP 本批次
	 *     新建的表不在活体里（readSQL 已把它从影子容器整表擦除），此处自然
	 *     找不到——那不是错误，跳过即可。 */
	std::vector<std::size_t> shellIdx;
	for (std::size_t k = 0; k < matrices_.size(); ++k) {
		if (muts.droppedTables.count(trim(matrices_[k].getName())) != 0) {
			shellIdx.push_back(k);
			out.names.push_back(trim(matrices_[k].getName()));
		}
	}
	/* ③ 降序 erase（先删大下标，前面的下标不失效） */
	for (auto it = shellIdx.rbegin(); it != shellIdx.rend(); ++it) {
		matrices_.erase(matrices_.begin() + static_cast<std::ptrdiff_t>(*it));
		++out.count;
	}
	/* ④ 重锚定存活提供者：按名字找同名的矩阵地址。只在**确实摘过表壳**时做
	 *    ——「本批次 CREATE 后又 DROP」的新表不在活体里（shellIdx 为空），
	 *    此时没有元素被搬移，重锚定是空转。 */
	if (!shellIdx.empty()) {
		for (auto& [prov, nm] : provNames) {
			const mt::MatrixObject* m = mt::findByName(matrices_, nm);
			if (m != nullptr) { prov->rebase(*m); }
		}
	}
	return out;
}

/* ==================== 热删除的物理执行（原 applySql 阶段 4.6，不可逆）====================
 * 供两个执行序共用（谁先谁后由 applySql 按批次是否含 UPDATE 分派，见其阶段 4
 * 注释）：非 UPDATE 批次排在全部可回滚工作之后（插入失败 ⇒ 删除从未发生）；
 * UPDATE 批次（先删后建）排在插入之前（同名重建必须先摘旧件，插入失败 ⇒
 * 旧件已摘、新件回滚，applySql 的 catch 会点名丢失的变量）。
 *
 * 顺序：逆序断电 → 擦登记表 → 擦结构行 → 撤注册。每一步的理由：
 *  ① 逆声明序（表下标倒序、表内行号倒序）：销毁序 = 声明序倒序是既有资源
 *     不变式——总线变量的 fd 只在它自己的 destroy 里关闭，而挂在它上面的
 *     器件 destroy 要用同一个 fd 做去初始化，倒序保证总线最后关 fd。
 *     按行号倒序另有一条收益：被删行号之后创建的 MetaProvider 在先删后建
 *     模式下根本还不存在，在先插入后删除模式下则由 4.6.4① 的整条摘除兜住。
 *  ② 守护先行（requestStop + 屏障等待）：守护是唯一跨线程碰硬件的角色，
 *     必须在它的采样对象掉电之前退场，否则会去写一个已断电的器件
 *     （deactivate 的断电屏障是同一套理由，这里复用同样的共享预算手法）。
 *  ③ 先摘登记表再擦矩阵行：反过来会让 MetaProvider 短暂指向已不存在的行
 *     （popRows/removeRows 之后 row_ 越界即抛）。
 *  ④ 最后撤注册：registry_.removeBatch 在对象析构前撤所有权，故「名字查得到
 *     但器件已断电」的窗口不存在。
 *
 * 失败语义（与「全成功或全回滚」并列的第二套口径，如实告知而不假装回滚）：
 * 预检已在 applySql 的引用完整性/全结构判据里拦下全部可预判的错误，故这里
 * 失败只可能来自物理 destroy 本身（I2C 写失败、器件不响应…）。此时立即中止
 * 后续 destroy（继续硬闯只在别的器件上叠加失败），并如实报出「已删 N 个、
 * M 个未删」——结构矩阵与注册表保持**一致**（都只反映已成功删掉的部分），
 * 不留半删状态。
 *
 * hw 指针来自计划（plans）而非按名 find：先删后建模式下执行点在插入之后，
 * 按名 find 会命中新注册的同名变量、摘错对象——计划期取好指针是正确性前提
 * （见 applySql 阶段 1.5 的注释）。 */
RobotStruct::HotDeleteOutcome RobotStruct::executeHotDeletes(
		const std::vector<HotSqlPlan::Deletion>& plans,
		const std::map<std::string, std::size_t>& daemonNameIndex) {
	HotDeleteOutcome out;
	if (plans.empty()) { return out; }
	/* 4.6.0 逆声明序展平成「实际销毁顺序」清单 */
	struct Doomed {
		std::size_t matIdx = 0;
		std::size_t row = 0;
		std::string name;
		MatrixVars_Hardware* hw = nullptr;
	};
	std::vector<Doomed> order;
	for (const HotSqlPlan::Deletion& p : plans) {
		for (std::size_t k = 0; k < p.rows.size(); ++k) {
			order.push_back({p.matIdx, p.rows[k], p.names[k], p.hw[k]});
		}
	}
	std::sort(order.begin(), order.end(), [](const Doomed& a, const Doomed& b) {
		if (a.matIdx != b.matIdx) { return a.matIdx > b.matIdx; }	/* 表：倒序 */
		return a.row > b.row;					/* 表内：行号倒序 */
	});

	/* 4.6.1 待删守护：从原始行号定位登记槽（必须在矩阵被改写之前取出来
	 * ——槽是「哪一行是守护」的唯一索引，行没了就找不回来了）。 */
	std::vector<std::shared_ptr<DaemonRunner>> dyingHolders;
	for (const HotSqlPlan::Deletion& p : plans) {
		if (p.table != "Daemon") { continue; }
		for (std::size_t k = 0; k < p.names.size(); ++k) {
			auto slot = daemonNameIndex.find(p.names[k]);
			if (slot == daemonNameIndex.end() || slot->second >= daemonHolders_.size()) {
				throw IllegalArgumentException("热删除：守护 '" + p.names[k]
					+ "' 找不到对应的守护线程登记槽（内部不一致）");
			}
			dyingHolders.push_back(daemonHolders_[slot->second]);
		}
	}
	/* 4.6.2 守护退场屏障：与 deactivate() 同一套安全序列，单点在
	 * daemonStopBarrier（本文件顶部）——屏障之后才允许动硬件。 */
	daemonStopBarrier(dyingHolders, deactivateGraceMs_, "HotDelete",
		"the doomed hardware's destroy will race with them"
		" (their sampling targets are being removed)");
	if (stopRequested()) {
		throw std::runtime_error("热删除被中断（收到停止信号）：本批次的删除尚未开始");
	}

	/* 4.6.3 逆序断电（逐项容错并点名；失败即中止剩余项） */
	std::vector<std::string> failures;
	std::size_t attempted = 0;
	for (const Doomed& d : order) {
		if (stopRequested()) {
			failures.push_back("（收到停止信号，剩余 "
				+ std::to_string(order.size() - attempted) + " 个未删除）");
			break;
		}
		++attempted;
		try {
			d.hw->destroy();
		} catch (const std::exception& e) {
			failures.push_back(d.name + ": " + e.what());
			break;		/* 中止剩余项：继续硬闯只在别的器件上叠加失败 */
		}
		++out.deletedVars;
		out.deletedNames.push_back(d.name);
	}

	/* 4.6.4 结构/注册表的一致性收尾：只对**已成功断电**的部分落实。
	 * 未删成的行原样留在结构里（仍是活体的一部分，语义自洽）。 */
	if (out.deletedVars > 0) {
		std::set<MatrixVars_Hardware*> removed;
		std::vector<std::pair<std::size_t, std::vector<std::size_t>>> perTable;
		{
			std::map<std::size_t, std::vector<std::size_t>> byMat;
			for (std::size_t i = 0; i < order.size() && i < out.deletedVars; ++i) {
				byMat[order[i].matIdx].push_back(order[i].row);
				removed.insert(order[i].hw);
			}
			for (auto& [mi, rs] : byMat) { perTable.emplace_back(mi, std::move(rs)); }
		}
		applyHotDeleteConsistency(perTable, removed, dyingHolders, out);
	}

	if (!failures.empty()) {
		throw std::runtime_error(hotDeleteFailureMessage(order.size(), out.deletedVars,
			out.deletedNames, failures));
	}
	return out;
}

/* ---- 热删除的一致性收尾（原 executeHotDeletes 阶段 4.6.4，2026-09-26 批次 5 拆出）----
 * 只对**已成功断电**的部分落实「结构矩阵与注册表保持一致」；未删成的行原样留在
 * 结构里（仍是活体的一部分，语义自洽）。五步顺序是死的：
 *  ① 先摘指向被删行的 MetaProvider——判据用「矩阵 + 行号」，而行号此刻还是删除前
 *     的原始坐标，故必须先于 ①′/②（擦行会让坐标前移）。
 *  ①′ 重锚定存活提供者行号——② 的保序擦除使其后行前移，提供者 row() 必须同步。
 *  ② 擦结构行（保序，销毁序不变式不动）。
 *  ③ 守护登记表按保留项重建（不 resize：删中间项会让尾部前移）。
 *  ④ 最后撤注册：registry_.removeBatch 在对象析构前撤所有权，故「名字查得到但
 *     器件已断电」的窗口不存在。 */
void RobotStruct::applyHotDeleteConsistency(
		const std::vector<std::pair<std::size_t, std::vector<std::size_t>>>& perTable,
		const std::set<MatrixVars_Hardware*>& removed,
		const std::vector<std::shared_ptr<DaemonRunner>>& dyingHolders,
		HotDeleteOutcome& out) {
	/* ① 元数据提供者：指向被删行的整条摘除。判据是「矩阵 + 行号」而不是
	 * 引用相等——矩阵是 std::deque 的元素，地址稳定（这正是选 deque 的
	 * 原因），故该判据精确且不受行数变化影响；行号此刻还是删除前的
	 * 原始坐标，故必须先于 ①′/② 执行。 */
	std::set<std::pair<const mt::MatrixObject*, std::int64_t>> deadProvider;
	for (const auto& [mi, rs] : perTable) {
		for (std::size_t r : rs) {
			deadProvider.emplace(&matrices_[mi], static_cast<std::int64_t>(r));
		}
	}
	auto isDeadProvider = [&](const std::shared_ptr<MetaProvider>& p) {
		/* emplace 的显式构造：count({a, b}) 的括号初始化列表无法推导
		 * 到 set 的 value_type（pair 的转换构造函数不参与该重载）。 */
		return deadProvider.count(std::make_pair(&p->matrix(), p->row())) != 0;
	};
	metaProviders_.erase(std::remove_if(metaProviders_.begin(), metaProviders_.end(),
		isDeadProvider), metaProviders_.end());
	/* ①′ 存活提供者行号重锚定（2026-09-25 审计 A-4 收口）：② 的 removeRows
	 * 是保序擦除——同矩阵内行号大于被删行的行全部前移。提供者的 row()
	 * 是**后续批次** deadProvider 判据的坐标（上一步刚用过）与任何未来
	 * 惰性取行的下标，必须按「本矩阵被删行中小于本行行号的数量」同步
	 * 前移，否则下一批同表删除会摘错/漏摘提供者。原实现在这里留有已知
	 * 边界：混批（先插入后删除）新追加行的提供者以追加时行号创建，随
	 * 后的删行让其 row_ 过期——实际上不止混批，**任何**同表删行都会让
	 * 存活行的坐标过期（含先删后建的 UPDATE 批次里未改写的既有行），
	 * 本步骤一并收口。混批里追加行的 provider 已在 commitHotBatch 4.3
	 * 急切 seed（缓存以追加时坐标取到的是正确内容），此处只修坐标。 */
	for (const auto& [mi, rs] : perTable) {
		const mt::MatrixObject* mat = &matrices_[mi];
		for (const auto& p : metaProviders_) {
			if (p->matrixPtr() != mat) { continue; }
			std::int64_t shift = 0;
			for (std::size_t r : rs) {
				if (static_cast<std::int64_t>(r) < p->row()) { ++shift; }
			}
			if (shift > 0) { p->shiftRow(-shift); }
		}
	}
	/* ② 结构行（保序擦除，销毁序不变式不动） */
	for (const auto& [mi, rs] : perTable) {
		matrices_[mi].removeRows(rs);
		out.deletedRows += rs.size();
	}
	/* ③ 守护登记表：按保留项重建（不做 resize——删中间项会让尾部前移，
	 * resize 只能表达「砍掉尾巴」）。 */
	std::set<const DaemonRunner*> deadRunners;
	for (const auto& h : dyingHolders) { deadRunners.insert(h.get()); }
	std::vector<std::shared_ptr<DaemonRunner>> kept;
	kept.reserve(daemonHolders_.size());
	for (const auto& h : daemonHolders_) {
		if (deadRunners.count(h.get()) == 0) { kept.push_back(h); }
	}
	daemonHolders_ = std::move(kept);
	/* ④ 撤注册（所有权 + 名字索引 + 流引用） */
	registry_.removeBatch(removed);
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

/* ---- metaProviders_ 与活体结构的一致性自检（测试用，见 robot.hpp 契约）----
 * 期望集的表分派与 HwFactory::createHardVarsFrom 同集合：Alias/Vars/Action/
 * Daemon 表不产生 MetaProvider（标量走 registerScalarMatrix、守护走
 * MtVars_Daemon::Factory 且装配期即读全行），其余表每个数据行一个提供者。 */
bool RobotStruct::metaProvidersMatchLiveRows() const {
	std::multiset<std::pair<const mt::MatrixObject*, std::int64_t>> expect;
	for (const auto& m : matrices_) {
		const std::string& t = m.getName();
		if (t == "Alias" || t == "Vars" || t == "Action" || t == "Daemon") { continue; }
		for (std::int64_t r = 1; r < static_cast<std::int64_t>(m.rows()); ++r) {
			expect.emplace(&m, r);
		}
	}
	std::multiset<std::pair<const mt::MatrixObject*, std::int64_t>> got;
	for (const auto& p : metaProviders_) { got.emplace(p->matrixPtr(), p->row()); }
	return expect == got;
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
	 * 引用**。容器为 std::deque（2026-09-23 热插表），append 不再使元素引用失效
	 * ——这正是热插表能在 active() 之后追加的构造基础。但**本函数**仍须早于
	 * active()：它注入的是「LLM 协议驱动/摄像头驱动」，而 active() 之后的插入
	 * 属于热插表事务（RobotStruct::applySql），走的是另一条带全量校验与回滚的
	 * 路径（驱动模块存在性、重名、单消费者判据、$ 引用校验都在那里做）。此处
	 * 保留断言：绕过事务直接 append 会跳过那些校验，装配出半成品。 */
	if (!metaProviders_.empty()) {
		throw IllegalArgumentException("injectLuaDriver must be called before active():"
			" use the hot-SQL path (applySql) to add tables to a running robot");
	}

	std::string sql = "CREATE TABLE " + driverTable + " (name String);\n"
		+ "INSERT INTO " + driverTable + " (name) VALUES ('" + varName + "');\n";
	MemoryReader reader(std::move(sql));
	std::deque<mt::MatrixObject> extra;
	ScriptLoader::readSQL(reader, extra);
	for (auto& m : extra) { matrices_.push_back(std::move(m)); }
}

} // namespace bot
