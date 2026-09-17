#include "robot.hpp"

#include <algorithm>
#include <iostream>

namespace bot {

/* ---- HwRegistry：结构期注册（重名即抛）---- */
void HwRegistry::add(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
		const std::string& source) {
	if (byName_.count(name)) {
		throw IllegalArgumentException("Duplicate hardware variable name: '" + name + "'" + source);
	}
	MatrixVars_Hardware* p = hw.get();
	byName_.emplace(name, p);
	seq_.push_back(p);
	pool_.push_back(std::move(hw));
}

/* ---- HwRegistry：会话期别名覆盖（不进销毁序，只改名字索引）---- */
void HwRegistry::override(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw) {
	MatrixVars_Hardware* p = hw.get();
	byName_[name] = p;
	pool_.push_back(std::move(hw));
}

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
			if (registry_->contains(refName)) {
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

Json MetaProvider::staticPart() {
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

	Json obj = staticPart();
	for (const std::string& name : *refs_) {
		if (MatrixVars_Hardware* hv = registry_->find(name)) {
			Json snap = hv->read();
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

/* F-04′：destroy 阶段的元数据收集——字面列 + 引用列的「尽力而为」快照。
 *
 * 为什么不能只取字面列（F-04 的原实现 collectStatic）：总线句柄 fd 与从机
 * address 都不是字面列，而是器件行经 `$i2c-N` / `$uart-N` 从接口器件的快照
 * 来的（modules/I2C.lua 的 call 下发 fd）。modules/lib/drv_common.lua 的
 * i2c_pair 要用它们，于是丢字段的后果按驱动分两档：
 *   * 走 i2c_pair 的 PCA9685 / MPU6050 / BH1750 —— destroy 响亮报错
 *     （Deactive warning: ... Invalid Argument: fd or address）；
 *   * 走 `if fd ~= nil ...` 守卫的 ADS1015 / ADS1115 / ADS1256 / APDS9900 /
 *     BME280 / BME688 / MAX30102 / MCP3421 / SGP30 / VL53L0X / VL53L4CD /
 *     SCSCL —— 静默跳过整个去初始化，器件滞留在最后指令状态且一声不响。
 *
 * 为什么可以放心解析引用：接口器件（I2C/SPI/UART）的 fd 只由它自己的 destroy
 * 关闭，而「总线必须先于器件声明」在 build 期就被强制了——器件 build 要用 fd，
 * 就必须让总线先 build（总线前向引用在 build 期即失败，见 ref_bus_forward 用例）。
 * 于是倒序销毁天然保证总线最后关闭，器件 destroy 期间 fd 始终有效。
 *
 * 与 F-04 的原始诉求不冲突：要守的是「destroy 这个调用绝不被跳过」，不是
 * 「不许解析引用」。这里对每个引用**单独** try——抛错（引用已被销毁）只跳过
 * 该引用本身。会被跳过的只可能是「数据前向引用」链（如 Monitor→Ema→Daemon，
 * 引用者在倒序里反而最后销毁），而 v0/out/count 这类字段本就不是器件去初始化
 * 所需。ERR 快照同理跳过（守护槽已移交时 readSample 按 ERR 契约返回对象而非抛错，
 * 把 message 混进去初始化参数没有意义）。 */
Json MetaProvider::collectForDestroy() {
	Json obj = staticPart();
	for (const std::string& name : *refs_) {
		MatrixVars_Hardware* hv = registry_->find(name);
		if (hv == nullptr) { continue; }
		Json snap = JsonObject();
		try {
			snap = hv->read();
		} catch (const std::exception& e) {
			logD([&] {
				return "destroy metadata: '" + matrix_.getName() + "' row " + std::to_string(row_)
					+ " skipped '$" + name + "' (already released: " + e.what() + ")";
			});
			continue;
		}
		if (!snap.isObj()) {
			logD([&] {
				return "destroy metadata: '" + matrix_.getName() + "' row " + std::to_string(row_)
					+ " skipped '$" + name + "' (non-object snapshot)";
			});
			continue;
		}
		auto rs = snap.get("result");
		if (rs && rs->isStr() && rs->asString().getValue() == "ERR") {
			logD([&] {
				return "destroy metadata: '" + matrix_.getName() + "' row " + std::to_string(row_)
					+ " skipped '$" + name + "' (reference reported ERR)";
			});
			continue;
		}
		jsonMergeInto(obj, snap.asObject());
	}
	return obj;
}

/* ==================== ScriptLoader：SQL/矩阵 文本 → MatrixObject ==================== */

/* ---- SQL4cj → MatrixObject 结构表解析 ---- */
void ScriptLoader::readSQL(bot::Reader& input, std::vector<mt::MatrixObject>& out) {
	std::vector<sql4cj::Token> tokens = sql4cj::Lexer::parseAll(input);
	sql4cj::Parser parser(tokens);
	for (;;) {
		sql4cj::Event eve = parser.next();
		if (eve.type == sql4cj::EventType::END) { break; }
		switch (eve.type) {
		case sql4cj::EventType::CREATE_TABLE: {
			/* R-4：SQL 事件里的 token 本就完整，直接灌入构建器，
			 * 不再逐字符 addRune 重放（仓颉流 API 的残留）。 */
			mt::MatrixBuilder b;
			b.name(eve.create.tableName);
			for (const auto& col : eve.create.tableColumns) { b.item(col.name); }
			b.line(0);
			out.push_back(b.take());
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
				std::vector<std::string> values;
				values.reserve(row.size());
				for (const auto& p : row) { values.push_back(p.value); }
				root->appendRow(std::move(values), 0);
			}
			break;
		}
		default:
			break;
		}
	}
}

/* ---- 读取 @Action/@Alias 矩阵外表：自省结局，不破坏读缓存 ---- */
void ScriptLoader::readMatrix(bot::Reader& file, std::vector<mt::MatrixObject>& out) {
	mt::MatrixParser parser(file);
	while (auto mat = parser.parse()) {
		out.push_back(std::move(*mat));
	}
}

/* ==================== HwFactory：矩阵表 → 硬件变量 ==================== */

/* ---- 安全添加硬件变量（不可覆盖：insert_or_assign 语义会造成隐式改名）---- */
void HwFactory::addHardwareVar(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
		const std::string& source) {
	registry_.add(name, std::move(hw), source);
}

/* ---- 注册别名矩阵：启动期(e=nullopt)与动作脚本期(e=Some)两路 ---- */
void HwFactory::registerAliasMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session) {
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
			if (registry_.contains(name)) {
				std::cerr << "Alias '" << name << "' defined in action script overrides the structure definition" << std::endl;
			}
			registry_.override(name, std::move(aliasVar));	/* 会话期覆盖式注册（结构定义被动作脚本别名覆盖） */
		} else {
			addHardwareVar(name, std::move(aliasVar), " (in @Alias matrix)");
		}
	}
}

/* ---- 全部硬变量创建：声明序，Alias/Lua/Daemon 分派 ---- */
void HwFactory::createHardVars(const std::vector<mt::MatrixObject>& matrices) {
	for (const auto& matrix : matrices) {
		std::string mtName = matrix.getName();
		if (mtName == "Alias") { registerAliasMatrix(matrix, nullptr); }
		else if (mtName == "Action") { /* 结构期 Action 由 MR 动作脚本提供 */ }
		else if (mtName == "Daemon") { createDaemonVars(matrix, mtName); }
		else if (matrix.rows() > 0) { createLuaVars(matrix, mtName); }
	}
}

void HwFactory::createDaemonVars(const mt::MatrixObject& matrix, const std::string& mtName) {
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

void HwFactory::createLuaVars(const mt::MatrixObject& matrix, const std::string& mtName) {
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
		std::string ctrlName;
		if (idxCtrl >= 0) {
			std::string rawCtrl = MatrixVars_Hardware::getMt(matrix, i, idxCtrl);
			if (rawCtrl != "-") {
				if (registry_.find(rawCtrl) == nullptr) {
					throw IllegalArgumentException("Hardware variable '" + name + "' declares controller '"
						+ rawCtrl + "' in table '" + mtName
						+ "', but it is not defined yet. The controller must be defined before the matrix that references it.");
				}
				ctrlName = rawCtrl;
			}
		}
		auto metaProv = std::make_shared<MetaProvider>(matrix, i, idxCtrl, &registry_);
		metaProviders_.push_back(metaProv);	/* F-05：供 active() 在 build 前统一校验 $ 引用 */
		addHardwareVar(name, MtVars_Lua::Factory(scriptPath, name,
			[metaProv](bool forDestroy) {
				return forDestroy ? metaProv->collectForDestroy() : metaProv->collect();
			}, ctrlName, &registry_),
			" (in table '" + mtName + "')");
	}
}

/* ==================== ActionRunner：@Action 逐行重放 ==================== */

/* ---- 按表名查找（trim 后比较）；不存在返回 empty ---- */
std::optional<mt::MatrixObject> ActionRunner::findMt(
		const std::vector<mt::MatrixObject>& ll, const std::string& name) {
	for (const auto& m : ll) {
		if (trim(m.getName()) == name) { return m; }
	}
	return std::nullopt;
}

/* ---- @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error）---- */
void ActionRunner::runAction(std::vector<mt::MatrixObject>& actionMatrices) {
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
			MatrixVars_Hardware* hv = registry_.find(name_dev);
			if (hv == nullptr) {
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
			Json res = hv->play(params);
			MatrixVars_Hardware::ensureOk(res, "play " + name_dev + " (row " + std::to_string(i) + ")");
		}
	}
}

/* ---- @Action 列头解析：<device>.<param> ---- */
std::pair<std::string, std::string> ActionRunner::parseActionColumn(const std::string& name_HV) {
	std::size_t dot = name_HV.find('.');
	if (dot == std::string::npos || name_HV.find('.', dot + 1) != std::string::npos) {
		throw IllegalArgumentException("Action column '" + name_HV
			+ "' is not valid (expected format: <device>.<param>)");
	}
	return { name_HV.substr(0, dot), name_HV.substr(dot + 1) };
}

/* ---- @Action 单元格解析（别名解析 + 数值类型解析 + 分组）---- */
void ActionRunner::parseActionCell(Json& set, const mt::MatrixObject& actionDefs,
		std::int64_t i, std::int64_t j) {
	auto [name_dev, name_param_orig] = parseActionColumn(actionDefs(0, j));
	std::string name_param = name_param_orig;

	double val_fp64 = MatrixVars_Hardware::parseMtDouble(actionDefs, i, j);

	Json ret = JsonObject();
	MatrixVars_Hardware* hit = registry_.find(name_param_orig);
	if (hit != nullptr && dynamic_cast<MtVars_Alias*>(hit)) {
		ret = hit->read();
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

/* ==================== RobotStruct：编排门面 ==================== */

/* ---- 构建硬件：两阶段（build 失败上抛，active 由 finally 清理）---- */
void RobotStruct::active() {
	factory_.createHardVars(matrices_);
	/* F-05：$ 引用解析本是惰性的（首次 collect 时）——指向未定义变量会在
	 * 驱动 build 的真机事务中途才报错；装配完成后、任何 build 之前统一
	 * 校验存在性（此时全部变量已注册，前向引用合法）。 */
	for (const auto& p : metaProviders_) { p->validate(); }
	for (MatrixVars_Hardware* item : registry_.sequence()) {
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
}

/* ---- 执行 @Action 矩阵 + 内联 @Alias ---- */
void RobotStruct::run(bot::Reader& input) {
	std::vector<mt::MatrixObject> actionMatrices;
	ScriptLoader::readMatrix(input, actionMatrices);

	std::set<std::string> session;
	for (auto& matrix : actionMatrices) {
		if (trim(matrix.getName()) == "Alias") { factory_.registerAliasMatrix(matrix, &session); }
	}
	runner_.runAction(actionMatrices);
}

} // namespace bot
