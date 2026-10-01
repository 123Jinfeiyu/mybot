#pragma once
/* robot.hpp —— 机器人编排门面（移植 bot4cj/src/RobotStruct.cj）
 * 结构脚本（SQL）→ MatrixObject 表 → 硬件变量注册表；
 * @Action 逐行重放，controller/trigger/Alias/守护 机制在此装配。
 *
 * 2026-09-29 A2 头文件拆分：本头原先声明全部 12 个类型（699 行），而实现
 * 早已按 TU 拆好——现在声明也各归各位，本头只剩 RobotStruct 门面：
 *   hw_registry.hpp     HwRegistry（注册表：所有权/销毁序/名字索引）
 *   meta_provider.hpp   MetaRef / MetaProvider（元数据引用与静态列缓存）
 *   script_loader.hpp   ScriptLoader（SQL 事件 → MatrixObject，实在 structure.cpp）
 *   hw_factory.hpp      HwFactory（矩阵表 → 硬件变量装配）
 *   action_runner.hpp   ActionPlan / ActionRunner（@Action 派发与重放）
 *   hotsql_plan.hpp     HotSqlPlan / HotSqlPlanner（热 SQL 只读计划层）
 * 只需局部类型的 TU 改包含对应窄头；RobotStruct 的消费者仍只包含本头。 */

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "action_runner.hpp"
#include "hotsql_plan.hpp"
#include "hw_factory.hpp"
#include "hw_registry.hpp"
#include "json.hpp"
#include "matrix.hpp"
#include "meta_provider.hpp"
#include "script_loader.hpp"
#include "util_cli.hpp"
#include "util_except.hpp"

namespace bot {

/* ---- 机器人编排门面：装配流程 + 生命周期（R-1 后只剩这些）---- */
class RobotStruct {
	/* 结构脚本解析的矩阵对象（声明序）。
	 * 容器选型是**不变式**而非风格（2026-09-23 热插表）：MetaProvider 持
	 * 指向此处元素的引用（matrix_），std::vector 在 append 扩容时会让全部
	 * 旧引用悬垂；std::deque 的 push_back/push_front 只失效迭代器，**元素
	 * 引用保持有效**，故热插表可以在 active() 之后安全追加。 */
	std::deque<mt::MatrixObject> matrices_;
	std::vector<std::shared_ptr<DaemonRunner>> daemonHolders_;	/* 守护注册表：deactivate 先统一置 stop；shared_ptr 供守护线程持 self 保生命周期 */
	HwRegistry registry_;				/* 硬件变量：所有权 + 销毁序 + 名字索引（R-2） */
	/* F-05：Lua 变量的元数据提供者登记表——active() 在 build 前统一校验
	 * $ 引用存在性（shared_ptr 与变量同生命周期，防悬垂） */
	std::vector<std::shared_ptr<MetaProvider>> metaProviders_;
	HwFactory factory_;	/* 依赖上面的容器，声明序保证先构造 */
	ActionRunner runner_;
	/* @Include 基准目录（mtinclude::expandIncludes 的相对路径根，缺省 "."）。
	 * 三个调用面按「脚本集合的根」取值：--input 文件通道=脚本所在目录、
	 * agent run_action_script=--scripts-dir、stdin/TCP 无脚本位置=缺省 CWD。
	 * run() 前设置即可，多轮执行（agent 工具）每次设置均生效。 */
	std::string actionIncludeBaseDir_ = ".";
	/* 是否已处于「断电」状态：active() 置 false，deactivate() 置 true。
	 * 析构据此判断要不要补一次断电——断电是生命周期不变式，不是调用点义务
	 * （见 ~RobotStruct 注释）。缺省 true：只解析结构、从未上电时无事可做。 */
	bool deactivated_ = true;
	/* --daemon-grace-ms（2026-09-22 F-2）：deactivate() 二段屏障宽限上限，
	 * 缺省 daemon_limits::kGraceMsDefault（原写死 kMaxGraceMs=30000）。
	 * setter 须在 active() 前调用才有意义（deactivate 才会读它），但即便
	 * 晚调用也不构成 UB——只是当次 deactivate 用旧值。 */
	int64_t deactivateGraceMs_ = daemon_limits::kGraceMsDefault;

	/* ---- 热删除的物理执行（原 applySql 阶段 4.6，2026-09-23 热更新抽出）----
	 * 抽成函数供两个执行序共用：「先插入后删除」（既有批次——插入失败时删除
	 * 未执行，整批可回滚）与「先删后建」（含 UPDATE 的批次——改写行同名重建，
	 * 注册表重名即抛，必须先摘旧件再装新件）。计划必须由 applySql 在任何
	 * 插入/上电**之前**构建（hw 指针计划期取好；先建后拆下按名 find 会命中
	 * 新注册的同名变量、摘错对象）。语义与失败口径见 robot.cpp 实现处注释。 */
	struct HotDeleteOutcome {
		std::size_t deletedVars = 0;
		std::size_t deletedRows = 0;
		std::vector<std::string> deletedNames;
	};
	HotDeleteOutcome executeHotDeletes(const std::vector<HotSqlPlan::Deletion>& plans,
		const std::map<std::string, std::size_t>& daemonNameIndex);

	/* ---- 热删除的一致性收尾（原 executeHotDeletes 阶段 4.6.4，2026-09-26 批次 5 拆出）----
	 * 只对**已成功断电**的部分落实：① 摘除指向被删行的 MetaProvider → ①′ 重锚定
	 * 存活提供者的行号（removeRows 保序擦除会让其后行前移）→ ② 擦结构行 →
	 * ③ 按保留项重建守护登记表 → ④ registry_.removeBatch 撤注册。perTable 是
	 * 「矩阵下标 → 被删行号（原始坐标，升序）」、removed 是已断电变量指针集，二者
	 * 由调用方从销毁序清单算好传入——本方法不认 Doomed，只认这两个派生视图与
	 * dyingHolders。deletedRows 累加进 out。语义与顺序理由见 robot.cpp 实现处。 */
	void applyHotDeleteConsistency(
		const std::vector<std::pair<std::size_t, std::vector<std::size_t>>>& perTable,
		const std::set<MatrixVars_Hardware*>& removed,
		const std::vector<std::shared_ptr<DaemonRunner>>& dyingHolders,
		HotDeleteOutcome& out);

	/* ---- applySql 的阶段机（2026-09-25 拆分）----
	 * applySql 原本约 600 行单函数，阶段编号（0/1/1.5/2/2.4/3/4/4.6/4.7）早已
	 * 写在注释里；拆分就是把每段注释与代码一起搬进具名函数，applySql 留阶段
	 * 地图与生效摘要。产物包结构（PlanSet/Outcome）让阶段间只传数据不传旗标。
	 * 判据与理由的完整注释都在 robot.cpp 实现处。 */

	/* 阶段 4 产物包：提交段的回执计数。mark.seq 不出函数——addedVars 在提交段
	 * 内部定值（「新增」必须在动手删之前记数，理由在实现处）。 */
	struct CommitOutcome {
		std::size_t addedVars = 0;
		std::size_t addedTables = 0;
		std::size_t addedRows = 0;
	};

	/* 阶段 4.7 产物包：整表撤下（DROP TABLE 空壳）的回执计数。 */
	struct DropOutcome {
		std::size_t count = 0;
		std::vector<std::string> names;
	};

	CommitOutcome commitHotBatch(const HotSqlPlan::Deletes& delPlan,
		HotSqlPlan::Insertions& insPlan, const HotDeleteOutcome& alreadyDeleted);
	DropOutcome dropTableShells(const ScriptLoader::Mutations& muts);

	/* 生效摘要（applySql 尾段，2026-10-01 拆出为纯拼装）：REPL /sql 与 agent
	 * apply_sql 共用同一句话，各计数口径见 robot.cpp 实现处。 */
	std::string summarizeHotOutcome(const CommitOutcome& committed,
		const ScriptLoader::Mutations& muts, const HotDeleteOutcome& delOutcome,
		const HotSqlPlan::Insertions& insPlan, const DropOutcome& dropped) const;

	/* ---- commitHotBatch 的两块拆出（2026-09-29 A3）----
	 * rollbackHotBatch：插入段失败的逆序断电 + 撤注册 + 恢复矩阵（原 45 行
	 * rollback lambda，语义逐字保持）；rethrowWithLostVars：先删后建批次的
	 * 失败重包装（点名已摘除且未重建的变量，理由见实现处注释）。 */
	void rollbackHotBatch(const HwRegistry::Mark& mark, std::size_t metaMark,
		std::size_t daemonMark,
		const std::vector<std::pair<std::size_t, std::size_t>>& appended,
		std::size_t addedTables);
	[[noreturn]] void rethrowWithLostVars(const HotDeleteOutcome& alreadyDeleted);

public:
	explicit RobotStruct(bot::Reader& input)
		: factory_(registry_, daemonHolders_, metaProviders_), runner_(registry_) {
		ScriptLoader::readSQL(input, matrices_);
	}

	/* 兜底断电：仍未 deactivate 就补一次（幂等）。存在的理由是物理安全——
	 * 漏掉断电的代价是执行器停在最后指令位置、I2C 从设备滞留中间态。 */
	~RobotStruct();

	/* 激活所有硬件变量（两阶段构造：解析 → 创建 + build） */
	void active();

	/* 消除所有硬件变量（先统一置守护 stop 防饿死，再按声明序倒序 destroy） */
	void deactivate();

	/* 执行 @Action 矩阵（含内联 @Alias 注册） */
	void run(bot::Reader& input);

	/* ---- 热插表：结构 SQL 在上电之后真正生效（2026-09-23）----
	 * 契约：
	 *  1) sql 是**一条或一批**结构 SQL（CREATE TABLE / INSERT INTO / DELETE FROM /
	 *     DROP TABLE / UPDATE … SET … [WHERE …]；可混排），INSERT 的目标可以是
	 *     本批次新建的表，也可以是结构里既有表；
	 *  2) 前置条件 active() 已成功（机器人上电常驻）——热插是「带电变更」，
	 *     未上电时直接用结构脚本，不走这里；
	 *  3) 事务性：插入段**全成功或全回滚**。任何一步失败（校验/装配/build/动作
	 *     执行）都逆序 destroy 本批次新建的硬件、撤销注册与矩阵行，抛异常说明
	 *     成因；动作已下发的物理副作用不回退（与「已执行的动作不回退」同口径）。
	 *     DELETE/DROP 的物理断电**不可逆**（回滚 ≠ 复活器件）；UPDATE 的语义是
	 *     **断电重建**（命中行按「断电摘除 + 重新装配上电」执行），含 UPDATE 的
	 *     批次采用先删后建序——插入段失败时已摘除的旧行不复原，错误信息点名
	 *     丢失的变量。UPDATE 与该表的其它行级语句不得同批（readSQL 守门）；
	 *  4) Action 表**瞬态**（见 P6 注释）：其行不入结构矩阵、不注册变量，硬件
	 *     表全部 build 完成后执行一次随即丢弃——故动作可以引用本批次新建变量；
	 *  5) 返回一句人读的生效摘要（REPL /sql 与 agent apply_sql 共用）。 */
	std::string applySql(const std::string& sql);

	/* 设置 @Include 的相对路径基准目录（语义见成员注释） */
	void setActionIncludeBaseDir(std::string dir) { actionIncludeBaseDir_ = std::move(dir); }

	/* 设置 deactivate() 二段屏障的宽限上限（--daemon-grace-ms，语义见成员注释） */
	void setDeactivateGraceMs(int64_t ms) { deactivateGraceMs_ = ms; }

	/* PL-2（2026-09-22）：把执行预算挂到**全部** Lua 硬件变量（budget=nullptr 解除）。
	 * 调用方用 RAII 成对调用——窗口只在 run_action_script 一次工具调用之内，
	 * 窗口外 JIT 与钩子必须复原（test_jit_warmup.lua 锁的是 JIT 路径，
	 * 不能被本机制静默废掉）。遍历 registry_.sequence()：一行 @Action 可能经
	 * controller 转发/trigger 前置走到别的 lua_State，只挂被点名那一个会漏。 */
	void armExecBudget(const LuaExecBudget* budget) {
		for (MatrixVars_Hardware* hv : registry_.sequence()) { hv->armExecBudget(budget); }
	}

	/* agent 模式：只读硬件注册表视图（清单/种类），供工具输出与提示词使用 */
	const HwRegistry& registry() const { return registry_; }

	/* agent 模式：只读结构矩阵视图（声明序）。供 /sql 查询从原始矩阵取数渲染——
	 * 与 registry()（装配后的硬件变量视图）互补：前者看结构脚本定义，后者看装配结果。 */
	const std::deque<mt::MatrixObject>& matrices() const { return matrices_; }

	/* agent 模式：结构脚本是否已声明名为 name 的变量。
	 * 必须在 active() **之前**可判定，故直接扫矩阵表的 name 列，
	 * 而不是查注册表（注册表要 active() 里 createHardVars 才装配）。 */
	bool declaresVariable(const std::string& name) const;

	/* 测试自检（2026-09-25 审计 A-4）：metaProviders_ 与活体结构一一对应——
	 * 每个会装配 Lua 变量的数据行恰有一个提供者、每个提供者锚定一个范围内
	 * 的真实数据行（multiset 比对，行号错位/摘错/漏摘都会失衡）。生产路径
	 * 不调用；热删除 4.6.4 的 deadProvider 判据所声称的「精确」以此为准。 */
	bool metaProvidersMatchLiveRows() const;

	/* agent 模式：结构脚本未声明某个 Lua 驱动变量时注入一个（须在 active() 之前调用）。
	 * LLM 协议驱动与摄像头驱动都走这里。
	 * 复用脚本装载同一条路径（合成 SQL 文本 → ScriptLoader::readSQL → 追加矩阵），
	 * 免得为「多一个变量」再造一套矩阵构造 API。
	 * driverTable 同时是驱动文件名（modules/<driverTable>.lua），与变量名都须是
	 * 安全标识符——否则合成出来的 SQL 会被注入出别的表。 */
	void injectLuaDriver(const std::string& driverTable, const std::string& varName);

};

} // namespace bot
