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
	/* N-6：被引用为 controller 的名字不得覆盖——否则 controller 转发按名
	 * 查到别名（MtVars_Alias），报出的 "non-table result" 与真实成因
	 * （控制器被动作脚本覆盖）完全脱节。 */
	if (controllerRefs_.count(name)) {
		throw IllegalArgumentException("action script alias must not override '"
			+ name + "': it is referenced as a controller by another hardware variable");
	}
	MatrixVars_Hardware* p = hw.get();
	byName_[name] = p;
	pool_.push_back(std::move(hw));
}

/* N-15：表达式标识符求值器单点——三个调用面（元数据 $(...)、@Vars 装配、
 * 动作表达式列）此前各自内联同一形态的 lambda。 */
namespace {
double resolveVarsNumber(const HwRegistry* reg, const std::string& name);
} // namespace

std::function<double(const std::string&)> HwRegistry::makeVarsResolver() const {
	const HwRegistry* reg = this;
	return [reg](const std::string& nm) { return resolveVarsNumber(reg, nm); };
}

/* 表达式标识符求值：查注册表，必须是 @Vars（数值），递归取数。
 * 环（A→B→A）由 MtVars_Vars::numericValue 的深度计数兜底。 */
namespace {
double resolveVarsNumber(const HwRegistry* reg, const std::string& name) {
	MatrixVars_Hardware* hv = reg->find(name);
	if (hv == nullptr) {
		throw IllegalArgumentException("expression references '" + name
			+ "', but it is not a defined hardware variable");
	}
	MtVars_Vars* v = dynamic_cast<MtVars_Vars*>(hv);
	if (v == nullptr) {
		throw IllegalArgumentException("expression references '" + name
			+ "', which is not an @Vars; only numeric @Vars may appear in an expression");
	}
	return v->numericValue();
}
} // namespace

/* ---- MetaProvider：P8/M2 元数据收集 ----
 * 字面列与 "-" 继承结果首采冻结，之后每次浅拷贝重放；
 * "-" 整列缺省 → 跳过（Lua 侧 tonumber(nil) or 缺省）；
 * name 列、controller(skipCol) 列 → 跳过；
 * 引用列（值以 $ 前缀显式标记）→ 运行期每次实时 read() 快照，不入缓存；
 * $(...) 表达式引用 → seed 期解析/校验标识符，collect 期惰性求值。 */
void MetaProvider::seed() {
	if (cols_) { return; }
	std::vector<std::string> cArr, vArr;
	std::vector<MetaRef> rArr;
	std::int64_t idxName = matrix_.indexOfCol(0, "name");
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(matrix_.cols()); ++j) {
		const std::string& colName = matrix_(0, j);
		std::string value = matrix_.inherited(row_, j);
		if (value == "-") { continue; }
		if (j == idxName || j == skipCol_) { continue; }
		if (value.rfind("$", 0) == 0) {
			if (auto body = exprBody(value)) {
				MetaRef ref;
				ref.col = colName;
				ref.expr = std::make_shared<Expression>(Expression::parse(*body));
				/* 标识符存在性：与 $name 一样在 build 前统一校验（允许前向引用） */
				std::set<std::string> names;
				ref.expr->collectVars(names);
				for (const std::string& nm : names) {
					if (!registry_->contains(nm)) {
						throw IllegalArgumentException("'" + value + "' (column '" + colName
							+ "') in table '" + matrix_.getName() + "' row " + std::to_string(row_)
							+ ": expression references '" + nm + "', but it is not defined. "
							+ "(identifiers in a $(...) expression are checked after all tables"
							+ " are registered and before any driver build.)");
					}
				}
				rArr.push_back(std::move(ref));
			} else {
				std::string refName = value.substr(1);
				if (registry_->contains(refName)) {
					MetaRef ref;
					ref.col = colName;
					ref.var = refName;
					/* N-5：build 前锁定目标（seed 只在此处发生——validate() 于
					 * active() 内、全部变量注册后、任何 build/会话覆盖前调用），
					 * collect/destroy 期不再按名重查，会话别名覆盖不改变引用绑定 */
					ref.target = registry_->find(refName);
					rArr.push_back(std::move(ref));
				} else {
					throw IllegalArgumentException("'" + value + "' (column '" + colName
						+ "') in table '" + matrix_.getName() + "' row " + std::to_string(row_)
						+ ": references hardware variable '" + refName + "', but it is not defined. ('$' may forward-reference a variable declared later in the script, but the name must exist; checked after all tables are registered and before any driver build.)");
				}
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
 * 用线程局部深度计数兜底：正常引用链深度 1，超限即判定为环并清晰报错。
 * N-15：RAII 计数收敛为 util 的 RecursionGuard。 */
namespace {
thread_local int t_refDepth = 0;
constexpr int kMaxRefDepth = 32;
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
	return collectRefs(/*forDestroy=*/false);
}

/* N-16：collect 与 collectForDestroy 的公共内核（此前是 ~60 行双胞胎，
 * N-5 锁定指针等语义改动要改两处、靠评审对齐——现在只有一个实现）。
 * forDestroy 差异点：错误不再上抛（destroy 是清理路径，逐引用尽力而为）：
 * 表达式求值失败 / read 失败 / 非对象快照 → logD 跳过该列；ERR 快照跳过。 */
Json MetaProvider::collectRefs(bool forDestroy) {
	if (forDestroy) {
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
	} else if (t_refDepth >= kMaxRefDepth) {
		/* C1 防线：$ 引用列经 read() → getMetadata_() → collect() 互递归取实时
		 * 快照，环配置（如 A.src=$B 且 B.src=$A，或自引用）会无限递归直到栈
		 * 溢出——深度计数兜底，超限判环并清晰报错。（destroy 期不递归进入
		 * 本函数，被引用方的 collect 自带同一防线。） */
		throw IllegalArgumentException("circular '$' reference detected: reference chain exceeds "
			+ std::to_string(kMaxRefDepth) + " levels while resolving table '" + matrix_.getName()
			+ "' row " + std::to_string(row_)
			+ ". Check that '$' reference columns do not form a cycle (a variable referencing itself, directly or indirectly).");
	}
	RecursionGuard depthGuard(t_refDepth);

	Json obj = staticPart();
	for (const MetaRef& ref : *refs_) {
		if (ref.expr) {
			/* $(...) 表达式：把求值结果格式化后写回承载 $ 的列。
			 * 表达式只引用 @Vars（无资源），destroy 期照常求值。 */
			try {
				double v = ref.expr->eval(registry_->makeVarsResolver());
				obj.put(ref.col, JsonString(formatNumber(v)));
			} catch (const std::exception& e) {
				if (!forDestroy) { throw; }
				logD([&] {
					return "destroy metadata: '" + matrix_.getName() + "' row "
						+ std::to_string(row_) + " skipped expression column '" + ref.col
						+ "' (" + e.what() + ")";
				});
			}
			continue;
		}
		const std::string& colName = ref.col;
		const std::string& name = ref.var;
		/* N-5：seed 期锁定的目标指针——collect/destroy 期都不按名重查，
		 * 会话别名覆盖不影响引用绑定（含销毁期快照归属） */
		MatrixVars_Hardware* hv = ref.target;
		if (hv == nullptr) { continue; }
		Json snap;
		try {
			snap = hv->read();
		} catch (const std::exception& e) {
			if (!forDestroy) { throw; }
			logD([&] {
				return "destroy metadata: '" + matrix_.getName() + "' row " + std::to_string(row_)
					+ " skipped '$" + name + "' (already released: " + e.what() + ")";
			});
			continue;
		}
		if (snap.isStr() && dynamic_cast<MtVars_Vars*>(hv) != nullptr) {
			/* @Vars 数值常量：把值写回承载 $ 的列（如 Servo 的 max 列写 $smax） */
			obj.put(colName, snap);
			continue;
		}
		if (!snap.isObj()) {
			if (!forDestroy) {
				throw IllegalArgumentException("'$" + name
					+ "': referenced variable returned a non-object (scalar) snapshot; "
					+ "the '$' reference column '" + colName
					+ "' merges field snapshots (v0.., status, count). Only @Vars entries"
					+ " carry a numeric value that can fill a column; Alias/scalar variables are"
					+ " only supported via 'controller' columns.");
			}
			logD([&] {
				return "destroy metadata: '" + matrix_.getName() + "' row " + std::to_string(row_)
					+ " skipped '$" + name + "' (non-object snapshot)";
			});
			continue;
		}
		if (forDestroy) {
			auto rs = snap.get("result");
			if (rs && rs->isStr() && rs->asString().getValue() == "ERR") {
				logD([&] {
					return "destroy metadata: '" + matrix_.getName() + "' row " + std::to_string(row_)
						+ " skipped '$" + name + "' (reference reported ERR)";
				});
				continue;
			}
		}
		jsonMergeInto(obj, snap.asObject());
	}
	return obj;
}

Json MetaProvider::collectForDestroy() {
	return collectRefs(/*forDestroy=*/true);
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
			 * （对照 robots/test/insert_typo.sql 的负值契约；N-10：查找
			 * 收敛到 mt::findByName 单点） */
			mt::MatrixObject* root = mt::findByName(out, eve.insert.targetTabName);
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

/* ==================== HwFactory：矩阵表 → 硬件变量 ==================== */

/* ---- 安全添加硬件变量（不可覆盖：insert_or_assign 语义会造成隐式改名）---- */
void HwFactory::addHardwareVar(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
		const std::string& source) {
	registry_.add(name, std::move(hw), source);
}

/* ---- 注册标量文本矩阵（@Alias / @Vars）：启动期(session=nullptr)与动作脚本期(session)两路 ----
 * 两者共用一条注册路径：结构期重名即抛；动作脚本内重名报错、@Alias 同名覆盖结构定义，
 * @Vars 则拒绝覆盖（见 session 分支内的防线注释）。 */
void HwFactory::registerScalarMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session,
		ScalarKind kind) {
	const char* label = (kind == ScalarKind::Alias) ? "Alias" : "Vars";
	/* 重复报错沿用「Duplicate alias name」既有措辞（有负值回归守门），Vars 用自身名 */
	const char* dupLabel = (kind == ScalarKind::Alias) ? "alias" : "Vars";
	std::int64_t idxName = matrix.indexOfCol(0, "name");
	if (idxName < 0) {
		throw IllegalArgumentException(std::string(label) + " matrix '" + matrix.getName()
			+ "' has no 'name' column.");
	}
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		std::unique_ptr<MatrixVars_Hardware> var;
		if (kind == ScalarKind::Alias) {
			var = MtVars_Alias::Factory(matrix, i);
		} else {
			/* Vars 表达式惰性求值：标识符经注册表解析（环由 Vars 深度计数兜底；
			 * registry_ 的生命周期长于 factory_，解析器捕获其地址安全） */
			var = MtVars_Vars::Factory(matrix, i, registry_.makeVarsResolver());
		}
		if (session) {
			if (session->count(name)) {
				throw IllegalArgumentException("Duplicate " + std::string(dupLabel) + " name '"
					+ name + "' within the action script");
			}
			session->insert(name);
			if (kind == ScalarKind::Vars && registry_.contains(name)) {
				/* 审计 H-1/P0-2 防线：@Vars 的数值会经 $/$(...) 元数据引用列进入
				 * 硬件参数（max/min/fd/channel…），动作脚本同名覆盖等价于远程改写
				 * 结构定义（--tcp 模式下即安全边界失守，PoC 实证 note 50→4995）。
				 * @Alias 覆盖仅做键名替换、替换后仍过 rejectReservedKeys 元数据
				 * 防线，保留覆盖语义；@Vars 一律拒绝覆盖，参数化请使用新名字
				 * （引用结构 Vars 读取不受影响）。 */
				throw IllegalArgumentException("action script @Vars must not override the "
					"structure-defined variable '" + name + "' (use a fresh name)");
			}
			if (registry_.contains(name)) {
				std::cerr << label << " '" << name
					<< "' defined in action script overrides the structure definition" << std::endl;
			}
			registry_.override(name, std::move(var));	/* 会话期覆盖式注册（结构定义被动作脚本别名覆盖） */
		} else {
			addHardwareVar(name, std::move(var), std::string(" (in @") + label + " matrix)");
		}
	}
}

/* ---- 全部硬变量创建：声明序，Alias/Lua/Daemon 分派 ---- */
void HwFactory::createHardVars(const std::vector<mt::MatrixObject>& matrices) {
	for (const auto& matrix : matrices) {
		std::string mtName = matrix.getName();
		if (mtName == "Alias") { registerAliasMatrix(matrix, nullptr); }
		else if (mtName == "Vars") { registerVarsMatrix(matrix, nullptr); }
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
	std::string modulePath = modulePaths();	/* N-19：模块根目录单点 */
	std::string scriptPath = firstExistingModule(modulePath, mtName + ".lua");
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		precheckDuplicate(name, mtName);
		std::string ctrlName;
		if (idxCtrl >= 0) {
			std::string rawCtrl = matrix.inherited(i, idxCtrl);
			if (rawCtrl != "-") {
				if (registry_.find(rawCtrl) == nullptr) {
					throw IllegalArgumentException("Hardware variable '" + name + "' declares controller '"
						+ rawCtrl + "' in table '" + mtName
						+ "', but it is not defined yet. The controller must be defined before the matrix that references it.");
				}
				/* N-6：登记 controller 引用，会话期覆盖将拒绝触碰该名字 */
				registry_.addControllerRef(rawCtrl);
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

/* ---- @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error）---- */
void ActionRunner::runAction(std::vector<mt::MatrixObject>& actionMatrices) {
	const mt::MatrixObject* actionDefsPtr = mt::findByName(actionMatrices, "Action");
	if (actionDefsPtr == nullptr) {
		throw IllegalArgumentException("No @Action matrix found in the script. Please define @Action in your .mt file.");
	}
	const mt::MatrixObject& actionDefs = *actionDefsPtr;

	/* 列头只解析一次（C-M3）：原实现在 order/相位检测/每行每格三处
	 * 重复做 <device>.<param> 切分（切分规则单点在 mt::splitActionColumn） */
	std::vector<std::pair<std::string, std::string>> colHeads;
	colHeads.reserve(static_cast<std::size_t>(actionDefs.cols()));
	std::vector<std::string> order;
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
		colHeads.push_back(mt::splitActionColumn(actionDefs(0, j)));
		if (std::find(order.begin(), order.end(), colHeads.back().first) == order.end()) {
			order.push_back(colHeads.back().first);
		}
	}

	/* 相位设备：该设备的参数列里存在真实数值行（典型来自 mtmerge 相位归并，
	 * 前导行是无可继承的 "-"、后续行有数值）。只有这类设备做未起拍跳过；
	 * 整列 "-" 的无参设备（旧式写法）保持旧行为——靠 0.0 照常 play，
	 * 否则依赖 play 触发读数/副作用的所有 E2E（守护快照等）会整体失效。 */
	std::set<std::string> phaseDevices;
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
		const std::string& name_dev = colHeads[static_cast<std::size_t>(j)].first;
		if (phaseDevices.count(name_dev) > 0) { continue; }
		for (std::int64_t r = 1; r < static_cast<std::int64_t>(actionDefs.rows()); ++r) {
			const std::string& v = actionDefs(r, j);
			if (v != "-" && !v.empty()) { phaseDevices.insert(name_dev); break; }
		}
	}

	std::vector<ColExprCache> exprCache(static_cast<std::size_t>(actionDefs.cols()));
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(actionDefs.rows()); ++i) {
		/* D4：SIGINT/SIGTERM 后停止派发新动作帧，让 deactivate 清理链正常执行 */
		if (stopRequested()) {
			std::cerr << "Stop requested, halting action dispatch (row " << i << ")" << std::endl;
			break;
		}
		Json set = JsonObject();
		for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
			parseActionCell(set, actionDefs, i, j, phaseDevices,
				colHeads[static_cast<std::size_t>(j)], exprCache[static_cast<std::size_t>(j)]);
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
				/* 该设备本行所有参数列都未起拍（无可继承的 "-"，典型来自 mtmerge
				 * 相位前导）→ 不派发。旧行为会以 0.0 误发一次指令，把「还没轮到」
				 * 伪装成「指令为 0」；耗尽侧的行尾 "-" 会继承前值，不会走到这里。 */
				continue;
			}
			const Json& params = *paramsOpt;
			Json res = hv->play(params);
			MatrixVars_Hardware::ensureOk(res, "play " + name_dev + " (row " + std::to_string(i) + ")");
		}
	}
}

/* ---- @Action 单元格解析（别名解析 + 数值类型解析 + 分组）---- */
void ActionRunner::parseActionCell(Json& set, const mt::MatrixObject& actionDefs,
		std::int64_t i, std::int64_t j, const std::set<std::string>& phaseDevices,
		const std::pair<std::string, std::string>& colHead, ColExprCache& exprCache) {
	const std::string& name_dev = colHead.first;
	const std::string& name_param_orig = colHead.second;
	std::string name_param = name_param_orig;

	/* 未起拍：本格 "-" 且无前值可继承（首数据行），且该设备属于相位设备
	 * （后续行有真实数值，典型来自 mtmerge 相位前导）。不向该设备投递该参数
	 * ——若整行都未起拍，runAction 会整体跳过它，而不是以 0.0 误发一次指令。
	 * 整列 "-" 的无参设备不属于 phaseDevices，保持旧语义照常 play。 */
	if (phaseDevices.count(name_dev) > 0 && actionDefs.inherited(i, j) == "-") { return; }

	/* 单元格：$(...) 表达式按求值结果；否则走既有数值/继承解析。
	 * inherited 已解析 "-" 继承（上一行是表达式时按上一行表达式再算一次）。 */
	double val_fp64;
	std::string raw = actionDefs.inherited(i, j);
	if (auto body = exprBody(raw)) {
		/* C-M3：AST 按列缓存——"-" 继承链上同一表达式文本原每行重解析 */
		if (exprCache.body != *body) {
			exprCache.body = *body;
			exprCache.ast = Expression::parse(*body);
		}
		val_fp64 = exprCache.ast.eval(registry_.makeVarsResolver());
	} else {
		val_fp64 = MatrixVars_Hardware::parseMtDouble(actionDefs, i, j);
	}

	Json ret = JsonObject();
	MatrixVars_Hardware* hit = registry_.find(name_param_orig);
	if (hit != nullptr && dynamic_cast<MtVars_Text*>(hit)) {
		/* @Alias / @Vars：参数位名字替换（值即目标参数名） */
		ret = hit->read();
		name_param = ret.asString().getValue();
	}

	/* C-M3：getPtr 就地取子对象更新字段，消除逐拍 get 拷出 + put 写回的
	 * 两次深拷贝（原 asObject() 引用临时副本会悬垂，只能整份拷贝写回） */
	Json* set_params = set.getPtr(name_dev);
	if (set_params == nullptr) {
		set.put(name_dev, JsonObject());
		set_params = set.getPtr(name_dev);
	}
	set_params->put(name_param, JsonFloat(val_fp64));
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
	mt::readMatrix(input, actionMatrices);

	std::set<std::string> session;
	for (auto& matrix : actionMatrices) {
		std::string mtName = trim(matrix.getName());
		if (mtName == "Alias") { factory_.registerAliasMatrix(matrix, &session); }
		else if (mtName == "Vars") { factory_.registerVarsMatrix(matrix, &session); }
	}
	runner_.runAction(actionMatrices);
}

} // namespace bot
