/* robot.cpp —— RobotStruct 编排门面：生命周期 + 门面装配
 * 原单文件五类（HwRegistry/MetaProvider/ScriptLoader+HwFactory/ActionRunner/
 * RobotStruct）已拆分：hw_registry.cpp / meta_provider.cpp / hw_factory.cpp /
 * action_runner.cpp；2026-10-01 起 HotSqlPlanner 只读计划层也在自己的 TU
 * （hotsql_plan.cpp）；2026-10-03 阶段 4 起热插提交阶段机（commitHotBatch /
 * executeHotDeletes / dropTableShells）迁到 hotsql_service.cpp——本 TU 只剩
 * 装配流程、生命周期编排与 run()/applySql 两个转交入口。 */

#include "robot.hpp"

/* 2.2：robot.hpp 不再传导 Lua 头——本 TU 仍需要 @Include 展开 */
#include "mtinclude.hpp"

#include <iostream>
#include <utility>

#include "util_env.hpp"
#include "util_str.hpp"		/* trim（列名/表名归一） */

namespace bot {

namespace {

/* 守护退场屏障已迁至守护域门面（DaemonRegistry::stopBarrier，2026-10-03 阶段 4）
 * ——deactivate（断电前）与热删除（摘件前）共用同一套安全序列，单点在
 * daemon_registry.cpp。 */

/* 把列头行 + [rowBegin, rowEnd) 的数据行复制成一张独立矩阵（copyRows）——
 * 影子构建与瞬态 Action 提取共用——已随 HotSqlPlanner 迁至 hotsql_plan.cpp
 * （2026-10-01），本 TU 不再需要。 */

/* 热删除中途失败的报错文本（hotDeleteFailureMessage）已随热删除执行迁至
 * hotsql_service.cpp（2026-10-03 阶段 4）。 */

/* 阶段 2 的表级/逐行只读校验（validateNewTableDecl / validateHotNewRow）已随
 * HotSqlPlanner 迁至 hotsql_plan.cpp（2026-10-01）。 */

} // namespace

/* ==================== RobotStruct：编排门面 ==================== */

/* ---- 构建硬件：两阶段（build 失败上抛，active 由 finally 清理）---- */
void RobotStruct::active() {
	/* 状态机判据：只有 Loaded（尚未上电）/ Inactive（已干净断电）可以上电。
	 * Active 重复上电会让 registry_ 里既有变量二次 build（真机事务重放），
	 * Deactivating 只存在于 deactivate 调用栈内，都拒绝。 */
	if (state_ == LifecycleState::Active || state_ == LifecycleState::Deactivating) {
		throw IllegalArgumentException("机器人生命周期状态不允许 active()：当前状态非 Loaded/Inactive");
	}
	/* 先标「已上电」再装配：createHardVars / build 中途抛出时，析构仍会补一次
	 * deactivate 收拾已经 build 成功的那部分（deactivate 逐项容错，半成品可安全跑）。 */
	state_ = LifecycleState::Active;
	deactivated_ = false;
	factory_.createHardVars(structures_.all());
	/* F-05：$ 引用解析本是惰性的（首次 collect 时）——指向未定义变量会在
	 * 驱动 build 的真机事务中途才报错；装配完成后、任何 build 之前统一
	 * 校验存在性（此时全部变量已注册，前向引用合法）。 */
	metas_.validateAll();
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
	/* 状态机判据：Loaded（从未上电，无事可做）与 Inactive（已断电）直接返回，
	 * deactivate 因此天然幂等——显式重复调用与析构兜底都不会二次 destroy
	 * （对真器件不是无害的：已关断的器件再写寄存器可能落到无效状态机上）。 */
	if (state_ == LifecycleState::Loaded || state_ == LifecycleState::Inactive) { return; }
	state_ = LifecycleState::Deactivating;
	/* ---- 断电屏障 ----统一等到守护线程退场，然后才动硬件；序列理由、二段
	 * 宽限与告警文案都在守护域单点（DaemonRegistry::stopBarrier）。 */
	daemons_.stopBarrier(deactivateGraceMs_, "Deactivate",
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
	state_ = LifecycleState::Inactive;
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
	/* 生命周期判据（2026-10-03 阶段 3）：只拒绝**曾上电后又断电**的 Inactive
	 * ——此时注册表里的器件对象已 destroy，派发会落到死亡句柄。
	 * Loaded（尚未上电）刻意放行：run() 的前半段（动作矩阵解析、@Alias/@Vars
	 * 注册、派发前的列名/设备名校验）本就不申请真机资源，是既有的「离线预演」
	 * 能力（见 test_robot.cpp 顶注）；把整段锁在 Active 之后会改变这一可测契约。
	 * Deactivating 是本进程内不可观测的瞬态（只存在于 deactivate 调用栈内），
	 * 不在此判据之列。 */
	if (state_ == LifecycleState::Inactive) {
		throw IllegalArgumentException("机器人已断电，不能再执行动作：请先重新 active()");
	}
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

/* ---- 热插表：结构 SQL 带电生效（2026-09-23）----
 * 只判生命周期前置条件（须已上电常驻），其余整体转交热插提交服务
 * （hotsql_service.cpp，2026-10-03 阶段 4）：解析到影子容器 → 只读全量校验 →
 * 提交装配上电 → 热删除 → 整表撤下 → 生效摘要的阶段机与全部判据/理由注释
 * 都在那里。公开契约见 robot.hpp。 */
std::string RobotStruct::applySql(const std::string& sql) {
	if (state_ != LifecycleState::Active) {
		throw IllegalArgumentException("热插表需要机器人已上电常驻（active() 之后）；"
			"未上电时请把语句写进结构脚本");
	}
	return hotSql_.apply(sql, deactivateGraceMs_);
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

	/* 顺序不变式（B0-5）：MetaProvider 持有的 matrix_ 是对结构矩阵 **元素的
	 * 引用**。容器为 std::deque（2026-09-23 热插表），append 不再使元素引用失效
	 * ——这正是热插表能在 active() 之后追加的构造基础。但**本函数**仍须早于
	 * active()：它注入的是「LLM 协议驱动/摄像头驱动」，而 active() 之后的插入
	 * 属于热插表事务（RobotStruct::applySql），走的是另一条带全量校验与回滚的
	 * 路径（驱动模块存在性、重名、单消费者判据、$ 引用校验都在那里做）。此处
	 * 保留断言：绕过事务直接 append 会跳过那些校验，装配出半成品。 */
	if (!metas_.empty()) {
		throw IllegalArgumentException("injectLuaDriver must be called before active():"
			" use the hot-SQL path (applySql) to add tables to a running robot");
	}

	std::string sql = "CREATE TABLE " + driverTable + " (name String);\n"
		+ "INSERT INTO " + driverTable + " (name) VALUES ('" + varName + "');\n";
	MemoryReader reader(std::move(sql));
	std::deque<mt::MatrixObject> extra;
	ScriptLoader::readSQL(reader, extra);
	for (auto& m : extra) { structures_.append(std::move(m)); }
}

} // namespace bot