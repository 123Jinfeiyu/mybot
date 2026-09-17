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

public:
	/* 结构期注册：重名即抛（source 指明声明来源，如 " (in table 'X')"） */
	void add(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
			const std::string& source = "");

	/* 会话期别名覆盖：不进入销毁序（别名无资源可释放），只改写名字索引。
	 * 被覆盖的结构变量仍在销毁序中，deactivate 照常清理。 */
	void override(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw);

	MatrixVars_Hardware* find(const std::string& name) const {
		auto it = byName_.find(name);
		return it == byName_.end() ? nullptr : it->second;
	}

	bool contains(const std::string& name) const { return byName_.count(name) > 0; }

	/* 销毁序视图（build 正向 / destroy 反向）；会话别名不在其中 */
	const std::vector<MatrixVars_Hardware*>& sequence() const { return seq_; }
};

/* P8/M2：元数据静态列缓存（字面列冻结 + 引用列实时 read() 快照） */
class MetaProvider {
	const mt::MatrixObject& matrix_;
	std::int64_t row_;
	std::int64_t skipCol_;
	const HwRegistry* registry_;
	std::optional<std::vector<std::string>> cols_;
	std::optional<std::vector<std::string>> vals_;
	std::optional<std::vector<std::string>> refs_;

	void seed();

public:
	MetaProvider(const mt::MatrixObject& matrix, std::int64_t row, std::int64_t skipCol,
			const HwRegistry* registry)
		: matrix_(matrix), row_(row), skipCol_(skipCol), registry_(registry) {}

	Json collect();
	/* F-04：仅静态列（不解析 $ 引用）——destroy 阶段用 */
	Json collectStatic();
	/* F-05：装配完成后、build 前做引用存在性校验（seed 未通过即抛出） */
	void validate() { seed(); }
};

/* ---- 脚本装载：SQL/矩阵 文本 → MatrixObject（R-1 拆自 RobotStruct）---- */
class ScriptLoader {
public:
	/* SQL4cj → MatrixObject 结构表解析 */
	static void readSQL(bot::Reader& input, std::vector<mt::MatrixObject>& out);

	/* 读取 @Action/@Alias 矩阵外表：自省结局，不破坏读缓存 */
	static void readMatrix(bot::Reader& file, std::vector<mt::MatrixObject>& out);
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

public:
	HwFactory(HwRegistry& registry,
			std::vector<std::shared_ptr<DaemonRunner>>& daemonHolders,
			std::vector<std::shared_ptr<MetaProvider>>& metaProviders)
		: registry_(registry), daemonHolders_(daemonHolders), metaProviders_(metaProviders) {}

	/* 全部硬变量创建：声明序，Alias/Lua/Daemon 分派 */
	void createHardVars(const std::vector<mt::MatrixObject>& matrices);

	/* 注册别名矩阵：启动期(session=nullptr)与动作脚本期(session)两路 */
	void registerAliasMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session);
};

/* ---- 动作重放：@Action 逐行 play（R-1 拆自 RobotStruct）---- */
class ActionRunner {
	HwRegistry& registry_;

	static std::optional<mt::MatrixObject> findMt(
			const std::vector<mt::MatrixObject>& ll, const std::string& name);
	static std::pair<std::string, std::string> parseActionColumn(const std::string& name_HV);
	void parseActionCell(Json& set, const mt::MatrixObject& actionDefs,
			std::int64_t i, std::int64_t j);

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

	/* 储存整个矩阵变量表（预留 API，供 NN 矩阵脚本存储） */
	void save(std::ostream& out) { mt::MatrixSaver::save(matrices_, out); }
};

} // namespace bot
