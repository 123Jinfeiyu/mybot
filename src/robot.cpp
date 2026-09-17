#include "robot.hpp"

#include <algorithm>
#include <iostream>

namespace bot {

/* ---- MetaProvider：P8/M2 元数据收集 ----
 * 字面列与 "-" 继承结果首采冻结，之后每次浅拷贝重放；
 * "-" 整列缺省 → 跳过（Lua 侧 tonumber(nil) or 缺省）；
 * name 列、controller(skipCol) 列 → 跳过；
 * 引用列（值以 $ 前缀显式标记）→ 运行期每次实时 read() 快照，不入缓存。 */
void MetaProvider::seed() {
	if (cols_) { return; }
	std::vector<std::string> cArr, vArr, rArr;
	std::int64_t idxName = matrix_.indexOfCol(0, "name");
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(matrix_.cols()); ++j) {
		const std::string& colName = matrix_(0, j);
		std::string value = MatrixVars_Hardware::getMt(matrix_, row_, j);
		if (value == "-") { continue; }
		if (j == idxName || j == skipCol_) { continue; }
		if (value.rfind("$", 0) == 0) {
			std::string refName = value.substr(1);
			if (hardwareVars_->count(refName)) {
				rArr.push_back(refName);
			} else {
				throw IllegalArgumentException("'" + value + "' (column '" + colName
					+ "') in table '" + matrix_.getName() + "' row " + std::to_string(row_)
					+ ": references hardware variable '" + refName + "', but it is not defined. ('$' may forward-reference a variable declared later in the script, but the name must exist; checked after all tables are registered and before any driver build.)");
			}
		} else {
			cArr.push_back(colName);
			vArr.push_back(value);
		}
	}
	cols_ = std::move(cArr);
	vals_ = std::move(vArr);
	refs_ = std::move(rArr);
}

/* C1 防线：$ 引用列经 read() → getMetadata_() → collect() 互递归取实时快照，
 * 环配置（如 A.src=$B 且 B.src=$A，或自引用）会无限递归直到栈溢出。
 * 用线程局部深度计数兜底：正常引用链深度 1，超限即判定为环并清晰报错。 */
namespace {
thread_local int t_refDepth = 0;
constexpr int kMaxRefDepth = 32;
struct RefDepthGuard {
	~RefDepthGuard() { --t_refDepth; }
};
} // namespace

Json MetaProvider::collectStatic() {
	seed();
	Json obj = JsonObject();
	auto& colsArr = *cols_;
	auto& valsArr = *vals_;
	for (std::size_t k = 0; k < colsArr.size(); ++k) {
		obj.put(colsArr[k], JsonString(valsArr[k]));
	}
	return obj;
}

Json MetaProvider::collect() {
	if (t_refDepth >= kMaxRefDepth) {
		throw IllegalArgumentException("circular '$' reference detected: reference chain exceeds "
			+ std::to_string(kMaxRefDepth) + " levels while resolving table '" + matrix_.getName()
			+ "' row " + std::to_string(row_)
			+ ". Check that '$' reference columns do not form a cycle (a variable referencing itself, directly or indirectly).");
	}
	++t_refDepth;
	RefDepthGuard depthGuard;

	Json obj = collectStatic();
	for (const std::string& name : *refs_) {
		auto it = hardwareVars_->find(name);
		if (it != hardwareVars_->end()) {
			Json snap = it->second->read();
			if (!snap.isObj()) {
				throw IllegalArgumentException("'$" + name
					+ "': referenced variable returned a non-object (scalar) snapshot; "
					+ "'$' reference columns merge field snapshots (v0.., status, count), "
					+ "scalar values are only supported via 'controller' columns.");
			}
			jsonMergeInto(obj, snap.asObject());
		}
	}
	return obj;
}

/* ---- MatrixObject 工具：第 r 行第 colName 列下标（缺失返回 -1）---- */

/* ---- 安全添加硬件变量（不可覆盖：insert_or_assign 语义会造成隐式改名）---- */
void RobotStruct::addHardwareVar(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
		const std::string& source) {
	if (hardwareVars_.count(name)) {
		throw IllegalArgumentException("Duplicate hardware variable name: '" + name + "'" + source);
	}
	MatrixVars_Hardware* p = hw.get();
	hardwareVars_.emplace(name, p);
	hwSeq_.push_back(p);
	hwPool_.push_back(std::move(hw));
}

/* ---- 注册别名矩阵：启动期(e=nullopt)与动作脚本期(e=Some)两路 ---- */
void RobotStruct::registerAliasMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session) {
	std::int64_t idxName = matrix.indexOfCol(0, "name");
	if (idxName < 0) {
		throw IllegalArgumentException("Alias matrix '" + matrix.getName() + "' has no 'name' column.");
	}
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		auto aliasVar = MtVars_Alias::Factory(matrix, i);
		if (session) {
			if (session->count(name)) {
				throw IllegalArgumentException("Duplicate alias name '" + name + "' within the action script");
			}
			session->insert(name);
			if (hardwareVars_.count(name)) {
				std::cerr << "Alias '" << name << "' defined in action script overrides the structure definition" << std::endl;
			}
			MatrixVars_Hardware* p = aliasVar.get();
			hardwareVars_[name] = p;	/* 会话期覆盖式注册（结构定义被动作脚本别名覆盖） */
			hwPool_.push_back(std::move(aliasVar));
		} else {
			addHardwareVar(name, std::move(aliasVar), " (in @Alias matrix)");
		}
	}
}

/* ---- 全部硬变量创建：声明序，Alias/Lua/Daemon 分派 ---- */
void RobotStruct::createHardVars() {
	for (const auto& matrix : matrices_) {
		std::string mtName = matrix.getName();
		if (mtName == "Alias") { registerAliasMatrix(matrix, nullptr); }
		else if (mtName == "Action") { /* 结构期 Action 由 MR 动作脚本提供 */ }
		else if (mtName == "Daemon") { createDaemonVars(matrix, mtName); }
		else if (matrix.rows() > 0) { createLuaVars(matrix, mtName); }
	}
}

void RobotStruct::createDaemonVars(const mt::MatrixObject& matrix, const std::string& mtName) {
	std::int64_t idxName = matrix.indexOfCol(0, "name");
	if (idxName < 0) {
		throw IllegalArgumentException("Table '" + mtName + "' has no 'name' column.");
	}
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		precheckDuplicate(name, mtName);
		addHardwareVar(name, MtVars_Daemon::Factory(matrix, i, daemonHolders_),
			" (in table '" + mtName + "')");
	}
}

void RobotStruct::createLuaVars(const mt::MatrixObject& matrix, const std::string& mtName) {
	std::int64_t idxName = matrix.indexOfCol(0, "name");
	if (idxName < 0) {
		throw IllegalArgumentException("Table '" + mtName + "' has no 'name' column.");
	}
	std::int64_t idxCtrl = matrix.indexOfCol(0, "controller");
	auto modulePathOpt = getEnvVar("BOT4CJ_MODULE_PATH");
	std::string modulePath = modulePathOpt.value_or("./modules");
	std::string scriptPath = firstExistingModule(modulePath, mtName + ".lua");
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		precheckDuplicate(name, mtName);
		MatrixVars_Hardware* ctrl = nullptr;
		if (idxCtrl >= 0) {
			std::string ctrlName = MatrixVars_Hardware::getMt(matrix, i, idxCtrl);
			if (ctrlName != "-") {
				auto it = hardwareVars_.find(ctrlName);
				if (it != hardwareVars_.end()) { ctrl = it->second; }
				else {
					throw IllegalArgumentException("Hardware variable '" + name + "' declares controller '"
						+ ctrlName + "' in table '" + mtName
						+ "', but it is not defined yet. The controller must be defined before the matrix that references it.");
				}
			}
		}
		auto metaProv = std::make_shared<MetaProvider>(matrix, i, idxCtrl, &hardwareVars_);
		metaProviders_.push_back(metaProv);	/* F-05：供 active() 在 build 前统一校验 $ 引用 */
		addHardwareVar(name, MtVars_Lua::Factory(scriptPath, name,
			[metaProv](bool staticOnly) {
				return staticOnly ? metaProv->collectStatic() : metaProv->collect();
			}, ctrl),
			" (in table '" + mtName + "')");
	}
}

/* ---- 按表名查找（trim 后比较）；不存在返回 empty ---- */
std::optional<mt::MatrixObject> RobotStruct::findMt(
		const std::vector<mt::MatrixObject>& ll, const std::string& name) {
	for (const auto& m : ll) {
		if (trim(m.getName()) == name) { return m; }
	}
	return std::nullopt;
}

/* ---- 读取 @Action/@Alias 矩阵外表：自省结局，不破坏读缓存 ---- */
void RobotStruct::readMatrix(bot::Reader& file, std::vector<mt::MatrixObject>& out) {
	mt::MatrixParser parser(file);
	while (auto mat = parser.parse()) {
		out.push_back(std::move(*mat));
	}
}

/* ---- 构建硬件：两阶段（build 失败上抛，active 由 finally 清理）---- */
void RobotStruct::active() {
	createHardVars();
	/* F-05：$ 引用解析本是惰性的（首次 collect 时）——指向未定义变量会在
	 * 驱动 build 的真机事务中途才报错；装配完成后、任何 build 之前统一
	 * 校验存在性（此时全部变量已注册，前向引用合法）。 */
	for (const auto& p : metaProviders_) { p->validate(); }
	for (MatrixVars_Hardware* item : hwSeq_) {
		item->build();
	}
}

/* ---- 消除硬件：先统一置守护 stop（不逐个 join，避免同步饿死），再逆序 destroy ---- */
void RobotStruct::deactivate() {
	for (const auto& h : daemonHolders_) {
		daemon_request_stop(h->slot.load());
	}
	std::string lastWarn;
	std::size_t failed = 0;
	for (auto it = hwSeq_.rbegin(); it != hwSeq_.rend(); ++it) {
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
		std::cerr << "Deactivate: " << failed << " of " << hwSeq_.size()
			<< " hardware variable(s) failed to destroy (see warnings above)" << std::endl;
	}
}

/* ---- 执行 @Action 矩阵 + 内联 @Alias ---- */
void RobotStruct::run(bot::Reader& input) {
	std::vector<mt::MatrixObject> actionMatrices;
	readMatrix(input, actionMatrices);

	std::set<std::string> session;
	for (auto& matrix : actionMatrices) {
		if (trim(matrix.getName()) == "Alias") { registerAliasMatrix(matrix, &session); }
	}
	runAction(actionMatrices);
}

/* ---- @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error）---- */
void RobotStruct::runAction(std::vector<mt::MatrixObject>& actionMatrices) {
	auto actionDefsOpt = findMt(actionMatrices, "Action");
	if (!actionDefsOpt) {
		throw IllegalArgumentException("No @Action matrix found in the script. Please define @Action in your .mt file.");
	}
	mt::MatrixObject& actionDefs = *actionDefsOpt;

	std::vector<std::string> order;
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
		auto [name_dev, name_param] = parseActionColumn(actionDefs(0, j));
		if (std::find(order.begin(), order.end(), name_dev) == order.end()) {
			order.push_back(name_dev);
		}
	}

	for (std::int64_t i = 1; i < static_cast<std::int64_t>(actionDefs.rows()); ++i) {
		/* D4：SIGINT/SIGTERM 后停止派发新动作帧，让 deactivate 清理链正常执行 */
		if (stopRequested()) {
			std::cerr << "Stop requested, halting action dispatch (row " << i << ")" << std::endl;
			break;
		}
		Json set = JsonObject();
		for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
			parseActionCell(set, actionDefs, i, j);
		}
		for (const std::string& name_dev : order) {
			auto it = hardwareVars_.find(name_dev);
			if (it == hardwareVars_.end()) {
				throw IllegalArgumentException("Hardware variable '" + name_dev
					+ "' is used in @Action (row " + std::to_string(i)
					+ ") but not defined in your hardware declaration section.");
			}
			auto paramsOpt = set.get(name_dev);
			if (!paramsOpt) {
				throw IllegalArgumentException("Parameter for hardware variable '" + name_dev
					+ "' is missing in action row " + std::to_string(i) + ".");
			}
			const Json& params = *paramsOpt;
			Json res = it->second->play(params);
			MatrixVars_Hardware::ensureOk(res, "play " + name_dev + " (row " + std::to_string(i) + ")");
		}
	}
}

/* ---- @Action 列头解析：<device>.<param> ---- */
std::pair<std::string, std::string> RobotStruct::parseActionColumn(const std::string& name_HV) {
	std::size_t dot = name_HV.find('.');
	if (dot == std::string::npos || name_HV.find('.', dot + 1) != std::string::npos) {
		throw IllegalArgumentException("Action column '" + name_HV
			+ "' is not valid (expected format: <device>.<param>)");
	}
	return { name_HV.substr(0, dot), name_HV.substr(dot + 1) };
}

/* ---- @Action 单元格解析（别名解析 + 数值类型解析 + 分组）---- */
void RobotStruct::parseActionCell(Json& set, const mt::MatrixObject& actionDefs,
		std::int64_t i, std::int64_t j) {
	auto [name_dev, name_param_orig] = parseActionColumn(actionDefs(0, j));
	std::string name_param = name_param_orig;

	double val_fp64 = MatrixVars_Hardware::parseMtDouble(actionDefs, i, j);

	Json ret = JsonObject();
	auto hit = hardwareVars_.find(name_param_orig);
	if (hit != hardwareVars_.end() && dynamic_cast<MtVars_Alias*>(hit->second)) {
		ret = hit->second->read();
		name_param = ret.asString().getValue();
	}

	Json set_params;
	auto obj_ = set.get(name_dev);
	if (obj_) {
		set_params = *obj_;	/* 拷贝：asObject() 引用临时副本会悬垂，需写回 */
	} else {
		set_params = JsonObject();
		set.put(name_dev, JsonObject());
	}
	set_params.put(name_param, JsonFloat(val_fp64));
	set.put(name_dev, set_params);
}

/* ---- SQL4cj → MatrixObject 结构表解析 ---- */
void RobotStruct::readSQL(bot::Reader& input, std::vector<mt::MatrixObject>& out) {
	std::vector<sql4cj::Token> tokens = sql4cj::Lexer::parseAll(input);
	sql4cj::Parser parser(tokens);
	for (;;) {
		sql4cj::Event eve = parser.next();
		if (eve.type == sql4cj::EventType::END) { break; }
		switch (eve.type) {
		case sql4cj::EventType::CREATE_TABLE: {
			mt::MatrixObject m;
			for (char ch : eve.create.tableName) { m.addRune(ch); }
			m.submitName();
			for (const auto& col : eve.create.tableColumns) {
				for (char ch : col.name) { m.addRune(ch); }
				m.submitItem();
			}
			m.submitLine(0);
			out.push_back(std::move(m));
			break;
		}
		case sql4cj::EventType::INSERT_TABLE: {
			if (out.empty()) { throw std::runtime_error("No table created yet."); }
			/* INSERT 目标表必须已创建：按名定位，拼错/未定义即解析期报错
			 * （对照 robots/test/insert_typo.sql 的负路径契约） */
			mt::MatrixObject* root = nullptr;
			for (auto& m : out) {
				if (trim(m.getName()) == eve.insert.targetTabName) { root = &m; break; }
			}
			if (root == nullptr) {
				throw IllegalArgumentException(
					"INSERT INTO '" + eve.insert.targetTabName + "': table is not defined");
			}
			for (const auto& row : eve.insert.tableRows) {
				for (const auto& p : row) {
					for (char ch : p.value) { root->addRune(ch); }
					root->submitItem();
				}
				root->submitLine(0);
			}
			break;
		}
		default:
			break;
		}
	}
}

} // namespace bot