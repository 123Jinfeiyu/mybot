#pragma once
/* robot.hpp —— 机器人构成模块（移植 bot4cj/src/RobotStruct.cj）
 * 结构脚本（SQL）→ MatrixObject 表 → 硬件变量注册表；
 * @Action 逐行重放，controller/trigger/Alias/守护 机制在此装配。
 * R-1：RobotStruct 收敛为编排门面；脚本装载（ScriptLoader）、硬件装配
 * （HwFactory）、动作重放（ActionRunner）三个协作对象各司其职。
 */

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hardware.hpp"
#include "json.hpp"
#include "matrix.hpp"
#include "sqlparse.hpp"
#include "util.hpp"

namespace bot {

/* 重构计划 2.2：本头文件不再传导 LuaJIT 头（luaend.hpp 经 daemonvar/luavar
 * 拉入 lua.h，所有 include robot.hpp 的 TU 都被迫解析 Lua 头）。DaemonRunner
 * 只以 shared_ptr 成员出现——shared_ptr 对不完整类型安全（类型擦除删除器），
 * 前向声明即可；具体头由 robot.cpp 与真正用到的 TU 自行引入。 */
class DaemonRunner;

/* R-2：硬件变量注册表——所有权、销毁序、名字索引的单一来源。
 * 旧实现三个并行容器（hardwareVars_/hwPool_/hwSeq_）靠 addHardwareVar
 * 手工同步；会话期别名覆盖直接改写 map，不变式被打破。
 * 实现 HwLookup 只读接口（2.1）：MtVars_Lua 等只依赖该抽象；byName()
 * 是 agent 清单的具象视图，不进接口（假实现无需维护清单）。
 *
 * 阶段 1（2026-09-20）：会话变量作用域。动作脚本内联的 @Alias/@Vars 走
 * session_ 覆盖层，生命周期严格等于一次 RobotStruct::run()——此前它们直接
 * 改写 byName_（永久遮蔽结构定义）且所有权堆在 pool_ 里永不回收（agent 长驻
 * 模式无界增长）。分层后 pool_/seq_/byName_ 三者只属于结构期，MetaRef::target
 * 的裸指针不变式从「靠约定」变成「靠构造」。 */
class HwRegistry : public HwLookup {
	std::vector<std::unique_ptr<MatrixVars_Hardware>> pool_;	/* 结构期所有权（只增不删） */
	std::vector<MatrixVars_Hardware*> seq_;			/* 销毁序：结构声明序，active/deactivate 按序 */
	std::map<std::string, MatrixVars_Hardware*> byName_;	/* 结构期名字索引 */
	/* N-6：被结构引用列（controller 出边 / trigger 入边）指向的名字集合——
	 * 会话期覆盖不得触碰，否则派发按名查到别名（MtVars_Alias），报出的
	 * "non-table result" 与真实成因（引用目标被覆盖）完全脱节。 */
	std::set<std::string> flowRefs_;
	/* 阶段 1：会话覆盖层。仅由 SessionScope 持有生命周期；不变量是
	 * 「非空 ⟺ 正在一次 run() 之内」。 */
	std::map<std::string, std::unique_ptr<MatrixVars_Hardware>> session_;

public:
	/* 结构期注册：重名即抛（source 指明声明来源，如 " (in table 'X')"） */
	void add(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
			const std::string& source = "");

	/* 会话期别名覆盖：写进会话覆盖层（不进入销毁序、不触碰结构视图）。
	 * 名字被引用为 controller/trigger 时拒绝覆盖（N-6）。
	 * 作用域 = 外层 SessionScope 的生命周期（RobotStruct::run 一次）。 */
	void override(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw);

	/* 会话期单条撤销（#34 @Trigger）：每次触发绑定的临时别名在触发执行完
	 * 后逐个移除，不依赖整个 SessionScope（run() 级）提前结束——反射触发
	 * 是 run() 内部的多次子作用域。名字不在会话层时静默（防御性）。 */
	void removeSessionOverride(const std::string& name);

	/* 会话作用域（阶段 1）：run() 入口构造、出口析构，异常路径同样回滚。
	 * 构造即清空（防御性：正常路径下构造时必为空），析构把会话变量连同
	 * 其所有权一起释放——结构定义与结构期所有权一字不动。 */
	class SessionScope {
		HwRegistry* reg_;
	public:
		explicit SessionScope(HwRegistry& reg) : reg_(&reg) { reg_->session_.clear(); }
		~SessionScope() { reg_->session_.clear(); }
		SessionScope(const SessionScope&) = delete;
		SessionScope& operator=(const SessionScope&) = delete;
	};

	/* 会话层规模（只读）。测试与诊断用：结构清单 byName() 不得因会话而增长。 */
	std::size_t sessionSize() const { return session_.size(); }

	/* 登记结构引用列指向的名字（controller/trigger，createLuaVars 装配期调用） */
	void addFlowRef(const std::string& name) { flowRefs_.insert(name); }

	/* ---- 热插事务标记（2026-09-23）----
	 * 唯一用途：RobotStruct::applySql 的「全成功或全回滚」。结构期三容器在
	 * add() 里锁步增长（pool_ 与 seq_ 尾部一一对应、byName_ 是名字索引），
	 * 故一个长度 + 流引用快照就足以把注册表精确还原到批次开始前。
	 * 这是对「pool_ 只增不删」这条既有不变式的**受控、有界**放宽：撤销范围
	 * 严格等于本次热插批次新登记的部分，结构期既有条目一字不动。 */
	struct Mark {
		std::size_t seq = 0;			/* 销毁序长度（pool_ 与之锁步等长） */
		std::set<std::string> flowRefs;		/* 流引用集合快照（通常极小） */
	};

	Mark mark() const { return Mark{seq_.size(), flowRefs_}; }

	/* 回滚到标记：**调用方必须先对尾部变量逆序 destroy()**（断电是物理动作，
	 * 注册表不认识硬件）。本方法只撤注册与所有权——弹出尾部条目、摘掉对应的
	 * 名字索引、恢复流引用快照；元素随 unique_ptr 析构逐个释放。 */
	void rollbackTo(const Mark& m);

	/* ---- 热删除：按指针集合撤销注册（2026-09-23）----
	 * 与 rollbackTo 的分工：rollbackTo 撤的是「本批次**新加**的尾部条目」（热插
	 * 回滚），本方法撤的是「**任意位置**的既有条目」（热删除）——都持有同一份
	 * 契约：**调用方必须先 destroy() 断电**，本方法只撤所有权与索引。
	 *
	 * 与 rollbackTo 的差别正是本方法存在的理由：pool_/seq_/byName_ 三容器在
	 * add() 里锁步增长，但**全体下标在下标 0 处对齐**这条便利只在「撤尾部」时
	 * 成立。撤中间条目会把后续元素前移，于是任何「先记下标、再逐个 erase」的
	 * 实现都会在第一次 erase 之后就错位。这里改为一趟 erase-remove：先把
	 * doomed 在**全部三个容器**里的存在性验完，再统一重排——校验与修改分离，
	 * 不存在「改了一半发现指针不认识」的中间态。
	 *
	 * 为什么用指针集合而不是名字集合：调用方的判据来自结构行（哪些行被删），
	 * 而「行 → 变量」的映射要经 name 列查表；指针集合让调用方直接用它在装配期
	 * 就已识别的对象，且天然去重（同一变量不可能注册两次）。
	 *
	 * 悬垂指针防线（本方法的存在前提，不是副作用）：被删变量的引用方在框架里
	 * 是**终身缓存的裸指针**——MtVars_Lua::ctrlPtrs_/trigPtrs_（首用缓存，
	 * 注释写着「指针终身有效」）与 MetaRef::target（seed 期锁定，注释写着
	 * 「pool_ 只增不减，生命周期安全」）。本方法只保证**注册表自身**不再交出
	 * 这些指针；「不留下指向已删变量的缓存」由调用方在预检阶段以「被引用即
	 * 拒绝删除」保证（见 RobotStruct::applySql 的删除预检）。 */
	void removeBatch(const std::set<MatrixVars_Hardware*>& doomed);

	/* N-15：表达式标识符求值器（只认 @Vars 数值；未定义/非 @Vars 即抛）——
	 * MetaProvider 的 $(...) 求值、HwFactory 装配共用 */
	std::function<double(const std::string&)> makeVarsResolver() const override;

	/* @Action 求值面（数据格/列头）的标识符求值器：@Vars → 数值真值、
	 * @Alias → 文本；驱动/守护名抛错（表达式里没有驱动的一席之地——
	 * 名字是拿来做算术/拼接的，不是拿来做播放的）。仅 Action 路径使用。 */
	std::function<ExprValue(const std::string&)> makeActionExprResolver() const override;

	MatrixVars_Hardware* find(const std::string& name) const override {
		/* 会话层优先：run() 之内别名可见（@Action 设备位/参数位解析靠它），
		 * run() 之外会话层必为空，行为与结构视图完全一致 */
		auto s = session_.find(name);
		if (s != session_.end()) { return s->second.get(); }
		auto it = byName_.find(name);
		return it == byName_.end() ? nullptr : it->second;
	}

	bool contains(const std::string& name) const override {
		return session_.count(name) > 0 || byName_.count(name) > 0;
	}

	/* 销毁序视图（build 正向 / destroy 反向）；会话别名不在其中 */
	const std::vector<MatrixVars_Hardware*>& sequence() const { return seq_; }

	/* agent 模式：只读名字索引（硬件清单）。只给 const 视图——
	 * 清单是「描述」用途，不应成为第二条改写注册表的路径。
	 * 阶段 1：**只含结构变量**——会话层生命周期严格包含在 run() 之内，
	 * 而工具调用只发生在两次 run() 之间（那时会话层必为空），故无需合并视图。 */
	const std::map<std::string, MatrixVars_Hardware*>& byName() const { return byName_; }
};

/* 元数据引用列：变量引用（var 非空）或 $(...) 表达式引用（expr 非空）。
 * 表达式在 seed 期解析（语法早报 + 标识符存在性校验），collect 期惰性求值。
 * N-5：var 引用在 seed 期（build 前，先于任何会话别名覆盖）锁定目标指针
 * target——collect/collectForDestroy 不再按名重查。否则 --tcp 会话别名
 * 覆盖后，结构变量的销毁期快照会解析到覆盖者（别名），拿到错误的 fd/addr
 * 快照做去初始化。锁定期语义：$ 引用绑定结构期变量，会话覆盖不影响它。
 * target 为非所有权指针，HwRegistry::pool_ 只增不减，生命周期安全。 */
struct MetaRef {
	std::string col;				/* 承载 $ 的列名 */
	std::string var;				/* 变量引用（expr 为空时有效） */
	MatrixVars_Hardware* target = nullptr;	/* seed 期锁定的引用目标 */
	std::shared_ptr<const Expression> expr;		/* 表达式引用 */
};

/* P8/M2：元数据静态列缓存（字面列冻结 + 引用列实时 read() 快照）。
 * name 列与结构引用列（controller/trigger）不进元数据——按保留列名跳过。 */
class MetaProvider {
	/* 锚定的矩阵：**指针而非引用**（2026-09-23 整表撤下）。矩阵元素住在
	 * matrices_（std::deque）里，整表 DROP 会 erase 掉某个元素，而 deque 的
	 * 中间 erase 会让其后所有元素引用失效（标准如此，不依赖某个实现是否原地
	 * 搬值）；引用成员绑死一次、无法重锚，故改为指针并配 rebase()。行数变化
	 * （行删除）不影响本指针——那只会前移行号；本指针只在整表移除后需要更新。 */
	const mt::MatrixObject* matrix_;
	std::int64_t row_;
	const HwRegistry* registry_;
	std::optional<std::vector<std::string>> cols_;
	std::optional<std::vector<std::string>> vals_;
	/* 引用列：变量快照按字段并入元数据；@Vars 标量/表达式则把值写到该列名下。 */
	std::optional<std::vector<MetaRef>> refs_;

	void seed();
	/* 字面列（含 "-" 继承结果）构造——运行期与销毁期两条收集路径共用 */
	Json staticPart();
	/* N-16：collect / collectForDestroy 公共内核（forDestroy=false 时
	 * 引用解析失败照常上抛；true 时逐引用尽力而为、logD 跳过） */
	Json collectRefs(bool forDestroy);

	/* 阶段 3 拆分：单个引用列的快照并入 / destroy 跳过日志单点（契约见实现处） */
	bool appendRef(Json& obj, const MetaRef& ref, bool forDestroy);
	void logDestroySkip(const std::string& what, const std::string& why);

public:
	MetaProvider(const mt::MatrixObject& matrix, std::int64_t row,
			const HwRegistry* registry)
		: matrix_(&matrix), row_(row), registry_(registry) {}

	Json collect();
	/* F-04′：字面列 + 引用列的「尽力而为」快照——destroy 阶段用。
	 * 单个引用解析失败只跳过该引用，绝不放弃整轮 destroy（理由见 robot.cpp 实现处）。 */
	Json collectForDestroy();
	/* F-05：装配完成后、build 前做引用存在性校验（seed 未通过即抛出） */
	void validate() { seed(); }

	/* 热删除（2026-09-23）：本提供者锚定的「矩阵 + 行」。RobotStruct 用它摘除
	 * 被删行的提供者——判据必须是这一对（而不是提供者自身的某个 id），因为
	 * 行列就是它在结构里的唯一身份。矩阵是 std::deque 的元素、地址稳定，
	 * 故该判据精确且不受行数变化影响。 */
	const mt::MatrixObject& matrix() const { return *matrix_; }
	std::int64_t row() const { return row_; }

	/* 整表撤下（2026-09-23）：矩阵地址（供 RobotStruct 在 erase 前后建
	 * 旧地址→新地址映射，据此重锚定存活提供者）。 */
	const mt::MatrixObject* matrixPtr() const { return matrix_; }
	/* 重锚定到矩阵的新地址（整表 DROP 后 deque 元素搬移/引用失效时调用）。
	 * **不清缓存**：存活表的内容逐字未变（只是位置变了），seed 结果（cols_/
	 * vals_/refs_）语义仍然成立；行号 row_ 也不因整表移除而变。 */
	void rebase(const mt::MatrixObject& m) { matrix_ = &m; }
	/* 行删除的行号重锚定（2026-09-25 审计 A-4 收口）：removeRows 是保序擦除，
	 * 同矩阵内位于被删行之后的行会前移，锚定这些行的提供者必须同步减行号
	 * ——row() 是**后续批次** deadProvider 判据的坐标（executeHotDeletes 的
	 * 4.6.4①），过期即摘错/漏摘提供者。**不清缓存**：seed 只在行号定值时
	 * 读一次矩阵（active/commitHotBatch 的 validate 都是急切 seed），缓存
	 * 的静态列与按名/指针锁定的引用不受行移动影响。delta 恒为负（行只会
	 * 前移），签名保持通用减法。 */
	void shiftRow(std::int64_t delta) { row_ += delta; }
};

/* ---- 脚本装载：SQL/矩阵 文本 → MatrixObject（R-1 拆自 RobotStruct）----
 * N-11：矩阵文本解析（readMatrix）已下沉 mt 层，本类只剩 SQL 事件装配。 */
class ScriptLoader {
public:
	/* 热删除改动记录（2026-09-23）：readSQL 执行 DELETE 时把「删了哪些原始行、
	 * 哪些硬件变量名」如实回报给调用方（RobotStruct::applySql）。
	 *
	 * 为什么必须回报而不是让调用方自查：DELETE 会**改变容器内行号**，而调用方
	 * 需要的是**原始坐标**下的行号——它要拿这些行号回活体矩阵 `matrices_` 上
	 * 精确摘行。readSQL 内部逐条维护「当前行号 → 原始行号」映射，是全链路里
	 * 唯一知道这个映射的地方；让 applySql 事后靠「比较行内容」反推既脆弱
	 * （内容相同的两行无法区分）又重复一遍这里的匹配逻辑。
	 *
	 * deletedRows 按容器下标对齐（与 applySql 的影子容器同序），值是该表**原始
	 * 行号**（一次 DELETE 命中多行即多条）；deletedNames 是该批次所有被删变量名
	 * （name 列取值），供生效摘要与审计对账。两者都只增不改——调用方据新增部分
	 * 判断本批次的影响面。 */
	struct Mutations {
		std::map<std::string, std::vector<std::size_t>> deletedRows;
		std::vector<std::string> deletedNames;
		/* 每表本批次**插入的行数**（按表名）。调用方（applySql 的影子容器对既有
		 * 表是全量拷贝）靠它划出「哪些行是本批次新增」——不能用「当前行数 >
		 * 解析前行数」代替：一条 INSERT 与一条 DELETE 落在同一张表上时净行数
		 * 可能不变，那个判据会把新增行整批漏掉（既装配不出来，也不进瞬态 Action）。
		 * 表名在**本批次 CREATE 出来的表**上也会出现（其插入行数即全表数据行数）。
		 * UPDATE 的分解实现（改写行 = 擦原行 + 尾部追加改后行）**同样计数**：
		 * 改后行就是「本批次新增行」，靠本计数进入新增区间；被改写的原始行另计入
		 * deletedRows（原始坐标）。 */
		std::map<std::string, std::size_t> insertedRows;
		/* 每表本批次**被 UPDATE 改写的行数**（按表名，2026-09-23 热更新）。两个
		 * 用途：① applySql 据此判定「本批次含改写」→ 切换为先删后建执行序（见
		 * 其注释：先建后拆下同名重建会在注册表撞名）；② 生效摘要与审计如实报出
		 * 「已热更新 N 行（断电重建）」，而不是把同一次改写谎报成一次删除加一次
		 * 新增。改写行同时计入 deletedRows/insertedRows（物理事实如此），本字段
		 * 只是改写事实的独立记账。 */
		std::map<std::string, std::size_t> updatedRows;
		/* 本批次被 DROP 掉的**表名**（整表撤下，2026-09-23）。readSQL 已在影子容器
		 * 上把该表的全部数据行清空（并计入 deletedRows/deletedNames，故行的断电与
		 * 撤注册走与 DELETE 同一条 DelPlan 机器）；调用方据此**额外**把空表壳从活体
		 * matrices_ 上摘除，并重锚定其后 MetaProvider 的矩阵引用（deque 整表移除会
		 * 让后续元素引用失效）。表名在「本批次 CREATE 后又被 DROP」的表上也会出现
		 * ——那类表从未装配，摘除面只有影子容器。 */
		std::set<std::string> droppedTables;
	};

	/* SQL4cj → MatrixObject 结构表解析。
	 * 容器为 std::deque（2026-09-23 热插表）：结构期 matrices_ 必须是
	 * 「两端插入不失效元素引用」的容器，MetaProvider::matrix_ 的引用不变式
	 * 才能从「靠 inject 必须早于 active 的约定」升级为构造保证（见 robot.cpp）。
	 *
	 * allowDelete 缺省 **false**：同一个函数服务两条路径——结构脚本装配
	 * （RobotStruct 构造期，机器人尚未上电，删不了任何东西）与热插行级变更
	 * （applySql：DELETE / DROP / UPDATE——UPDATE 分解为「DELETE 命中行 +
	 * INSERT 改后行」，同样只在带电路径放行）。缺省关使启动路径的接受语言逐字
	 * 不变，且「结构脚本里写 DELETE/UPDATE」得到的是一句人话而不是运行期怪错；
	 * 热插调用点显式传 true。
	 * mutations 为空 = 不收集变更信息（同样只在热插路径需要）。 */
	static void readSQL(bot::Reader& input, std::deque<mt::MatrixObject>& out,
			bool allowDelete = false, Mutations* mutations = nullptr);
};

/* ---- 硬件装配：矩阵表 → 硬件变量（R-1 拆自 RobotStruct）---- */
class HwFactory {
	HwRegistry& registry_;
	std::vector<std::shared_ptr<DaemonRunner>>& daemonHolders_;
	std::vector<std::shared_ptr<MetaProvider>>& metaProviders_;

	/* 重名前置判重（资源泄漏防线：Lua 状态机/共享槽先申请，此处先确认可注册） */
	void precheckDuplicate(const std::string& name, const std::string& mtName) {
		if (registry_.contains(name)) {
			throw IllegalArgumentException("Duplicate hardware variable name: '" + name
				+ "' (in table '" + mtName + "')");
		}
	}

	void createDaemonVars(const mt::MatrixObject& matrix, const std::string& mtName,
			std::int64_t fromRow);
	void createLuaVars(const mt::MatrixObject& matrix, const std::string& mtName,
			std::int64_t fromRow);

	/* 装配前的单消费者判据（审计 1.3）：只对**自己声明** SINGLE_CONSUMER 的驱动
	 * 生效（其寄存器访问跨多个总线事务，器件侧地址相位会被第二个消费者踩掉）。
	 * HAL 的每次访问是单次 I2C_RDWR 原子事务，故绝大多数器件允许多消费者，
	 * 不做一刀切。判据 = (总线设备路径, 归一化地址) 唯一；冲突即报错并点名双方。
	 * 实现与理由见 hw_factory.cpp 的同名函数注释。 */
	static void checkSingleConsumer(const std::deque<mt::MatrixObject>& matrices);

	/* F5-4：同一物理串口被 2+ 个「各自 open 独立 fd」的消费者（UART 总线变量 /
	 * 真机守护）声明时的加载期告警——HAL 不做跨变量引用计数，后关闭者以自己
	 * open 时的配置为准。只告警不拒绝（同配置轮询下实际可用；是否收紧属
	 * 破坏性决策）。实现与理由见 hw_factory.cpp 的同名函数注释。 */
	static void warnSharedUartPaths(const std::deque<mt::MatrixObject>& matrices);

	/* 标量文本矩阵种类：@Alias(参数别名的目标) / @Vars(命名数值) */
	enum class ScalarKind { Alias, Vars };

	/* 注册标量文本矩阵的公共实现（启动期/动作脚本期两路，见实现处）。
	 * fromRow 是**首个待装配的行号**（1 = 全量；热插表的增量装配传旧行数），
	 * 行 0 恒为列头。 */
	void registerScalarMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session,
			ScalarKind kind, std::int64_t fromRow);

public:
	/* 守护侧缺省值（2026-09-21 第七轮审计）。单消费者判据遇到 Daemon 行**缺列**时
	 * 必须按守护侧的缺省补全，否则「省略 proto/path」这类被 docs/daemon.md 认可的
	 * 最小写法（`INSERT INTO Daemon (name, chunk)`）会整体绕过判据——运行期真的
	 * 开了总线 fd 却从不登记，漏标即双消费者静默数据错乱，且无运行期症状。
	 * 这三个值**必须**与 modules/lib/sensor_daemon.lua 的
	 *   local proto = cfg.proto or "i2c"
	 *   local path  = cfg.path or (proto == "spi" and "/dev/spidev0.0" or "/dev/i2c-1")
	 * 逐字一致；漂移由 test/cpp/test_robot.cpp 的 daemon_defaults_match_lua_source
	 * 读 Lua 源按文本钉住（不是在两处各手抄一遍就算数）。放成 public 正是为了让
	 * 那条用例能引用到 C++ 侧的值，做**两侧**比对。 */
	static constexpr const char* kDaemonDefaultProto = "i2c";
	static constexpr const char* kDaemonDefaultPathI2c = "/dev/i2c-1";
	static constexpr const char* kDaemonDefaultPathSpi = "/dev/spidev0.0";

	HwFactory(HwRegistry& registry,
			std::vector<std::shared_ptr<DaemonRunner>>& daemonHolders,
			std::vector<std::shared_ptr<MetaProvider>>& metaProviders)
		: registry_(registry), daemonHolders_(daemonHolders), metaProviders_(metaProviders) {}

	/* 全部硬变量创建：声明序，Alias/Vars/Lua/Daemon 分派 */
	void createHardVars(const std::deque<mt::MatrixObject>& matrices);

	/* 增量装配：只装配**单个矩阵**的 [fromRow, rows) 行（行 0 为列头）。
	 * 启动期 createHardVars 逐矩阵以 fromRow=1 调用本函数（单一装配路径，
	 * 全量/增量不各写一份）；热插表事务对「批次内新增的矩阵」用 fromRow=1、
	 * 对「既有矩阵的新增行」用 fromRow=旧行数。调用方负责前置的全量静态
	 * 校验（判据/重名/列/模块存在性等，见 RobotStruct::applySql）。 */
	void createHardVarsFrom(const mt::MatrixObject& matrix, std::int64_t fromRow);

	/* 全结构判据入口（单消费者 + 同串口多 fd 告警）——启动期 createHardVars
	 * 内部同样调用这两个。HotSqlPlanner 在提交之前对合并视图复跑同一判据，
	 * 无需 factory 实例；只读矩阵与模块文件，保留既有告警。
	 * 注意：判据看的是整张结构，故调用方传入的必须是合并后的视图。 */
	static void precheckStructure(const std::deque<mt::MatrixObject>& matrices);

	/* 注册 @Alias 别名矩阵 */
	void registerAliasMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session) {
		registerScalarMatrix(matrix, session, ScalarKind::Alias, 1);
	}

	/* 注册 @Vars 命名数值矩阵 */
	void registerVarsMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session) {
		registerScalarMatrix(matrix, session, ScalarKind::Vars, 1);
	}
};

/* ---- @Action 派发计划（拆自 runAction 的纯计算前段）----
 * 列头只解析一次（C-M3）：原实现在 order/相位检测/每行每格三处
 * 重复做 <device>.<param> 切分（切分规则单点在 mt::splitActionColumn）。
 * 设备列解析（resolveActionDevice）与参数位解析（resolveActionParam）：
 * 解析后的名字是派发身份——分组、顺序、相位、查重全部按解析名；
 * colHeads 保留原文供报错归因。
 * build 是纯计算（只读注册表，不碰硬件、不派发），可白盒单测。 */
struct ActionPlan {
	std::vector<std::pair<std::string, std::string>> colHeads;	/* 原始列头 <device>.<param> */
	std::vector<std::string> devs;	/* 解析后设备名（派发身份） */
	std::vector<std::string> params;	/* 解析后参数名 */
	std::vector<std::string> order;	/* 设备首现顺序（派发序） */
	/* 解析名查重（撞设备第二层防线）：归并期 checkDisjoint 只见列头文本，
	 * 不同别名/表达式指到同一 (设备, 参数) 只有这里可见。键 = 解析后的
	 * (设备, 参数)——同设备不同参数是合法的一拍多参（合并进同一次 play）；
	 * 同键重复则逐行静默覆盖（含 "-" 继承把两条时间线写进同一键），必须
	 * 显式报错。 */
	std::map<std::pair<std::string, std::string>, std::string> seenCell;
	/* 相位设备：该设备的参数列里存在真实数值行（典型来自 mtmerge 相位归并，
	 * 前导行是无可继承的 "-"、后续行有数值）。只有这类设备做未起拍跳过；
	 * 整列 "-" 的无参设备（旧式写法）保持旧行为——靠 0.0 照常 play，
	 * 否则依赖 play 触发读数/副作用的所有 E2E（守护快照等）会整体失效。 */
	std::set<std::string> phaseDevices;

	static ActionPlan build(const HwLookup& registry, const mt::MatrixObject& defs);
};

/* ---- 动作重放：@Action 逐行 play（R-1 拆自 RobotStruct）---- */
class ActionRunner {
	HwRegistry& registry_;

	/* C-M3：$(...) 表达式 AST 按列缓存——"-" 继承链上同一表达式文本
	 * 原实现每行每格重解析一次（O(行数²) 字符扫描）。body 为上次解析的
	 * 表达式文本，不同则重解析（继承传播的是同一文本，命中即 O(1)）。 */
	struct ColExprCache {
		std::string body;
		Expression ast;
	};

	/* N-10：按名查矩阵与列头切分下沉 mt 层（findByName / splitActionColumn） */
	void parseActionCell(Json& set, const mt::MatrixObject& actionDefs,
			std::int64_t i, std::int64_t j, const std::set<std::string>& phaseDevices,
			const std::string& resolvedParam,
			const std::string& resolvedDev, ColExprCache& exprCache);

public:
	explicit ActionRunner(HwRegistry& registry) : registry_(registry) {}

	/* ---- @Action 设备列解析单点（公开静态：白盒单测直接锚定）----
	 * 设备位允许写 @Alias（2026-09-19 设计评估）：解析穿透——别名不是播放
	 * 目标而是名字绑定，read() 出目标名后必须命中**非标量**硬件变量
	 * （驱动/守护），一层为止（别名指向别名报错）。未知名字原样返回：派发期
	 * 按既有「not defined (row N)」报错，保留行号上下文。@Vars 是数值常量，
	 * 永远不可能合法命名设备，显式拒绝而非落到 not defined。
	 * 设备位也接受 $(...) 表达式（2026-09-20 设计评估）：求值产物作为设备名
	 * 候选，走与裸名完全相同的解析——命中别名再解一层（$(p) ≡ p），产物
	 * 本身不再递归求值（别名值即使形如 "$(…)" 也只按文本查找，环无从建立）。
	 * 求值在派发期：主从覆盖改绑后的真值在此生效。
	 * 撞设备两层防线：归并期 checkDisjoint 按列头文本（同名在两个输入里都
	 * 出现），不同别名/表达式指到同一设备只有派发期可见——runAction 在解析
	 * 后按 (设备, 参数) 查重，是第二层的落点。 */
	static std::string resolveActionDevice(const HwLookup& registry, const std::string& dev);

	/* ---- @Action 参数位解析单点（公开静态：白盒单测直接锚定）----
	 * 参数位允许写 @Alias/@Vars 名（既有语义：命中标量文本即以其值为参数名）
	 * 与 $(...) 表达式（2026-09-20 设计评估）：求值产物即参数名，**不再解析**
	 * （参数名不是名字绑定）。派发期内别名/变量不变，每列解析一次。 */
	static std::string resolveActionParam(const HwLookup& registry, const std::string& param);

	/* 单矩阵动作派发核心：@Action 与 #34 @Reflex 共用同一派发逻辑 */
	void dispatchMatrix(const mt::MatrixObject& actionDefs);

	/* @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error） */
	void runAction(std::vector<mt::MatrixObject>& actionMatrices);

	/* #34 条件反射：@Trigger 逐行触发 @Reflex 动作序列，触发参数以临时
	 * 会话 @Alias 绑定（子作用域，执行完逐个撤销）；无 @Trigger 时反射
	 * 定义惰性存在（不执行、不校验引用）。 */
	void runReflex(std::vector<mt::MatrixObject>& actionMatrices);
};

/* ---- 热 SQL 计划数据：不持有硬件资源，提交段与只读计划层共用 ----
 * 行号/表下标与硬件指针均锚定建计划时的输入；调用方须在同一结构与注册表上
 * 立即执行一次，不得跨结构变更复用。指针只作为执行段的对象身份，不在计划期
 * 调用硬件方法；所有权始终属于注册表。 */
struct HotSqlPlan {
	struct Deletion {
		std::size_t matIdx = 0;		/* 输入矩阵下标 */
		std::string table;
		std::vector<std::size_t> rows;	/* 原始行号，升序 */
		std::vector<std::string> names;	/* 与 rows 平行：硬件变量名 */
		/* 与 names 平行：计划期从注册表取好的对象指针（pool_ 只增期间裸指针
		 * 稳定——对象在堆上，vector 扩容搬动的是 unique_ptr 不是对象本体）。 */
		std::vector<MatrixVars_Hardware*> hw;
	};

	/* 阶段 1.5 产物包：备删计划。前移到任何插入之前的理由（先删后建的两个
	 * 硬前提）在 planHotDeletes 实现处。 */
	struct Deletes {
		bool deleteFirst = false;		/* 批次含 UPDATE ⇒ 先删后建执行序 */
		std::set<std::string> doomedNames;	/* 将被摘除的变量名（阶段 2 重名预检豁免用） */
		std::vector<Deletion> dels;
		std::map<std::string, std::size_t> daemonNameIndex;	/* 守护名 → 声明序下标 */
		/* 表不在活体里 ⇒ 本批次 CREATE 出来的删除：撤销范围为空且必须为空，
		 * 但影子上的删除效果要落到待提交的新表上——对账读 newTables（阶段 2
		 * 的产物），只能延后到阶段 2.4（reconcileNewTableDels）。 */
		std::vector<std::pair<std::string, std::vector<std::size_t>>> newTableDels;
	};

	/* 阶段 2 产物包：建计划（新增行落位 + 新表 + 瞬态 Action），全部只读校验
	 * 通过后才存在；提交阶段按它增量装配。 */
	struct Insertions {
		/* (影子下标 [0, liveCount)，本批次新增行)——影子下标与输入矩阵一一对应 */
		std::vector<std::pair<std::size_t, std::vector<std::vector<std::string>>>> appends;
		std::vector<mt::MatrixObject> newTables;	/* 本批次新建的硬件表 */
		std::vector<mt::MatrixObject> actionMats;	/* 瞬态 Action（执行后即弃） */
	};

	ScriptLoader::Mutations mutations;
	Deletes deletions;
	Insertions insertions;
};

/* 解析 → 备删/插入计划 → 对账 → 合并预检。只依赖 const 结构视图和名字查询，
 * 不引用 RobotStruct、不持有 HwFactory；只读指硬件/结构只读，模块路径检测与
 * 既有告警仍会执行。装配期校验（如未知 $ 引用）仍由提交段负责，不提前改变
 * UPDATE 的失败口径。 */
class HotSqlPlanner {
	const std::deque<mt::MatrixObject>& matrices_;
	const HwLookup& registry_;

	HotSqlPlan::Deletes planHotDeletes(const ScriptLoader::Mutations& muts) const;
	HotSqlPlan::Insertions planHotInsertions(const std::deque<mt::MatrixObject>& staged,
		std::size_t liveCount, const ScriptLoader::Mutations& muts,
		const HotSqlPlan::Deletes& delPlan) const;
	static void reconcileNewTableDels(const std::deque<mt::MatrixObject>& staged,
		const HotSqlPlan::Deletes& delPlan, HotSqlPlan::Insertions& insPlan);
	void checkMergedView(const HotSqlPlan::Deletes& delPlan,
		const HotSqlPlan::Insertions& insPlan) const;

public:
	HotSqlPlanner(const std::deque<mt::MatrixObject>& matrices, const HwLookup& registry)
		: matrices_(matrices), registry_(registry) {}

	/* 成功返回即已通过原热 SQL 只读预检；无需 active() 或硬件装配。 */
	HotSqlPlan build(const std::string& sql) const;
};

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
