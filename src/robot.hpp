#pragma once
/* robot.hpp —— 机器人构成模块（移植 bot4cj/src/RobotStruct.cj）
 * 结构脚本（SQL）→ MatrixObject 表 → 硬件变量注册表；
 * @Action 逐行重放，controller/Alias/守护 机制在此装配。
 * R-1：RobotStruct 收敛为编排门面；脚本装载（ScriptLoader）、硬件装配
 * （HwFactory）、动作重放（ActionRunner）三个协作对象各司其职。
 */

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "daemonvar.hpp"
#include "hardware.hpp"
#include "json.hpp"
#include "luavar.hpp"
#include "matrix.hpp"
#include "sqlparse.hpp"
#include "util.hpp"

namespace bot {

/* R-2：硬件变量注册表——所有权、销毁序、名字索引的单一来源。
 * 旧实现三个并行容器（hardwareVars_/hwPool_/hwSeq_）靠 addHardwareVar
 * 手工同步；会话期别名覆盖直接改写 map，不变式被打破。 */
class HwRegistry {
	std::vector<std::unique_ptr<MatrixVars_Hardware>> pool_;	/* 所有权（含 run 期别名） */
	std::vector<MatrixVars_Hardware*> seq_;			/* 销毁序：结构声明序，active/deactivate 按序 */
	std::map<std::string, MatrixVars_Hardware*> byName_;	/* 名字索引 */
	/* N-6：被引用为 controller 的名字集合——会话期覆盖不得触碰，
	 * 否则 controller 转发按名查到别名（MtVars_Alias），报出的
	 * "non-table result" 与真实成因（控制器被覆盖）完全脱节。 */
	std::set<std::string> controllerRefs_;

public:
	/* 结构期注册：重名即抛（source 指明声明来源，如 " (in table 'X')"） */
	void add(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
			const std::string& source = "");

	/* 会话期别名覆盖：不进入销毁序（别名无资源可释放），只改写名字索引。
	 * 被覆盖的结构变量仍在销毁序中，deactivate 照常清理。
	 * 名字被引用为 controller 时拒绝覆盖（N-6）。 */
	void override(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw);

	/* 登记 controller 引用（createLuaVars 装配期调用） */
	void addControllerRef(const std::string& name) { controllerRefs_.insert(name); }

	/* N-15：表达式标识符求值器（只认 @Vars 数值；未定义/非 @Vars 即抛）——
	 * MetaProvider 的 $(...) 求值、HwFactory 装配、runAction 表达式列共用 */
	std::function<double(const std::string&)> makeVarsResolver() const;

	MatrixVars_Hardware* find(const std::string& name) const {
		auto it = byName_.find(name);
		return it == byName_.end() ? nullptr : it->second;
	}

	bool contains(const std::string& name) const { return byName_.count(name) > 0; }

	/* 销毁序视图（build 正向 / destroy 反向）；会话别名不在其中 */
	const std::vector<MatrixVars_Hardware*>& sequence() const { return seq_; }

	/* agent 模式：只读名字索引（硬件清单）。只给 const 视图——
	 * 清单是「描述」用途，不应成为第二条改写注册表的路径。 */
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

/* P8/M2：元数据静态列缓存（字面列冻结 + 引用列实时 read() 快照） */
class MetaProvider {
	const mt::MatrixObject& matrix_;
	std::int64_t row_;
	std::int64_t skipCol_;
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

public:
	MetaProvider(const mt::MatrixObject& matrix, std::int64_t row, std::int64_t skipCol,
			const HwRegistry* registry)
		: matrix_(matrix), row_(row), skipCol_(skipCol), registry_(registry) {}

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
	/* SQL4cj → MatrixObject 结构表解析 */
	static void readSQL(bot::Reader& input, std::vector<mt::MatrixObject>& out);
};

/* ---- 硬件装配：矩阵表 → 硬件变量（R-1 拆自 RobotStruct）---- */
class HwFactory {
	HwRegistry& registry_;
	std::vector<std::shared_ptr<DaemonRunner>>& daemonHolders_;
	std::vector<std::shared_ptr<MetaProvider>>& metaProviders_;

	/* 安全添加硬件变量（查重复键；source 指明声明来源） */
	void addHardwareVar(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
			const std::string& source = "");

	/* 重名前置判重（资源泄漏防线：Lua 状态机/共享槽先申请，此处先确认可注册） */
	void precheckDuplicate(const std::string& name, const std::string& mtName) {
		if (registry_.contains(name)) {
			throw IllegalArgumentException("Duplicate hardware variable name: '" + name
				+ "' (in table '" + mtName + "')");
		}
	}

	void createDaemonVars(const mt::MatrixObject& matrix, const std::string& mtName);
	void createLuaVars(const mt::MatrixObject& matrix, const std::string& mtName);

	/* 标量文本矩阵种类：@Alias(参数别名的目标) / @Vars(命名数值) */
	enum class ScalarKind { Alias, Vars };

	/* 注册标量文本矩阵的公共实现（启动期/动作脚本期两路，见实现处） */
	void registerScalarMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session,
			ScalarKind kind);

public:
	HwFactory(HwRegistry& registry,
			std::vector<std::shared_ptr<DaemonRunner>>& daemonHolders,
			std::vector<std::shared_ptr<MetaProvider>>& metaProviders)
		: registry_(registry), daemonHolders_(daemonHolders), metaProviders_(metaProviders) {}

	/* 全部硬变量创建：声明序，Alias/Vars/Lua/Daemon 分派 */
	void createHardVars(const std::vector<mt::MatrixObject>& matrices);

	/* 注册 @Alias 别名矩阵 */
	void registerAliasMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session) {
		registerScalarMatrix(matrix, session, ScalarKind::Alias);
	}

	/* 注册 @Vars 命名数值矩阵 */
	void registerVarsMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session) {
		registerScalarMatrix(matrix, session, ScalarKind::Vars);
	}
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
			const std::pair<std::string, std::string>& colHead, ColExprCache& exprCache);

public:
	explicit ActionRunner(HwRegistry& registry) : registry_(registry) {}

	/* @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error） */
	void runAction(std::vector<mt::MatrixObject>& actionMatrices);
};

/* ---- 机器人编排门面：装配流程 + 生命周期（R-1 后只剩这些）---- */
class RobotStruct {
	std::vector<mt::MatrixObject> matrices_;	/* 结构脚本解析的矩阵对象（声明序） */
	std::vector<std::shared_ptr<DaemonRunner>> daemonHolders_;	/* 守护注册表：deactivate 先统一置 stop；shared_ptr 供守护线程持 self 保生命周期 */
	HwRegistry registry_;				/* 硬件变量：所有权 + 销毁序 + 名字索引（R-2） */
	/* F-05：Lua 变量的元数据提供者登记表——active() 在 build 前统一校验
	 * $ 引用存在性（shared_ptr 与变量同生命周期，防悬垂） */
	std::vector<std::shared_ptr<MetaProvider>> metaProviders_;
	HwFactory factory_;	/* 依赖上面的容器，声明序保证先构造 */
	ActionRunner runner_;

public:
	explicit RobotStruct(bot::Reader& input)
		: factory_(registry_, daemonHolders_, metaProviders_), runner_(registry_) {
		ScriptLoader::readSQL(input, matrices_);
	}

	/* 激活所有硬件变量（两阶段构造：解析 → 创建 + build） */
	void active();

	/* 消除所有硬件变量（先统一置守护 stop 防饿死，再按声明序倒序 destroy） */
	void deactivate();

	/* 执行 @Action 矩阵（含内联 @Alias 注册） */
	void run(bot::Reader& input);

	/* agent 模式：只读硬件注册表视图（清单/种类），供工具输出与提示词使用 */
	const HwRegistry& registry() const { return registry_; }

	/* 储存整个矩阵变量表（预留 API，供 NN 矩阵脚本存储） */
	void save(std::ostream& out) { mt::MatrixSaver::save(matrices_, out); }
};

} // namespace bot
