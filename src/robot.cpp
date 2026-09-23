/* robot.cpp —— RobotStruct 编排门面（重构 2026-09-20：按类拆分 TU）
 * 原单文件五类（HwRegistry/MetaProvider/ScriptLoader+HwFactory/ActionRunner/
 * RobotStruct）已拆分：hw_registry.cpp / meta_provider.cpp / hw_factory.cpp /
 * action_runner.cpp；本 TU 只剩装配流程 + 生命周期编排。 */

#include "robot.hpp"

/* 2.2：robot.hpp 不再传导 Lua 头——本 TU 编排守护生命周期，显式引入 */
#include "daemonvar.hpp"
#include "mtinclude.hpp"

/* 流引用列判定（controller/trigger 及编号列）——热删除的引用完整性预检要用。
 * 走单点是必须的：controller2/trigger10 这类编号列也算引用列，自带一份判据
 * 就会漏掉它们而留下悬垂指针。 */
#include "flowcolumn.hpp"

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
	/* ---- 阶段 0：前置条件 ---- */
	if (deactivated_) {
		throw IllegalArgumentException("热插表需要机器人已上电常驻（active() 之后）；"
			"未上电时请把语句写进结构脚本");
	}

	/* ---- 阶段 1：解析到影子容器（活体一字未动）----
	 * 影子必须**逐字复制既有表的全部行**，不能只填列头（2026-09-23 热删除）：
	 * DELETE 要在影子上做行匹配（WHERE 可以引用任何既有列的任何值），只看得到
	 * 列头就一条也匹配不上；INSERT 的「按列名落位 / 漏列报错」也需要同一条
	 * 影子路径（C3 防线）。原实现只填列头是因为当时只有 INSERT——追加行无需
	 * 看到既有行，代价是 DELETE 无法在此路径上表达。
	 * 代价：影子是既有结构的**全量拷贝**（百行级，且只在一次热插批次期间存在）。 */
	std::deque<mt::MatrixObject> staged;
	for (const auto& m : matrices_) {
		mt::MatrixBuilder b;
		b.name(m.getName());
		for (std::size_t c = 0; c < m.cols(); ++c) { b.item(m(0, c)); }
		b.line(0);
		for (std::size_t r = 1; r < m.rows(); ++r) {
			for (std::size_t c = 0; c < m.cols(); ++c) { b.item(m(r, c)); }
			b.line(static_cast<std::int64_t>(r));
		}
		staged.push_back(b.take());
	}
	const std::size_t liveCount = matrices_.size();

	ScriptLoader::Mutations muts;
	{
		MemoryReader reader(sql);
		ScriptLoader::readSQL(reader, staged, /* allowDelete= */ true, &muts);
	}

	auto rowOf = [](const mt::MatrixObject& m, std::size_t r) {
		std::vector<std::string> row;
		row.reserve(m.cols());
		for (std::size_t c = 0; c < m.cols(); ++c) { row.push_back(m(r, c)); }
		return row;
	};

	/* 注意：既有表的影子**已经**反映了删除（readSQL 直接在它上面擦行），
	 * 不要在这里再擦一遍——那会是第二次应用同一批删除，而行号已经前移过。 */

	/* ---- 阶段 1.5：备删计划（纯只读；原阶段 2.5 前移，2026-09-23 热更新）----
	 * 前移的原因都在「先删后建」：
	 *  ① doomedNames 要供阶段 2 的新行重名预检做豁免——UPDATE 同名重建时，
	 *     旧名此刻仍挂在注册表上（物理摘除发生在后面），不豁免会把「改写」
	 *     误判成「重名」而拒绝；
	 *  ② hw 指针必须在任何插入之前取好：先建后拆下按名 find 会命中新注册的
	 *     同名变量、摘错对象。
	 * 批次新建表的删除对账（popRows）依赖 newTables——那是阶段 2 的产物，
	 * 只能延后（newTableDels 收集，阶段 2.4 处理）。 */
	const bool deleteFirst = !muts.updatedRows.empty();
	std::set<std::string> doomedNames;
	std::vector<HotDelPlan> dels;
	/* 守护名 → daemonHolders_ 下标：见 executeHotDeletes 的守护退场屏障。 */
	std::map<std::string, std::size_t> daemonNameIndex;
	for (std::size_t k = 0; k < matrices_.size(); ++k) {
		if (trim(matrices_[k].getName()) != "Daemon") { continue; }
		const std::int64_t idxN = matrices_[k].indexOfCol(0, "name");
		if (idxN < 0) { continue; }
		for (std::size_t r = 1; r < matrices_[k].rows(); ++r) {
			const std::string dn = trim(matrices_[k](r, static_cast<std::size_t>(idxN)));
			if (!dn.empty()) { daemonNameIndex[dn] = daemonNameIndex.size(); }
		}
	}
	std::vector<std::pair<std::string, std::vector<std::size_t>>> newTableDels;
	for (const auto& [tname, rows] : muts.deletedRows) {
		std::size_t si = liveCount;
		for (std::size_t k = 0; k < liveCount; ++k) {
			if (trim(matrices_[k].getName()) == tname) { si = k; break; }
		}
		if (si == liveCount) {
			/* 表不在活体结构里 ⇒ 它是**本批次 CREATE 出来的**：撤销范围为空且必须
			 * 为空（这些行从未装配、从未注册、从未上电，没有硬件可摘、没有断电
			 * 可做）。但影子上的删除效果必须落到待提交的新表上，否则新表会带着
			 * 「已被删掉的行」一起装配，凭空长出一个器件。
			 *
			 * 这里**不能**按原行号再删一遍：影子（= newTables 里的那张表）已经由
			 * readSQL 擦过行，行号已前移，二次应用会越界或删错行。
			 * 删除只会命中尾部的新增行、新增行只可能追加在尾部，故「影子比
			 * 原始多出的行数」就是必须从尾部丢掉的行数——用 popRows 精确表达，
			 * 不依赖任何行号。newTables 是阶段 2 的产物，对账延后到阶段 2.4。 */
			newTableDels.emplace_back(tname, rows);
			continue;
		}
		HotDelPlan p;
		p.matIdx = si;
		p.table = tname;
		p.rows = rows;
		const std::int64_t idxName = matrices_[si].indexOfCol(0, "name");
		if (idxName < 0) {
			throw IllegalArgumentException("热删除：表 '" + tname + "' 没有 'name' 列，"
				"无法定位要摘除的硬件变量");
		}
		for (std::size_t r : rows) {
			if (r >= matrices_[si].rows()) {
				throw IllegalArgumentException("热删除：表 '" + tname + "' 的第 "
					+ std::to_string(r) + " 行不存在（结构可能已被改动）");
			}
			const std::string v = trim(matrices_[si](r, static_cast<std::size_t>(idxName)));
			p.names.push_back(v);
			doomedNames.insert(v);
			/* 计划期取好对象指针（前移的理由②）：执行期（先删后建模式下已在
			 * 插入之后）按名 find 会命中新注册的同名变量。指针判空的报错口径
			 * 与旧实现逐字一致。 */
			MatrixVars_Hardware* hw = registry_.find(v);
			if (hw == nullptr) {
				/* 表里有行但注册表里没有该变量＝结构与装配脱节（例如启动期装配
				 * 就失败过、或该行从未成功 build）。删不掉不存在的变量，如实报错
				 * 而不是假装删过。 */
				throw IllegalArgumentException("热删除：表 '" + tname + "' 的行 '"
					+ v + "' 在硬件注册表里不存在（结构与该机器人的装配结果已脱节）");
			}
			p.hw.push_back(hw);
		}
		dels.push_back(std::move(p));
	}

	/* ---- 阶段 2：建计划 + 只读全量校验（不触碰活体、不申请任何资源）---- */
	/* 影子下标 [0, liveCount) 与 matrices_ 一一对应（影子就是按它顺序填的）。 */
	std::vector<std::pair<std::size_t, std::vector<std::vector<std::string>>>> appends;
	std::vector<mt::MatrixObject> newTables;	/* 本批次新建的硬件表 */
	std::vector<mt::MatrixObject> actionMats;	/* 瞬态 Action（新表或既有表的新行） */
	std::set<std::string> batchNames;		/* 批内变量名集合（批内重名早报） */

	for (std::size_t si = 0; si < staged.size(); ++si) {
		const bool isShadow = si < liveCount;
		const mt::MatrixObject& st = staged[si];
		const std::string tname = trim(st.getName());
		const bool isAction = (tname == "Action");

		if (!isShadow) {
			for (std::size_t sj = 0; sj < si; ++sj) {
				if (trim(staged[sj].getName()) == tname) {
					throw IllegalArgumentException("热插表：表 '" + tname + "' 已存在"
						"（结构里既有同名表，或本批次内重复 CREATE）");
				}
			}
			/* 表名决定 modules/<表名>.lua 的解析路径（createLuaVars 的唯一分派依据），
			 * 而热插语句来自不可信输入（REPL / 模型）——含 '/' 或 ".." 的表名会把
			 * 模块查找指到任意文件。启动期脚本是可信文件，故只在热插这条路上拦。 */
			if (!isSafeIdent(tname)) {
				throw IllegalArgumentException("热插表：表名 '" + tname + "' 不是安全标识符"
					"（字母/数字/下划线，且以字母或下划线开头）");
			}
			if (!isAction) {
				/* 列名进 Daemon 配置串（`列名=值;`）与元数据键：含 ';'/'=' 的列名
				 * 能在配置串里造出额外字段（等价于改写守护的 driver/proto）。
				 * Action 表列名是 <设备>.<参数>，天然含 '.'，不在此列。 */
				for (std::size_t c = 0; c < st.cols(); ++c) {
					if (!isSafeIdent(st(0, c))) {
						throw IllegalArgumentException("热插表：表 '" + tname + "' 的列名 '"
							+ st(0, c) + "' 不是安全标识符");
					}
				}
			}
			/* 驱动模块存在性：启动期这条缺失要到 build 才以 loadfile 失败暴露，
			 * 热插路径提前折成一句点名表名的人话（判据与 createLuaVars 同一来源）。 */
			if (!isAction && tname != "Alias" && tname != "Vars" && tname != "Daemon") {
				if (!findExistingModule(modulePaths(), tname + ".lua")) {
					throw IllegalArgumentException("热插表：表 '" + tname
						+ "' 需要驱动模块 modules/" + tname + ".lua，模块路径下找不到它");
				}
			}
		}

		/* insCount = 本批次插入到这张表的行数（readSQL 回报的权威计数）。 */
		const std::size_t insCount = [&]() {
			auto it = muts.insertedRows.find(tname);
			return it == muts.insertedRows.end() ? std::size_t(0) : it->second;
		}();
		/* 本批次新增行的区间：**插入的行永远在尾部连续一块**（INSERT 只追加、
		 * 删除只前移它前面的行），故这块 = [st.rows() - insCount, st.rows())。
		 * 朴素做法「用解析前行数当起点」在同一批既有 INSERT 又有 DELETE 落在
		 * 这张表上时会错：删除让尾部的插入块整体前移，旧起点就越过了尾端
		 * （表现为新增行被整批漏掉，甚至行号越界）。insCount 是本批次插入行数
		 * 的权威来源，与行号无关，故不受删除影响。
		 * 非影子表（本批次新建）整张都是新增，直接取 [1, rows)。 */
		const std::size_t newFrom = isShadow
			? (st.rows() >= insCount ? st.rows() - insCount : 1) : 1;
		const std::size_t newTo = st.rows();
		/* 既有表且本批次既没插行也没删行 → 无需进入逐行校验与建计划 */
		if (isShadow && muts.deletedRows.count(tname) == 0 && insCount == 0) { continue; }

		const std::int64_t idxName = st.indexOfCol(0, "name");
		for (std::size_t r = newFrom; r < newTo; ++r) {
			if (!isAction) {
				if (idxName < 0) {
					throw IllegalArgumentException("热插表：表 '" + tname
						+ "' 没有 'name' 列，无法为新增行命名硬件变量");
				}
				const std::string v = trim(st(r, static_cast<std::size_t>(idxName)));
				if (v.empty()) {
					throw IllegalArgumentException("热插表：表 '" + tname + "' 第 "
						+ std::to_string(r) + " 行的 name 列为空");
				}
					/* 重名判据的 doomed 豁免（先删后建模式）：UPDATE 同名重建的行
					 * 复用旧名，而旧名此刻仍挂在注册表上（本批次的物理摘除排在
					 * 插入之前执行）——不豁免会把「改写」误判成「重名」。豁免只在
					 * deleteFirst 下给：普通批次（先插入后删除）里「删 A 再插 A」
					 * 仍然拒绝，保持既有行为与报错位置逐字不变。改名（SET name）
					 * 到仍存活的名字不豁免，照常拒绝。 */
					if ((registry_.contains(v) && !(deleteFirst && doomedNames.count(v) != 0))
							|| !batchNames.insert(v).second) {
						throw IllegalArgumentException("热插表：硬件变量名 '" + v + "' 重复"
							"（结构里已有同名变量，或本批次内重复）");
					}
			}
			if (tname == "Daemon") {
				const std::int64_t idxChunk = st.indexOfCol(0, "chunk");
				if (idxChunk < 0) {
					throw IllegalArgumentException("热插表：Daemon 表没有 'chunk' 列");
				}
				const std::string chunk = trim(st(r, static_cast<std::size_t>(idxChunk)));
				/* '-' = 继承上一行的 chunk（来自可信的既有行或本批次已校验的行），跳过。
				 * 真实值必须是模块根下存在的相对路径：resolveDaemonChunk 允许绝对路径
				 * （启动期脚本可信，不拦），而热插的语句不可信——放行即等于让模型
				 * 指定任意 Lua 文件当守护 chunk 加载。 */
				if (chunk != "-") {
					if (checkRelPath(chunk) != RelPathCheck::OK
							|| !findExistingModule(modulePaths(), chunk)) {
						throw IllegalArgumentException("热插表：Daemon 表 '" + tname + "' 第 "
							+ std::to_string(r) + " 行的 chunk '" + chunk
							+ "' 必须是模块路径下存在的相对路径（热插不允许绝对路径/目录穿越）");
					}
				}
			}
		}

		if (isAction) {
			/* Action 瞬态：保留「列头 + 本批次新增行」成一张独立矩阵（不入活体）。 */
			if (newTo > newFrom) {
				mt::MatrixBuilder ab;
				ab.name(st.getName());
				for (std::size_t c = 0; c < st.cols(); ++c) { ab.item(st(0, c)); }
				ab.line(0);
				for (std::size_t r = newFrom; r < newTo; ++r) {
					for (std::size_t c = 0; c < st.cols(); ++c) { ab.item(st(r, c)); }
					ab.line(static_cast<std::int64_t>(r));
				}
				actionMats.push_back(ab.take());
			}
		} else if (isShadow) {
			if (newTo > newFrom) {
				std::vector<std::vector<std::string>> rows;
				for (std::size_t r = newFrom; r < newTo; ++r) { rows.push_back(rowOf(st, r)); }
				appends.emplace_back(si, std::move(rows));
			}
		} else {
			/* 本批次新建的表：直接取影子即可——影子已经带着本批次的插入与删除
			 * （readSQL 就在这个容器上改），故「先 CREATE 再 INSERT 再 DELETE」
			 * 的新表到这里已经是删干净的样子，不会凭空长出一个已删掉的器件。
			 * 切勿在这里再应用一次删除：行号已随擦除前移，二次应用会越界或删错行。 */
			newTables.push_back(st);
		}
	}

	/* ---- 阶段 2.4：批次新建表的删除对账（原阶段 2.5 的 newTable 分支，2026-09-23
	 * 热更新随计划前移拆出）----
	 * 计划本体（dels / doomedNames / hw 指针 / daemonNameIndex）已前移到阶段 1.5
	 * （见其注释）；本段只处理「表不在活体里 ⇒ 本批次 CREATE 出来的」的删除——
	 * 撤销范围为空且必须为空（这些行从未装配、从未注册、从未上电，没有硬件可
	 * 摘、没有断电可做），但影子上的删除效果必须落到待提交的新表上，否则新表
	 * 会带着「已被删掉的行」一起装配，凭空长出一个器件。
	 *
	 * 这里**不能**按原行号再删一遍：影子（= newTables 里的那张表）已经由
	 * readSQL 擦过行，行号已前移，二次应用会越界或删错行。删除只会命中尾部的
	 * 新增行、新增行只可能追加在尾部，故「影子比原始多出的行数」就是必须从
	 * 尾部丢掉的行数——用 popRows 精确表达，不依赖任何行号。对账要读 newTables
	 * （阶段 2 的产物），故留在原位。 */
	for (const auto& [tname, rows] : newTableDels) {
		mt::MatrixObject* nt = mt::findByName(newTables, tname);
		if (nt == nullptr) { continue; }	/* 该新表随后又被 DROP：影子已整表擦除 */
		for (const auto& st : staged) {
			if (trim(st.getName()) != tname) { continue; }
			const std::size_t appended = st.rows() > 0 ? st.rows() - 1 : 0;
			const std::size_t keep = rows.size() >= appended ? 0 : appended - rows.size();
			if (appended > keep) { nt->popRows(appended - keep); }
			break;
		}
	}

	/* 空表 DROP（表壳动了、行没动）也算变更：判据必须包含 droppedTables，
	 * 否则「DROP 一张本来就没有数据行的表」会被误判成「本批次什么都没做」。 */
	if (appends.empty() && newTables.empty() && actionMats.empty() && dels.empty()
			&& muts.droppedTables.empty()) {
		throw IllegalArgumentException("热插表：本批次没有任何 CREATE/INSERT/DELETE/DROP/UPDATE 变更"
			"（SELECT 是只读查询，请用 query_sql / REPL 的 /sql）");
	}

	/* ---- 阶段 3：全结构判据 + 引用完整性（对「提交之后的视图」，2026-09-23
	 *    热更新起两道判据同在合并视图上跑；原阶段 2.6 的引用扫描并入此处）----
	 * 消费判据看的是整张结构（新行可能与任意既有行抢同一器件），故必须合并
	 * 视图后判定；合并视图只为这两条判据而生，随即丢弃。
	 * 热删除/热更新同样要走这条：删掉一个 I2C 总线变量后，某个守护可能仍在用
	 * 同一个 path 采样（守护的 path 是**字面路径**，不经 $ 引用，故下面的引用
	 * 完整性检查看不见它）——checkSingleConsumer 恰好把两侧都归一成
	 * (路径, 地址) 键，对变更后的视图复跑即能拦下这种物理耦合。
	 *
	 * 引用完整性（物理不可逆的唯一预检防线；destroy() 是物理断电，删了就没有
	 * 回滚可言——回滚 ≠ 复活器件）：合并视图里若有任何行引用 doomed 变量，
	 * 删除/改写会留下悬垂裸指针。这些引用在框架里是终身缓存——MtVars_Lua::
	 * ctrlPtrs_/trigPtrs_（首用缓存）与 MetaRef::target（seed 期锁定），且都
	 * 注释写着「指针终身有效」正是因为 pool_ 只增不减。热删除第一次打破那条
	 * 不变式，故必须在这里把「不留悬垂」重新建立为构造保证。
	 *   判据分三类，覆盖结构脚本里**全部**能指向另一个变量的写法：
	 *     a) 流列 controller/trigger 及其编号列 —— 走 isFlowColumn 单点，
	 *        故 controller2/trigger10 也算，漏了就是悬垂指针；
	 *     b) 元数据列的字面 $name 引用 —— seed 期锁定 MetaRef::target 的
	 *        那一类（跨表，如 Monitor 的 src='$sim0'）；
	 *     c) 元数据列的 $(...) 表达式 —— 标识符经注册表按名求值，
	 *        目标消失即运行期报错（不是悬垂，但同样是结构已坏）。
	 * 扫描面 = 合并视图（= 提交之后的结构）：dying 行已擦除——引用方自己也被
	 * 删掉时，引用随行一起消失，不构成悬垂；本批次新增/改写行**在列**（2026-09-23
	 * 热更新扩面）：流引用是首用缓存的懒解析，不在装配期爆出来，INSERT/UPDATE
	 * 的新行引用同批被删变量若不在预检里拦下，就是一颗运行期才炸（或更糟：
	 * 静默指向错对象）的雷——对先删后建的 UPDATE 批次，那还会落在「旧件已摘、
	 * 新件装配失败」的不可复原窗口里。 */
	{
		std::deque<mt::MatrixObject> merged = matrices_;
		/* 先落实删除（擦行），再追加新行——顺序与提交阶段保持一致，
		 * 否则「批内先 INSERT 到既有表、再 DELETE 掉它」的净效果会被算错。 */
		for (const HotDelPlan& p : dels) { merged[p.matIdx].removeRows(p.rows); }
		for (const auto& [idx, rows] : appends) {
			for (const auto& row : rows) { merged[idx].appendRow(row, 0); }
		}
		for (const auto& t : newTables) { merged.push_back(t); }

		if (!dels.empty()) {
			std::vector<std::string> refErrors;
			for (const mt::MatrixObject& m : merged) {
				if (m.rows() == 0) { continue; }
				const std::string tname = trim(m.getName());
				for (std::size_t r = 1; r < m.rows(); ++r) {
					for (std::size_t c = 0; c < m.cols(); ++c) {
						const std::string& colName = m(0, c);
						const std::string val = m.inherited(r, c);
						if (val.empty() || val == "-") { continue; }
						const std::string where = "表 '" + tname + "' 第 "
							+ std::to_string(r) + " 行的列 '" + colName + "'";
						bool isFlow = isFlowColumn(colName);
						if (isFlow && doomedNames.count(val) != 0) {
							refErrors.push_back(where + " 把 '" + val
								+ "' 引用为 controller/trigger");
							continue;
						}
						if (!isFlow && val[0] == '$') {
							if (auto body = exprBody(val)) {
								std::set<std::string> vars;
								try {
									Expression::parse(*body).collectVars(vars);
								} catch (const std::exception&) {
									continue;	/* 语法坏掉的表达式由装配期既有判据报错 */
								}
								for (const std::string& nm : vars) {
									if (doomedNames.count(nm) != 0) {
										refErrors.push_back(where + " 的表达式 '" + val
											+ "' 引用了 '" + nm + "'");
									}
								}
							} else if (doomedNames.count(val.substr(1)) != 0) {
								refErrors.push_back(where + " 以 '" + val + "' 引用了 '"
									+ val.substr(1) + "'");
							}
						}
					}
				}
			}
			if (!refErrors.empty()) {
				std::string msg = "热删除被拒绝：以下存活结构仍引用待删变量，删除会留下指向"
					"已释放器件的引用（先删引用方，或把这些行一起删）：";
				for (const std::string& e : refErrors) { msg += "\n  - " + e; }
				throw IllegalArgumentException(msg);
			}
		}

		factory_.precheckStructure(merged);
	}

	/* ---- 阶段 4：提交 + 增量装配 + 上电（插入段任何一步失败 ⇒ 全回滚）----
	 * 执行序（2026-09-23 热更新起有两种，按批次是否含 UPDATE 分派）：
	 *  * 先插入后删除（既有次序；非 UPDATE 批次行为逐字保持）：插入段全程
	 *    可回滚，物理删除排最后——「DELETE + INSERT 同批且插入失败」时删除
	 *    从未发生，整批回到原点；
	 *  * 先删后建（deleteFirst，批次含 UPDATE——改写行同名重建，注册表重名
	 *    即抛，必须先摘旧件再装新件）：删除的不可逆点提前到插入之前，插入段
	 *    失败时新件全数回滚断电、已摘旧件**不复原**（destroy 是物理断电，
	 *    回滚 ≠ 复活器件），catch 块如实点名丢失的变量。收益有二：新旧器件
	 *    不再短暂并存抢同一物理器件；新行 MetaProvider 的行号在创建时就已是
	 *    终值，不会被随后的删行前移打乱。 */
	HotDeleteOutcome delOutcome;
	if (deleteFirst) {
		delOutcome = executeHotDeletes(dels, daemonNameIndex);
	}
	/* mark 取在（可能已发生的）删除之后：先删后建下 mark.seq 即删后规模，
	 * 回滚只回滚插入段——已执行的物理删除不在任何回滚范围内（见上）。 */
	const HwRegistry::Mark mark = registry_.mark();
	const std::size_t metaMark = metaProviders_.size();
	const std::size_t daemonMark = daemonHolders_.size();
	std::vector<std::pair<std::size_t, std::size_t>> appended;	/* (矩阵下标, 追加行数) */
	std::size_t addedTables = 0;

	auto rollback = [&]() {
		/* ① 逆序断电：destroy 逐项容错（半装配/未 build 的项也要能安全 destroy，
		 *    与 deactivate 同口径），单个失败不能中断整轮回滚。 */
		const auto& seq = registry_.sequence();
		for (std::size_t i = seq.size(); i > mark.seq; --i) {
			try {
				seq[i - 1]->destroy();
			} catch (const std::exception& e) {
				std::cerr << "Hot SQL rollback warning: " << e.what() << std::endl;
			}
		}
		/* ② 撤注册（含流引用快照还原）→ ③ 弹登记表 → ④ 恢复矩阵 */
		registry_.rollbackTo(mark);
		metaProviders_.resize(metaMark);
		daemonHolders_.resize(daemonMark);
		for (const auto& [idx, n] : appended) { matrices_[idx].popRows(n); }
		matrices_.resize(matrices_.size() - addedTables);
	};

	try {
		/* 4.1 既有表：先落行（矩阵是装配的输入），再按旧行数增量装配 */
		for (const auto& [idx, rows] : appends) {
			mt::MatrixObject& live = matrices_[idx];
			const std::int64_t from = static_cast<std::int64_t>(live.rows());
			for (const auto& row : rows) {
				live.appendRow(row, static_cast<std::int64_t>(live.rows()));
			}
			appended.emplace_back(idx, rows.size());
			factory_.createHardVarsFrom(live, from);
		}
		/* 4.2 新表：整表入列后按 fromRow=1 装配 */
		for (auto& t : newTables) {
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
		if (!actionMats.empty()) { runner_.runAction(actionMats); }
	} catch (...) {
		try {
			rollback();
		} catch (const std::exception& e) {
			/* 回滚本身失败已超出手头能修的范畴：如实留痕，但不掩盖原错误
			 * （原错误才是调用方要看的成因）。 */
			std::cerr << "Hot SQL rollback FAILED: " << e.what() << std::endl;
		}
		/* 先删后建的失败口径（区别于既有批次的「原样上抛」）：本批次含 UPDATE
		 * 且已摘除过旧件时，活体**已经动过**——插入段失败只是「新件未建成」，
		 * 旧件不会回来。这里把成因包装成 runtime_error 并点名丢失的变量：
		 *  - 不谎报：模型/操作者必须知道「哪些变量已经没了」；
		 *  - 类型必须是 runtime_error 而非原样上抛——若成因本身是
		 *    IllegalArgumentException（如装配期重名），apply_sql 的派发漏斗
		 *    会按异常类型把它归类成「校验拒绝、活体一字未动」（paramError /
		 *    executed=false），对本口径是谎报：删旧件是真实发生过的副作用。 */
		if (deleteFirst && delOutcome.deletedVars > 0) {
			std::string names;
			for (const std::string& n : delOutcome.deletedNames) {
				if (!names.empty()) { names += ", "; }
				names += n;
			}
			const std::string tail =
				"；注意：本批次含 UPDATE，其命中旧行已断电摘除且未能重建（"
				+ names + "）——硬件不会自动复原，请核对状态后按需要的值重新 INSERT";
			try {
				throw;
			} catch (const std::exception& e) {
				throw std::runtime_error(std::string(e.what()) + tail);
			} catch (...) {
				throw std::runtime_error(
					"非标准异常（Lua 错误或外来抛出）" + tail);
			}
		}
		throw;
	}

	/* ==================== 阶段 4.6：热删除的物理执行（不可逆）====================
	 * 实现已抽出为 executeHotDeletes（2026-09-23 热更新：先删后建与先插入后
	 * 删除两个执行序共用同一套物理机器，语义/顺序/失败口径的完整注释随实现走）。
	 * 这里只留分派与既有次序的定位：非 UPDATE 批次把删除排在**全部可回滚工作
	 * 之后**——插入段失败时删除从未发生，整批回到原点（「一旦开始删，前面新增
	 * 的部分都已成功」，不存在「新器件要回滚、旧器件已删掉」的混合状态）；
	 * UPDATE 批次（deleteFirst）已在阶段 4 开头执行过，此处跳过。 */
	/* 「新增了几个变量」必须在**动手删之前**定下来：删除会从同一份注册表里摘
	 * 条目，用「事后总数 - 批次前总数」会把两边相互抵掉（同批删一插一时净增量
	 * 为 0，摘要就会谎报「新增 0 个」）。新增是「装配阶段登记了多少」这个
	 * 绝对值，删除是另一个维度，两者各自如实报数。deleteFirst 批次的删除已在
	 * 插入前执行，此刻的注册表规模即「删后 + 插后」，与删后 mark 相减同样得
	 * 出纯新增数。 */
	const std::size_t varsAfterInsert = registry_.sequence().size();
	if (!deleteFirst) {
		delOutcome = executeHotDeletes(dels, daemonNameIndex);
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
	std::size_t droppedTables = 0;
	std::vector<std::string> droppedNames;
	if (!muts.droppedTables.empty()) {
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
				droppedNames.push_back(trim(matrices_[k].getName()));
			}
		}
		/* ③ 降序 erase（先删大下标，前面的下标不失效） */
		for (auto it = shellIdx.rbegin(); it != shellIdx.rend(); ++it) {
			matrices_.erase(matrices_.begin() + static_cast<std::ptrdiff_t>(*it));
			++droppedTables;
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
	}

	/* ---- 生效摘要（REPL /sql 与 agent apply_sql 共用同一句话）---- */
	const std::size_t addedVars = varsAfterInsert - mark.seq;
	std::size_t addedRows = 0;
	for (const auto& [idx, n] : appended) { (void)idx; addedRows += n; }
	/* 热更新行数（G2 对账）：改写行的物理事实同时体现在「新增」与「删除」两个
	 * 计数里（断电重建），本行是改写事实的独立记账——复盘时三个数能相互对上。 */
	std::size_t updatedTotal = 0;
	for (const auto& [t, n] : muts.updatedRows) { (void)t; updatedTotal += n; }
	std::string msg = "已生效：新增硬件变量 " + std::to_string(addedVars) + " 个";
	if (addedTables > 0) { msg += "，新增表 " + std::to_string(addedTables) + " 张"; }
	if (addedRows > 0) { msg += "，既有表追加 " + std::to_string(addedRows) + " 行"; }
	if (updatedTotal > 0) {
		msg += "，已热更新 " + std::to_string(updatedTotal) + " 行（断电重建）";
	}
	if (delOutcome.deletedVars > 0) {
		msg += "，已删除硬件变量 " + std::to_string(delOutcome.deletedVars) + " 个（";
		for (std::size_t i = 0; i < delOutcome.deletedNames.size(); ++i) {
			if (i > 0) { msg += ", "; }
			msg += delOutcome.deletedNames[i];
		}
		msg += "）";
		if (delOutcome.deletedRows > delOutcome.deletedVars) {
			msg += "、结构表删除 " + std::to_string(delOutcome.deletedRows) + " 行";
		}
	}
	if (!actionMats.empty()) {
		std::size_t n = 0;
		for (const auto& m : actionMats) { n += m.rows() > 0 ? m.rows() - 1 : 0; }
		msg += "，瞬态动作已执行 " + std::to_string(n) + " 行（未注册，已丢弃）";
	}
	if (droppedTables > 0) {
		msg += "，已撤下表 " + std::to_string(droppedTables) + " 张（";
		for (std::size_t i = 0; i < droppedNames.size(); ++i) {
			if (i > 0) { msg += ", "; }
			msg += droppedNames[i];
		}
		msg += "）";
	}
	return msg;
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
		const std::vector<HotDelPlan>& plans,
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
	for (const HotDelPlan& p : plans) {
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
	for (const HotDelPlan& p : plans) {
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
	/* 4.6.2 守护退场屏障：与 deactivate() 同一手法——全体共享一个 deadline
	 * （逐个累加会让最坏退出延迟随守护数线性增长），超时者给宽限并如实告警。
	 * 这一步之后才允许动硬件。 */
	for (const auto& h : dyingHolders) { h->requestStop(); }
	if (!dyingHolders.empty()) {
		int64_t budgetMs = 0;
		for (const auto& h : dyingHolders) { budgetMs = std::max(budgetMs, h->joinTimeoutMs()); }
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(budgetMs);
		std::vector<DaemonRunner*> abandoned;
		for (const auto& h : dyingHolders) {
			if (!h->waitUntil(deadline)) { abandoned.push_back(h.get()); }
		}
		if (!abandoned.empty()) {
			const int64_t graceMs = std::min<int64_t>(budgetMs, deactivateGraceMs_);
			std::cerr << "热删除: " << abandoned.size()
				<< " 个守护未在停止屏障内退场，给予 " << graceMs
				<< "ms 宽限后再摘器件" << std::endl;
			std::size_t stillRunning = 0;
			for (DaemonRunner* d : abandoned) {
				if (!d->awaitExit(graceMs)) { ++stillRunning; }
			}
			if (stillRunning > 0) {
				std::cerr << "热删除: " << stillRunning
					<< " 个守护在宽限后仍在运行——它们可能继续触碰总线"
					"（其采样对象即将被摘除）" << std::endl;
			}
		}
	}
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
		/* ① 元数据提供者：指向被删行的整条摘除。判据是「矩阵 + 行号」而不是
		 * 引用相等——矩阵是 std::deque 的元素，地址稳定（这正是选 deque 的
		 * 原因），故该判据精确且不受行数变化影响。
		 * 已知边界（先于热更新存在，本特性不改其口径）：先插入后删除的混批里，
		 * 本批次新追加行的 provider 以追加时行号创建，随后 removeRows 擦掉更
		 * 早的原始行会让新行前移、其 row_ 失配——而 seed() 是惰性的（首次
		 * collect 才按 row_ 读矩阵），届时会读到错误的行。先删后建（UPDATE
		 * 批次）天然无此问题：所有删除先于任何新行创建，行号生来就是终值。 */
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

	if (!failures.empty()) {
		std::string msg = "热删除中途失败：已删除 " + std::to_string(out.deletedVars)
			+ " 个硬件变量";
		if (!out.deletedNames.empty()) {
			msg += "（";
			for (std::size_t i = 0; i < out.deletedNames.size(); ++i) {
				if (i > 0) { msg += ", "; }
				msg += out.deletedNames[i];
			}
			msg += "）";
		}
		msg += "，" + std::to_string(order.size() - out.deletedVars) + " 个未删除。成因：";
		for (std::size_t i = 0; i < failures.size(); ++i) {
			if (i > 0) { msg += "; "; }
			msg += failures[i];
		}
		msg += "。结构与注册表只反映已成功摘除的部分，未删的行保持原状。"
			"（destroy 是物理断电，已断电的器件不会因报错而复电——修好成因后"
			"对剩余项重发 DELETE 即可。）";
		throw std::runtime_error(msg);
	}
	return out;
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
