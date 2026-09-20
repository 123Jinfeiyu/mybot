/* hw_factory.cpp —— 脚本装载与硬件装配（拆自 robot.cpp 阶段 R-1）
 * ScriptLoader：SQL 事件 → MatrixObject；HwFactory：矩阵表 → 硬件变量
 * （Alias/Vars/Lua/Daemon 分派 + controller/trigger 流引用装配校验）。 */

#include "robot.hpp"

/* 装配 Daemon/Lua/标量变量，显式引入对应实现头 */
#include "daemonvar.hpp"
#include "luavar.hpp"

#include "flowcolumn.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
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
			/* C3 防线（2026-09-20 审计）：INSERT 的值必须按**列名**落位到建表
			 * 声明序，不能按 INSERT 自带的列序顺序 append。
			 * 旧实现只取 p.value 逐位 appendRow，columnName 仅参与 Number 列的
			 * 解析期校验——于是
			 *     CREATE TABLE Timer (name String, delay Number);
			 *     INSERT INTO Timer (delay, name) VALUES (50, 'timer-0');
			 * 校验看到的是「delay←50 ✓、name←'timer-0' ✓」（按 INSERT 列名配对），
			 * 存储却把 50 写进 name、'timer-0' 写进 delay：声明为 Number 的列拿到
			 * 字符串，而 number_bad.sql 那条守门承诺的「Number 列非数值必报错」
			 * 被整条绕过，失败形态是「跑得通但值不对」。
			 * 列名未知/重复/漏列一律显式报错——仓库内 138 个 .sql 的 INSERT 列序
			 * 与建表声明序逐一相同（脚本核查），故对既有脚本零行为变化。 */
			const std::int64_t ncols = static_cast<std::int64_t>(root->cols());
			for (const auto& row : eve.insert.tableRows) {
				std::vector<std::string> values(static_cast<std::size_t>(ncols));
				std::vector<char> filled(static_cast<std::size_t>(ncols), 0);
				for (const auto& p : row) {
					const std::int64_t idx = root->indexOfCol(0, p.columnName);
					if (idx < 0) {
						throw IllegalArgumentException("INSERT INTO '" + eve.insert.targetTabName
							+ "': no such column '" + p.columnName + "'");
					}
					if (filled[static_cast<std::size_t>(idx)] != 0) {
						throw IllegalArgumentException("INSERT INTO '" + eve.insert.targetTabName
							+ "': column '" + p.columnName + "' listed twice");
					}
					values[static_cast<std::size_t>(idx)] = p.value;
					filled[static_cast<std::size_t>(idx)] = 1;
				}
				for (std::int64_t j = 0; j < ncols; ++j) {
					if (filled[static_cast<std::size_t>(j)] == 0) {
						throw IllegalArgumentException("INSERT INTO '" + eve.insert.targetTabName
							+ "': missing value for column '" + (*root)(0, j)
							+ "' (every column must be given a value; use '-' to inherit)");
					}
				}
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

/* ==================== 单消费者判据（审计 1.3）====================
 * HAL 的每次 I2C 访问都是**单次 I2C_RDWR ioctl**（寄存器指针写 + 数据读合并成
 * 一个内核事务，见 HAL.lua 的 i2c_read_reg / i2c_read_multi_bytes），内核在
 * adapter 层串行化，两个 fd 也插不进对方的指针相位。所以「主线程驱动 + 守护
 * 同址采样」对绝大多数器件是安全的——robots/test/ 下 20 个夹具正是刻意这么写的
 * （.mt 里同时点名设备与守护，注释写明"给守护留采样窗口"）。
 *
 * 真正不安全的只有一类：**器件侧状态跨多个事务**的驱动。典型是握手式间接访问
 * （AS7262：poll HW_STATUS → 写 HW_WRITE=vaddr → poll → 读 HW_READ）——地址相位
 * 存在器件里，另一个消费者插进来就会读到对方的寄存器，且双方都看不出被踩。
 *
 * 判据不放在 C++ 的表里（会与驱动漂移），而是由**驱动自己声明**：模块源里出现
 * 行首 SINGLE_CONSUMER 标记即视为声明。纯文本扫描、不执行 Lua（与
 * test/gen_const_check.cmake 从 Lua 源抽常量同一手法）。
 * 冲突键 = (总线设备路径, 归一化地址)；SPI/UART 无地址概念，键退化为 (路径, "")。
 * 以 $ 引用消费别人的变量（Monitor/Ema/Kalman）没有 I2C/address 列，天然不登记。 */
namespace {

/* 行首标记扫描：允许前导空白；注释里的偶提（"-- SINGLE_CONSUMER"）不算声明 */
bool declaresSingleConsumer(const std::string& path) {
	static const std::string kMarker = "SINGLE_CONSUMER";
	std::string text;
	std::string err;
	if (!readWholeFileLimited(path, 1u << 20, text, err, "驱动")) { return false; }
	std::size_t pos = 0;
	while (pos < text.size()) {
		const std::size_t nl = text.find('\n', pos);
		const std::string line = text.substr(pos, nl == std::string::npos ? nl : nl - pos);
		const std::size_t first = line.find_first_not_of(" \t");
		if (first != std::string::npos
			&& line.compare(first, kMarker.size(), kMarker) == 0) { return true; }
		if (nl == std::string::npos) { break; }
		pos = nl + 1;
	}
	return false;
}

/* 地址归一化：'0x48' 与 '72' 必须落到同一个键。解析不出来就按原串（去空白）用，
 * 宁可多一个键（漏判一次）也不要把两个不同地址并成一个（误报）。 */
std::string normalizeAddress(const std::string& raw) {
	std::string s = raw;
	s.erase(0, s.find_first_not_of(" \t"));
	s.erase(s.find_last_not_of(" \t") + 1);
	if (s.empty() || s == "-") { return std::string(); }
	const char* begin = s.c_str();
	char* end = nullptr;
	const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
	const long long v = std::strtoll(begin, &end, base);
	if (end == begin || *end != '\0') { return s; }
	return std::to_string(static_cast<long long>(v));	/* 十进制规范形 */
}

/* 结构脚本里的总线引用带 $ 前缀（'$i2c-1'），映射键用裸名 */
std::string stripRef(const std::string& v) {
	return (!v.empty() && v[0] == '$') ? v.substr(1) : v;
}

/* 可选列取值：列不存在或值为占位符 '-' 时返回空串 */
std::string optCol(const mt::MatrixObject& m, std::int64_t row, const char* col) {
	const std::int64_t idx = m.indexOfCol(0, col);
	if (idx < 0) { return std::string(); }
	const std::string v = m.inherited(row, idx);
	return (v == "-") ? std::string() : v;
}

} // namespace

void HwFactory::checkSingleConsumer(const std::vector<mt::MatrixObject>& matrices) {
	/* ---- 1) 总线名 → 设备路径（I2C/SPI/UART 表的 name/path 列）---- */
	std::map<std::string, std::string> busPath;
	for (const auto& m : matrices) {
		const std::string t = m.getName();
		if (t != "I2C" && t != "SPI" && t != "UART") { continue; }
		const std::int64_t idxName = m.indexOfCol(0, "name");
		const std::int64_t idxPath = m.indexOfCol(0, "path");
		if (idxName < 0 || idxPath < 0) { continue; }
		for (std::int64_t i = 1; i < static_cast<std::int64_t>(m.rows()); ++i) {
			busPath[m.inherited(i, idxName)] = m.inherited(i, idxPath);
		}
	}

	/* ---- 2) 逐表登记消费者，第二个即冲突 ---- */
	std::map<std::string, std::string> owner;	/* 键 -> 首个消费者的定位串 */
	auto claim = [&](const std::string& key, const std::string& who) {
		if (key.empty()) { return; }
		const auto it = owner.find(key);
		if (it != owner.end()) {
			throw IllegalArgumentException(
				"Device '" + key + "' would have two concurrent consumers: "
				+ it->second + " and " + who + ". This driver declares SINGLE_CONSUMER"
				" (its register access spans multiple bus transactions, so a second"
				" consumer corrupts the device-side address phase). Drop one of them,"
				" or sample it from the daemon only and consume via '$' reference.");
		}
		owner.emplace(key, who);
	};

	const std::string modulePath = modulePaths();	/* N-19：模块根目录单点 */
	for (const auto& m : matrices) {
		const std::string t = m.getName();
		if (t == "Alias" || t == "Vars" || t == "Action" || m.rows() == 0) { continue; }
		const std::int64_t idxName = m.indexOfCol(0, "name");
		if (idxName < 0) { continue; }

		if (t == "Daemon") {
			/* 守护采样的是 driver 列指名的那颗器件；proto=virt 是内存仿真，
			 * 不碰真器件，不参与判据（否则无硬件夹具会全线误报）。 */
			for (std::int64_t i = 1; i < static_cast<std::int64_t>(m.rows()); ++i) {
				const std::string proto = optCol(m, i, "proto");
				if (proto.empty() || proto == "virt") { continue; }
				const std::string drv = optCol(m, i, "driver");
				if (drv.empty()) { continue; }
				if (!declaresSingleConsumer(firstExistingModule(modulePath, drv + ".lua"))) { continue; }
				const std::string path = optCol(m, i, "path");
				if (path.empty()) { continue; }
				const std::string who = "daemon '" + m.inherited(i, idxName)
					+ "' (in table '" + t + "', row " + std::to_string(i) + ")";
				claim(path + " " + normalizeAddress(optCol(m, i, "addr")), who);
			}
			continue;
		}

		if (!declaresSingleConsumer(firstExistingModule(modulePath, t + ".lua"))) { continue; }
		for (std::int64_t i = 1; i < static_cast<std::int64_t>(m.rows()); ++i) {
			/* 只登记**直接持总线句柄**的行：有 I2C/SPI/UART 引用列才算消费者。
			 * 经 controller 转发的行（如挂在 PCA9685 上的 14 路 Servo）不自己开总线，
			 * 也就不是本判据意义上的消费者。 */
			std::string path;
			for (const char* busCol : {"I2C", "SPI", "UART"}) {
				const std::string ref = stripRef(optCol(m, i, busCol));
				if (ref.empty()) { continue; }
				const auto it = busPath.find(ref);
				path = (it != busPath.end()) ? it->second : ref;
				break;
			}
			if (path.empty()) { continue; }
			const std::string who = "hardware variable '" + m.inherited(i, idxName)
				+ "' (in table '" + t + "', row " + std::to_string(i) + ")";
			claim(path + " " + normalizeAddress(optCol(m, i, "address")), who);
		}
	}
}

/* ---- 全部硬变量创建：声明序，Alias/Lua/Daemon 分派 ---- */
void HwFactory::createHardVars(const std::vector<mt::MatrixObject>& matrices) {
	/* 装配前先做单消费者判据：此时还没申请任何资源（Lua 状态/共享槽/总线句柄），
	 * 报错即干净退出，不会留下半装配的机器人。 */
	checkSingleConsumer(matrices);
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
