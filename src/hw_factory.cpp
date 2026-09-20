/* hw_factory.cpp —— 脚本装载与硬件装配（拆自 robot.cpp 阶段 R-1）
 * ScriptLoader：SQL 事件 → MatrixObject；HwFactory：矩阵表 → 硬件变量
 * （Alias/Vars/Lua/Daemon 分派 + controller/trigger 流引用装配校验）。 */

#include "robot.hpp"

/* 装配 Daemon/Lua/标量变量，显式引入对应实现头 */
#include "daemonvar.hpp"
#include "luavar.hpp"

#include "flowcolumn.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <utility>

namespace bot {

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
			if (out.empty()) { throw IllegalArgumentException("No table created yet."); }
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

/* ---- 注册标量文本矩阵（@Alias / @Vars）：启动期(session=nullptr)与动作脚本期(session)两路 ----
 * 两者共用一条注册路径：结构期重名即抛；动作脚本内重名报错、@Alias 同名覆盖结构定义，
 * @Vars 则拒绝覆盖（见 session 分支内的防线注释）。 */
void HwFactory::registerScalarMatrix(const mt::MatrixObject& matrix, std::set<std::string>* session,
		ScalarKind kind) {
	const char* label = (kind == ScalarKind::Alias) ? "Alias" : "Vars";
	/* 重复报错沿用「Duplicate alias name」既有措辞（有负值回归守门），Vars 用自身名 */
	const char* dupLabel = (kind == ScalarKind::Alias) ? "alias" : "Vars";
	std::int64_t idxName = matrix.requireCol(0, "name", std::string(label) + " matrix");
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
			registry_.add(name, std::move(var), std::string(" (in @") + label + " matrix)");
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
	std::int64_t idxName = matrix.requireCol(0, "name", "Table");
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		precheckDuplicate(name, mtName);
		registry_.add(name, MtVars_Daemon::Factory(matrix, i, daemonHolders_),
			" (in table '" + mtName + "')");
	}
}

/* ---- 流引用列解析结果（阶段 3 拆分：createLuaVars 的引用列内核）---- */
struct FlowRowRefs {
	std::vector<std::string> ctrlNames;
	std::vector<std::string> trigNames;
};

/* 解析一行内的流引用列（controller/trigger 基名或基名+数字）并完成装配期
 * 校验，命中即登记 flowRef。逐列规则：
 * - 多列 = 扇出（controller 各收同一份转发 payload，trigger 依序前置调用），
 *   不是同行接力；同列族重复指向装配期拒绝；
 * - 「先声明」约束对每个目标独立生效——两条引用边都只指向更早声明的变量
 *   ⇒ 引用图按构造是 DAG，派发期无需环检测（前向引用是 $ 数据列的专属能力）；
 * - trigger（入边）另拒 $ 引用与框架保留键（调用边不是数据线）、触发器名
 *   与本行元数据列同名冲突（触发器结果按名并入会易主该键）。 */
FlowRowRefs parseFlowRow(const mt::MatrixObject& matrix, std::int64_t i, const std::string& name,
		const std::string& mtName, HwRegistry& registry) {
	FlowRowRefs out;
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(matrix.cols()); ++j) {
		const std::string& colName = matrix(0, j);
		const bool isCtrl = flowColumnMatches(colName, "controller");
		if (!isCtrl && !flowColumnMatches(colName, "trigger")) { continue; }
		std::string raw = matrix.inherited(i, j);
		if (raw == "-") { continue; }
		std::vector<std::string>& names = isCtrl ? out.ctrlNames : out.trigNames;
		const char* kind = isCtrl ? "controller" : "trigger";
		/* 同列族重复指向：扇出语义下只会对同一目标双调用，装配期拒绝 */
		if (std::find(names.begin(), names.end(), raw) != names.end()) {
			throw IllegalArgumentException("Hardware variable '" + name + "' declares "
				+ kind + " '" + raw + "' more than once in table '" + mtName
				+ "' (a flow column target must be unique per row).");
		}
		if (!isCtrl) {
			if (raw.rfind("$", 0) == 0) {
				throw IllegalArgumentException("Hardware variable '" + name + "' declares trigger '"
					+ raw + "' in table '" + mtName + "': the trigger column takes a plain"
					+ " variable name and does not support '$' references (a trigger is an"
					+ " invocation edge, not a data wire; use a '$' metadata column for snapshots).");
			}
			if (raw == "result" || raw == "message" || raw == "mode") {
				throw IllegalArgumentException("Hardware variable '" + name + "' declares trigger '"
					+ raw + "' in table '" + mtName + "': '" + raw
					+ "' is a framework-reserved key and cannot name a trigger.");
			}
		}
		MatrixVars_Hardware* target = registry.find(raw);
		if (target == nullptr) {
			throw IllegalArgumentException("Hardware variable '" + name + "' declares " + kind + " '"
				+ raw + "' in table '" + mtName
				+ "', but it is not defined yet. The " + kind + " must be defined before the matrix that references it.");
		}
		if (target->isScalarText()) {
			throw IllegalArgumentException("Hardware variable '" + name + "' declares " + kind + " '"
				+ raw + "' in table '" + mtName + "': '@Alias'/'@Vars' scalars cannot be "
				+ kind + " targets (both are invoked via play and must return a result table).");
		}
		if (!isCtrl) {
			/* 触发器结果以触发器名字为键并入调用参数——与本行元数据列同名会让
			 * 该键易主，装配期拒绝（排除集 = name + 全部流引用列；列集合此刻已
			 * 定型）；.mt 参数键同名由 MtVars_Lua::rejectReservedKeys 在派发期
			 * 拒绝（.mt 此刻还没加载）。 */
			for (std::int64_t c = 0; c < static_cast<std::int64_t>(matrix.cols()); ++c) {
				const std::string& other = matrix(0, c);
				if (other == "name" || isFlowColumn(other)) { continue; }
				if (other == raw) {
					throw IllegalArgumentException("Hardware variable '" + name + "' declares trigger '"
						+ raw + "' in table '" + mtName + "': the trigger result is merged"
						+ " under the trigger name, which collides with metadata column '"
						+ other + "'. Rename one of them.");
				}
			}
		}
		/* N-6：登记流引用，会话期覆盖将拒绝触碰该名字 */
		registry.addFlowRef(raw);
		names.push_back(raw);
	}
	return out;
}

void HwFactory::createLuaVars(const mt::MatrixObject& matrix, const std::string& mtName) {
	std::int64_t idxName = matrix.requireCol(0, "name", "Table");
	std::string modulePath = modulePaths();	/* N-19：模块根目录单点 */
	std::string scriptPath = firstExistingModule(modulePath, mtName + ".lua");
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
		std::string name = matrix(i, idxName);
		precheckDuplicate(name, mtName);
		FlowRowRefs flow = parseFlowRow(matrix, i, name, mtName, registry_);
		auto metaProv = std::make_shared<MetaProvider>(matrix, i, &registry_);
		metaProviders_.push_back(metaProv);	/* F-05：供 active() 在 build 前统一校验 $ 引用 */
		registry_.add(name, MtVars_Lua::Factory(scriptPath, name,
			[metaProv](bool forDestroy) {
				return forDestroy ? metaProv->collectForDestroy() : metaProv->collect();
			}, flow.ctrlNames, flow.trigNames, &registry_),
			" (in table '" + mtName + "')");
	}
}

} // namespace bot
