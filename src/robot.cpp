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

	/* ---- 阶段 1：解析到影子容器（活体一字未动）---- */
	std::deque<mt::MatrixObject> staged;
	for (const auto& m : matrices_) {
		mt::MatrixBuilder b;
		b.name(m.getName());
		for (std::size_t c = 0; c < m.cols(); ++c) { b.item(m(0, c)); }
		b.line(0);
		staged.push_back(b.take());
	}
	const std::size_t liveCount = matrices_.size();
	{
		MemoryReader reader(sql);
		ScriptLoader::readSQL(reader, staged);
	}

	auto rowOf = [](const mt::MatrixObject& m, std::size_t r) {
		std::vector<std::string> row;
		row.reserve(m.cols());
		for (std::size_t c = 0; c < m.cols(); ++c) { row.push_back(m(r, c)); }
		return row;
	};

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

		const std::size_t nrows = st.rows() > 0 ? st.rows() - 1 : 0;
		if (isShadow && nrows == 0) { continue; }	/* 既有表且本批次没给它插行 */

		const std::int64_t idxName = st.indexOfCol(0, "name");
		for (std::size_t r = 1; r < st.rows(); ++r) {
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
				if (registry_.contains(v) || !batchNames.insert(v).second) {
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
			mt::MatrixBuilder ab;
			ab.name(st.getName());
			for (std::size_t c = 0; c < st.cols(); ++c) { ab.item(st(0, c)); }
			ab.line(0);
			for (std::size_t r = 1; r < st.rows(); ++r) {
				for (std::size_t c = 0; c < st.cols(); ++c) { ab.item(st(r, c)); }
				ab.line(static_cast<std::int64_t>(r));
			}
			actionMats.push_back(ab.take());
		} else if (isShadow) {
			std::vector<std::vector<std::string>> rows;
			for (std::size_t r = 1; r < st.rows(); ++r) { rows.push_back(rowOf(st, r)); }
			appends.emplace_back(si, std::move(rows));
		} else {
			newTables.push_back(st);
		}
	}

	if (appends.empty() && newTables.empty() && actionMats.empty()) {
		throw IllegalArgumentException("热插表：本批次没有任何 CREATE/INSERT 变更"
			"（SELECT 是只读查询，请用 query_sql / REPL 的 /sql）");
	}

	/* ---- 阶段 3：全结构判据（对「提交之后的视图」复跑，与启动期同一实现）----
	 * 消费判据看的是整张结构（新行可能与任意既有行抢同一器件），故必须合并
	 * 视图后判定；合并视图只为这两条判据而生，随即丢弃。 */
	{
		std::deque<mt::MatrixObject> merged = matrices_;
		for (const auto& [idx, rows] : appends) {
			for (const auto& row : rows) { merged[idx].appendRow(row, 0); }
		}
		for (const auto& t : newTables) { merged.push_back(t); }
		factory_.precheckStructure(merged);
	}

	/* ---- 阶段 4：提交 + 增量装配 + 上电（任何一步失败 ⇒ 全回滚）---- */
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
		throw;
	}

	/* ---- 生效摘要（REPL /sql 与 agent apply_sql 共用同一句话）---- */
	const std::size_t addedVars = registry_.sequence().size() - mark.seq;
	std::size_t addedRows = 0;
	for (const auto& [idx, n] : appended) { (void)idx; addedRows += n; }
	std::string msg = "已生效：新增硬件变量 " + std::to_string(addedVars) + " 个";
	if (addedTables > 0) { msg += "，新增表 " + std::to_string(addedTables) + " 张"; }
	if (addedRows > 0) { msg += "，既有表追加 " + std::to_string(addedRows) + " 行"; }
	if (!actionMats.empty()) {
		std::size_t n = 0;
		for (const auto& m : actionMats) { n += m.rows() > 0 ? m.rows() - 1 : 0; }
		msg += "，瞬态动作已执行 " + std::to_string(n) + " 行（未注册，已丢弃）";
	}
	return msg;
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
