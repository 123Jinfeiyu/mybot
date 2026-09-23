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
	const mt::MatrixObject& matrix_;
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
		: matrix_(matrix), row_(row), registry_(registry) {}

	Json collect();
	/* F-04′：字面列 + 引用列的「尽力而为」快照——destroy 阶段用。
	 * 单个引用解析失败只跳过该引用，绝不放弃整轮 destroy（理由见 robot.cpp 实现处）。 */
	Json collectForDestroy();
	/* F-05：装配完成后、build 前做引用存在性校验（seed 未通过即抛出） */
	void validate() { seed(); }
};

/* ---- 脚本装载：SQL/矩阵 文本 → MatrixObject（R-1 拆自 RobotStruct）----
 * N-11：矩阵文本解析（readMatrix）已下沉 mt 层，本类只剩 SQL 事件装配。 */
class ScriptLoader {
public:
	/* SQL4cj → MatrixObject 结构表解析。
	 * 容器为 std::deque（2026-09-23 热插表）：结构期 matrices_ 必须是
	 * 「两端插入不失效元素引用」的容器，MetaProvider::matrix_ 的引用不变式
	 * 才能从「靠 inject 必须早于 active 的约定」升级为构造保证（见 robot.cpp）。 */
	static void readSQL(bot::Reader& input, std::deque<mt::MatrixObject>& out);
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
	void checkSingleConsumer(const std::deque<mt::MatrixObject>& matrices);

	/* F5-4：同一物理串口被 2+ 个「各自 open 独立 fd」的消费者（UART 总线变量 /
	 * 真机守护）声明时的加载期告警——HAL 不做跨变量引用计数，后关闭者以自己
	 * open 时的配置为准。只告警不拒绝（同配置轮询下实际可用；是否收紧属
	 * 破坏性决策）。实现与理由见 hw_factory.cpp 的同名函数注释。 */
	void warnSharedUartPaths(const std::deque<mt::MatrixObject>& matrices);

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
	 * 内部同样调用这两个。开放此入口是给 RobotStruct::applySql：热插批次必须
	 * 能在**提交任何变更之前**，对「提交之后的结构」复跑同一判据。
	 * 注意：判据看的是整张结构，故调用方传入的必须是合并后的视图。 */
	void precheckStructure(const std::deque<mt::MatrixObject>& matrices);

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

	/* @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error） */
	void runAction(std::vector<mt::MatrixObject>& actionMatrices);
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

	/* ---- 热插表：结构 SQL（CREATE/INSERT）在上电之后真正生效（2026-09-23）----
	 * 契约：
	 *  1) sql 是**一条或一批**结构 SQL（CREATE TABLE / INSERT INTO；可混排），
	 *     INSERT 的目标可以是本批次新建的表，也可以是结构里既有表；
	 *  2) 前置条件 active() 已成功（机器人上电常驻）——热插是「带电变更」，
	 *     未上电时直接用结构脚本，不走这里；
	 *  3) 事务性：**全成功或全回滚**。任何一步失败（校验/装配/build/动作执行）
	 *     都逆序 destroy 本批次新建的硬件、撤销注册与矩阵行，抛异常说明成因；
	 *     动作已下发的物理副作用不回退（与「已执行的动作不回退」同口径）；
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

	/* agent 模式：结构脚本未声明某个 Lua 驱动变量时注入一个（须在 active() 之前调用）。
	 * LLM 协议驱动与摄像头驱动都走这里。
	 * 复用脚本装载同一条路径（合成 SQL 文本 → ScriptLoader::readSQL → 追加矩阵），
	 * 免得为「多一个变量」再造一套矩阵构造 API。
	 * driverTable 同时是驱动文件名（modules/<driverTable>.lua），与变量名都须是
	 * 安全标识符——否则合成出来的 SQL 会被注入出别的表。 */
	void injectLuaDriver(const std::string& driverTable, const std::string& varName);

};

} // namespace bot
