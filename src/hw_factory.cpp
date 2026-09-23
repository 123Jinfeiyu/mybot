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
void ScriptLoader::readSQL(bot::Reader& input, std::deque<mt::MatrixObject>& out,
		bool allowDelete, Mutations* mutations) {
	std::vector<sql4cj::Token> tokens = sql4cj::Lexer::parseAll(input);
	sql4cj::Parser parser(tokens);
	/* 热删除的坐标还原（见 Mutations 注释）：每表的「当前行号 → 原始行号」映射，
	 * 按容器下标对齐存放。一条 DELETE 命中后即更新本表映射，故同一表的**后续**
	 * DELETE 仍以原始坐标记录（而 matchAll 用当前坐标匹配）。初值 = 恒等映射
	 * （此刻每个当前行号都还是它自己的原始行号）。 */
	std::vector<std::vector<std::size_t>> rowOrigin;
	/* 本批次（一次 readSQL 调用）内 CREATE 出来 / DROP 掉的表名，用于整表撤下的
	 * 交互判据：DROP 本批次新建的表 = 从影子容器整表擦除（从未装配）；DROP 后
	 * 再 INSERT/DELETE/CREATE 同名表一律拒绝（避免「表壳已被撤下却又被写回」）。 */
	std::set<std::string> createdHere;
	std::set<std::string> droppedHere;
	/* 本批次被 INSERT/DELETE（改**行**）触碰过的表名。DROP（撤**表**）要求它是本批次
	 * 针对该表的**唯一**语句：与 INSERT/DELETE 同批会打乱 applySql 靠 insertedRows
	 * 划「新增行区间」的算法（被删掉的新增行不在尾部连续块里），且「加了又整表撤下」
	 * 本就无意义——拆成两批即可。CREATE 不算触碰：本批次新建后又被 DROP 有专门路径。 */
	std::set<std::string> touchedHere;

	/* 把「这些数据行」从表上擦除，并维护「当前行号 → 原始行号」映射与 Mutations
	 * 记录。DELETE 与 DROP 共用这一段——DROP 就是「对全表数据行跑同一套擦除，
	 * 再额外把空表壳交给调用方摘除」（表壳摘除在 applySql，readSQL 只管数据行）。 */
	auto removeRowsOf = [&](mt::MatrixObject* root, const std::string& target,
			const std::vector<std::size_t>& hits) {
		if (hits.empty()) { return; }
		const std::size_t ti = static_cast<std::size_t>(root - &out[0]);
		/* 恒等映射的惰性初始化：只在**本容器首次改这张表**时铺一次。
		 * 注意不能假定「INSERT 路径已经铺过」——mutations 为空时 INSERT 的
		 * 追踪整段被跳过（启动期不需要），此处必须自给自足。 */
		if (rowOrigin.size() < out.size()) { rowOrigin.resize(out.size()); }
		if (rowOrigin[ti].empty()) {
			rowOrigin[ti].reserve(root->rows());
			for (std::size_t r = 0; r < root->rows(); ++r) { rowOrigin[ti].push_back(r); }
		}
		/* 先取走原始行号与变量名（都要在擦行之前读），再擦行。
		 * name 列缺失时不猜变量名——变量名的权威来源是装配期的 name 列，
		 * 缺失就是结构本身不合法，留给 hot-SQL 路径的既有判据报错。 */
		const std::int64_t idxName = root->indexOfCol(0, "name");
		std::vector<std::size_t> origins;
		origins.reserve(hits.size());
		for (std::size_t r : hits) {
			origins.push_back(rowOrigin[ti][r]);
			if (mutations != nullptr && idxName >= 0) {
				mutations->deletedNames.push_back((*root)(r, static_cast<std::size_t>(idxName)));
			}
		}
		root->removeRows(hits);
		/* 本表映射：被删的当前行号剔除（存活行号随之前移，索引自然对上） */
		std::vector<std::size_t> keptOrigins;
		keptOrigins.reserve(rowOrigin[ti].size() - hits.size());
		std::size_t hi = 0;
		for (std::size_t r = 0; r < rowOrigin[ti].size(); ++r) {
			if (hi < hits.size() && hits[hi] == r) { ++hi; continue; }
			keptOrigins.push_back(rowOrigin[ti][r]);
		}
		rowOrigin[ti] = std::move(keptOrigins);
		if (mutations != nullptr) {
			std::vector<std::size_t>& dst = mutations->deletedRows[target];
			for (std::size_t o : origins) { dst.push_back(o); }
		}
	};

	for (;;) {
		sql4cj::Event eve = parser.next();
		if (eve.type == sql4cj::EventType::END) { break; }
		switch (eve.type) {
		case sql4cj::EventType::CREATE_TABLE: {
			/* R-4：SQL 事件里的 token 本就完整，直接灌入构建器，
			 * 不再逐字符 addRune 重放（仓颉流 API 的残留）。 */
			if (droppedHere.count(eve.create.tableName)) {
				throw IllegalArgumentException("CREATE TABLE '" + eve.create.tableName
					+ "': the table was dropped earlier in this batch; recreate it in a"
					" separate statement batch (DROP is physically irreversible)");
			}
			mt::MatrixBuilder b;
			b.name(eve.create.tableName);
			for (const auto& col : eve.create.tableColumns) { b.item(col.name); }
			b.line(0);
			out.push_back(b.take());
			createdHere.insert(eve.create.tableName);
			break;
		}
		case sql4cj::EventType::INSERT_TABLE: {
			if (out.empty()) { throw IllegalArgumentException("No table created yet."); }
			if (droppedHere.count(eve.insert.targetTabName)) {
				throw IllegalArgumentException("INSERT INTO '" + eve.insert.targetTabName
					+ "': the table was dropped earlier in this batch");
			}
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
				/* 新增行在「原始坐标」里也是新的一行：用当前行数当它的原始行号
				 * （此刻它还没被任何删除前移过）。删掉它时这条映射会被剔除，
				 * 而调用方拿到该原始行号后只在「本批次新增区间」里找它。 */
				const std::size_t oi = static_cast<std::size_t>(root - &out[0]);
				if (rowOrigin.size() < out.size()) { rowOrigin.resize(out.size()); }
				/* 惰性恒等初始化：本容器首次改动这张表时，先把**既有**行号按恒等
				 * 铺一遍，再追加新增行。不能只 push 新增行——映射会与行号脱节
				 * （既有行没有条目），后续 DELETE 按当前行号取原始行号即越界。
				 * 启动期（mutations == nullptr）不需要映射内容，但同样必须铺：
				 * 热插路径会复用同一个容器继续改。 */
				if (rowOrigin[oi].empty()) {
					rowOrigin[oi].reserve(root->rows());
					for (std::size_t r = 0; r + 1 < root->rows(); ++r) {
						rowOrigin[oi].push_back(r);	/* 既有行，不含刚追加的这行 */
					}
				}
				rowOrigin[oi].push_back(root->rows() - 1);
				if (mutations != nullptr) { ++mutations->insertedRows[eve.insert.targetTabName]; }
			}
			/* 既存表被 INSERT 碰过 → 本批次不得再对它 DROP（见 touchedHere 注释）。
			 * 本批次新建的表不算——它整表撤下走 createdHere 路径，与行计数无关。 */
			if (!createdHere.count(eve.insert.targetTabName)) {
				touchedHere.insert(eve.insert.targetTabName);
			}
			break;
		}
		case sql4cj::EventType::DELETE_TABLE: {
			/* 启动期（结构脚本）一律关闭：机器人尚未上电，没有可变的结构。 */
			if (!allowDelete) {
				throw IllegalArgumentException("DELETE FROM '" + eve.del.targetTabName
					+ "': DELETE is a live-structure statement and cannot appear in a"
					" structure script (the robot is not powered on yet). Edit the"
					" script, or run DELETE on a live robot via /sql or apply_sql.");
			}
			if (out.empty()) { throw IllegalArgumentException("No table created yet."); }
			const std::string& target = eve.del.targetTabName;
			if (droppedHere.count(target)) {
				throw IllegalArgumentException("DELETE FROM '" + target
					+ "': the table was dropped earlier in this batch");
			}
			mt::MatrixObject* root = mt::findByName(out, target);
			if (root == nullptr) {
				throw IllegalArgumentException(
					"DELETE FROM '" + target + "': table is not defined");
			}
			/* 无条件 = 全部数据行（与 SQL 语义一致：DELETE FROM t 删空该表） */
			std::vector<std::size_t> hits;
			if (eve.del.where) {
				std::vector<std::pair<std::string, std::string>> conds;
				conds.reserve(eve.del.where->conds.size());
				for (const auto& c : eve.del.where->conds) { conds.emplace_back(c.column, c.value); }
				/* 列不存在/漏写的报错沿用 requireCol 与谓词解析的既有文案 */
				hits = mt::matchAll(*root, conds);
			} else {
				for (std::size_t r = 1; r < root->rows(); ++r) { hits.push_back(r); }
			}
			/* 「一条都没删到」按错误处理，而不是成功返回一句「删除 0 行」：
			 * 本路径上的 DELETE 是不可逆的物理断电操作，调用方（人/模型）拿到
			 * 「已生效」却什么都没发生时，无从判断是条件写错了还是器件本就不在
			 * ——拼错 name 的 DELETE 若静默成功，操作者会以为器件已摘除。
			 * 附带好处：本批次里只有「没删到」的 DELETE 时，不会被下游那条
			 * 「本批次没有任何变更」判据混淆成因（但即便如此，下面的变更计数
			 * 仍以防万一保留）。 */
			if (hits.empty()) {
				throw IllegalArgumentException("DELETE FROM '" + target
					+ "': no row matches, nothing was deleted"
					+ (eve.del.where ? " (check the WHERE values)" : " (the table has no data row)"));
			}
			removeRowsOf(root, target, hits);
			if (!createdHere.count(target)) { touchedHere.insert(target); }
			break;
		}
		case sql4cj::EventType::DROP_TABLE: {
			/* 与 DELETE 同为带电变更：结构脚本里出现同样拒绝（机器人尚未上电）。 */
			if (!allowDelete) {
				throw IllegalArgumentException("DROP TABLE '" + eve.drop.targetTabName
					+ "': DROP is a live-structure statement and cannot appear in a"
					" structure script (the robot is not powered on yet). Edit the"
					" script, or run DROP on a live robot via /sql or apply_sql.");
			}
			const std::string& target = eve.drop.targetTabName;
			if (droppedHere.count(target)) {
				throw IllegalArgumentException("DROP TABLE '" + target
					+ "': the table is dropped more than once in this batch");
			}
			/* 本批次已对这张**既存**表做过 INSERT/DELETE → 拒绝 DROP：
			 * applySql 靠 insertedRows 划「新增行区间（尾部连续块）」，而行级改动
			 * 之后该区间算法不再成立（被删掉的新增行不在尾部连续块里）；且「先加行
			 * 再整表撤下」本就没有意义。拆成两批（先改行、再 DROP）即可。 */
			if (touchedHere.count(target)) {
				throw IllegalArgumentException("DROP TABLE '" + target
					+ "': the table was already modified by INSERT/DELETE in this batch;"
					" run DROP in a separate statement batch");
			}
			if (out.empty()) { throw IllegalArgumentException("No table created yet."); }

			/* Action 表是**瞬态**的（结构期由 MR 动作脚本提供、热插期执行完即丢），
			 * 其行从不装配成常驻硬件变量，故「整表撤下」在它身上没有意义
			 * （行→变量 的模型不成立）。给一句人话而不是让它落到下游的
			 * 「没有 name 列」/「注册表里不存在」这类错位报错。 */
			if (trim(target) == "Action") {
				throw IllegalArgumentException("DROP TABLE Action: the Action table is"
					" transient (its rows are replayed, never registered as hardware"
					" variables) and cannot be dropped");
			}

			/* 无条件整表撤下（无 WHERE，见 DropTableEvent 注释）。 */
			mt::MatrixObject* root = mt::findByName(out, target);
			if (root == nullptr) {
				throw IllegalArgumentException("DROP TABLE '" + target + "': table is not defined");
			}

			if (createdHere.count(target)) {
				/* 本批次 CREATE 出来的表：从未装配、从未上电，撤销范围为空。
				 * **整表擦除**影子容器里的它（连带本批次插入的行一起消失），并把表名
				 * 传给调用方记账。表壳不复存在，故无需（也不该）走 applySql 的表壳
				 * 摘除步骤。 */
				const std::size_t k = static_cast<std::size_t>(root - &out[0]);
				out.erase(out.begin() + static_cast<std::ptrdiff_t>(k));
				if (k < rowOrigin.size()) {
					rowOrigin.erase(rowOrigin.begin() + static_cast<std::ptrdiff_t>(k));
				}
				createdHere.erase(target);
				droppedHere.insert(target);
				if (mutations != nullptr) { mutations->droppedTables.insert(target); }
				break;
			}

			/* 既存表：清空全部数据行（与 DELETE 同一条擦除机器，断电/撤注册据此
			 * 走 DelPlan），表壳保留到 applySql 的表级摘除步骤——这样影子容器与
			 * 活体矩阵**逐位对齐**，applySql 的下标不变式不被破坏。
			 * 空表（只有列头）也无条件撤下：撤一张本来就没有数据行的表是合法操作。 */
			std::vector<std::size_t> all;
			all.reserve(root->rows() > 1 ? root->rows() - 1 : 0);
			for (std::size_t r = 1; r < root->rows(); ++r) { all.push_back(r); }
			removeRowsOf(root, target, all);
			droppedHere.insert(target);
			if (mutations != nullptr) { mutations->droppedTables.insert(target); }
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
		ScalarKind kind, std::int64_t fromRow) {
	const char* label = (kind == ScalarKind::Alias) ? "Alias" : "Vars";
	/* 重复报错沿用「Duplicate alias name」既有措辞（有负值回归守门），Vars 用自身名 */
	const char* dupLabel = (kind == ScalarKind::Alias) ? "alias" : "Vars";
	std::int64_t idxName = matrix.requireCol(0, "name", std::string(label) + " matrix");
	for (std::int64_t i = fromRow; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
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

/* 守护侧的缺省值以 HwFactory::kDaemonDefault* 为**单点**（声明处有完整理由：
 * 缺列不等于"不参与判据"，必须按 sensor_daemon.lua 的缺省补全；两侧漂移由
 * test/cpp/test_robot.cpp 的 daemon_defaults_match_lua_source 读 Lua 源钉住）。 */

} // namespace

void HwFactory::checkSingleConsumer(const std::deque<mt::MatrixObject>& matrices) {
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
			/* 守护采样的是 driver 列指名的那颗器件；多器件守护
			 * （chunk = lib/sensor_daemon_multi.lua）的清单在 devices 列。
			 * proto=virt 是内存仿真，不碰真器件，不参与判据（否则无硬件夹具
			 * 会全线误报）；**缺列不等于 virt**——守护侧缺省就是 i2c，
			 * 见上面 kDaemonDefaultProto 的注释。 */
			for (std::int64_t i = 1; i < static_cast<std::int64_t>(m.rows()); ++i) {
				const std::string protoRaw = optCol(m, i, "proto");
				const std::string proto = protoRaw.empty() ? kDaemonDefaultProto : protoRaw;
				if (proto == "virt") { continue; }
				const std::string pathRaw = optCol(m, i, "path");
				const std::string path = pathRaw.empty()
					? std::string(proto == "spi" ? kDaemonDefaultPathSpi : kDaemonDefaultPathI2c)
					: pathRaw;
				const std::string whoBase = "daemon '" + m.inherited(i, idxName)
					+ "' (in table '" + t + "', row " + std::to_string(i) + ")";

				const std::string drv = optCol(m, i, "driver");
				const std::string devices = optCol(m, i, "devices");
				if (!drv.empty()) {
					/* ① 单器件守护：driver + addr 列 */
					if (declaresSingleConsumer(firstExistingModule(modulePath, drv + ".lua"))) {
						claim(path + " " + normalizeAddress(optCol(m, i, "addr")), whoBase);
					}
				} else if (!devices.empty()) {
					/* ② 多器件守护：`driver@addr@fields[@k=v...]`，以 '|' 分隔。
					 * 宿主此前从不解析这一列 ⇒ 整类守护不在判据内（漏标）。
					 * spec 语法错（缺 @、字段空）由守护侧 fail(-2) 在运行期报，
					 * 这里只跳过解析不出来的片段，不重复造一套语法校验。
					 * driver 列与 devices 列互斥（多器件守护不用 driver/addr/fields
					 * 列，见 sensor_daemon_multi.lua 文件头），故用 else if —— 否则
					 * 同一行会被 claim 两次，自己和自己冲突（误报）。 */
					std::size_t pos = 0;
					std::size_t specIdx = 0;
					while (pos < devices.size()) {
						const std::size_t bar = devices.find('|', pos);
						const std::string spec = devices.substr(pos,
							bar == std::string::npos ? std::string::npos : bar - pos);
						pos = (bar == std::string::npos) ? devices.size() : bar + 1;
						++specIdx;
						const std::size_t at1 = spec.find('@');
						if (spec.empty() || at1 == std::string::npos) { continue; }
						const std::string specDrv = trim(spec.substr(0, at1));
						const std::size_t at2 = spec.find('@', at1 + 1);
						const std::string specAddr = trim(spec.substr(at1 + 1,
							(at2 == std::string::npos ? spec.size() : at2) - at1 - 1));
						if (specDrv.empty() || !declaresSingleConsumer(
								firstExistingModule(modulePath, specDrv + ".lua"))) { continue; }
						claim(path + " " + normalizeAddress(specAddr),
							whoBase + " devices[" + std::to_string(specIdx) + "]='"
								+ specDrv + "'");
					}
				}
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

/* ---- F5-4：同物理串口多独立 fd 消费者的加载期告警 ----
 * 共享形态只有一种是危险的：两个消费者**各自 open** 同一 path（UART 总线
 * 变量重复声明同一路径、或 UART 变量 + 真机守护各开一个 fd）。此时每个
 * lua_State 独立快照/恢复 termios，后关闭者以自己 open 时的配置为准
 * （modules/lib/HAL.lua:869 自述「不做跨变量引用计数」），混用波特率还会
 * 互相改线速。而多个器件行引用**同一个** UART 总线变量（'$uart-0'）是
 * 安全形态：fd 只有一个、termios 只设一次，器件行不持有句柄——不告警。
 * virt 侧（总线 proto=virt、守护 proto=virt）走内存仿真不碰真串口，跳过
 * （否则无硬件夹具全线误报）。路径按字符串精确比对：/dev/serial0 与
 * /dev/ttyS0 这类符号链接别名无法在结构期归一，宁可漏告（与
 * normalizeAddress「宁漏不误」同取向）。只告警不拒绝——同配置轮询下
 * 实际可用，引用计数/共享 fd 属破坏性决策（fix-plan F6）。 */
void HwFactory::warnSharedUartPaths(const std::deque<mt::MatrixObject>& matrices) {
	std::map<std::string, std::vector<std::string>> users;	/* path -> 消费者定位串 */
	for (const auto& m : matrices) {
		const std::string t = m.getName();
		if (t == "Alias" || t == "Vars" || t == "Action" || m.rows() == 0) { continue; }
		const std::int64_t idxName = m.indexOfCol(0, "name");
		if (idxName < 0) { continue; }
		for (std::int64_t i = 1; i < static_cast<std::int64_t>(m.rows()); ++i) {
			std::string who;
			std::string path;
			if (t == "UART") {
				/* proto=virt 的总线行是内存仿真（attach_uart），不碰真串口 */
				const std::string proto = optCol(m, i, "proto");
				if (proto == "virt") { continue; }
				const std::int64_t idxPath = m.indexOfCol(0, "path");
				if (idxPath < 0) { continue; }
				path = m.inherited(i, idxPath);
				if (path.empty() || path == "-") { continue; }
				who = "UART bus variable '" + m.inherited(i, idxName) + "'";
			} else if (t == "Daemon") {
				const std::string proto = optCol(m, i, "proto");
				if (proto.empty() || proto == "virt") { continue; }
				path = optCol(m, i, "path");
				if (path.empty()) { continue; }
				who = "daemon '" + m.inherited(i, idxName) + "'";
			} else {
				continue;	/* 器件行经 '$uart-x' 共享总线的 fd，非独立消费者 */
			}
			users[path].push_back(who + " (table '" + t + "', row " + std::to_string(i) + ")");
		}
	}
	for (const auto& [path, who] : users) {
		if (who.size() < 2) { continue; }
		std::string names;
		for (const auto& w : who) { names += (names.empty() ? "" : ", ") + w; }
		std::cerr << "Warning: UART path '" << path << "' is opened independently by "
			<< who.size() << " consumers: " << names << ". Each keeps its own termios"
			" snapshot and the last one to close wins (no cross-variable reference"
			" counting); mixing baud rates or interleaved use may corrupt the serial"
			" line config. Share one UART bus variable via '$' reference, or sample"
			" via the daemon only." << std::endl;
	}
}

/* ---- 全部硬变量创建：声明序，Alias/Lua/Daemon 分派 ---- */
void HwFactory::createHardVars(const std::deque<mt::MatrixObject>& matrices) {
	/* 装配前先做单消费者判据：此时还没申请任何资源（Lua 状态/共享槽/总线句柄），
	 * 报错即干净退出，不会留下半装配的机器人。 */
	checkSingleConsumer(matrices);
	warnSharedUartPaths(matrices);	/* F5-4：同物理串口多独立 fd 告警（不改变行为） */
	for (const auto& matrix : matrices) { createHardVarsFrom(matrix, 1); }
}

/* 全结构判据入口：与 createHardVars 内部调用的两个判据同一实现（单一事实源）。
 * 热插表事务在提交前对「提交之后的结构视图」调用它——见头文件契约。 */
void HwFactory::precheckStructure(const std::deque<mt::MatrixObject>& matrices) {
	checkSingleConsumer(matrices);
	warnSharedUartPaths(matrices);
}

/* 单矩阵增量装配：全量装配的唯一下沉点（见头文件契约）。 */
void HwFactory::createHardVarsFrom(const mt::MatrixObject& matrix, std::int64_t fromRow) {
	std::string mtName = matrix.getName();
	if (mtName == "Alias") { registerScalarMatrix(matrix, nullptr, ScalarKind::Alias, fromRow); }
	else if (mtName == "Vars") { registerScalarMatrix(matrix, nullptr, ScalarKind::Vars, fromRow); }
	else if (mtName == "Action") { /* 结构期 Action 由 MR 动作脚本提供 */ }
	else if (mtName == "Daemon") { createDaemonVars(matrix, mtName, fromRow); }
	else if (matrix.rows() > 0) { createLuaVars(matrix, mtName, fromRow); }
}

void HwFactory::createDaemonVars(const mt::MatrixObject& matrix, const std::string& mtName,
		std::int64_t fromRow) {
	std::int64_t idxName = matrix.requireCol(0, "name", "Table");
	for (std::int64_t i = fromRow; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
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

void HwFactory::createLuaVars(const mt::MatrixObject& matrix, const std::string& mtName,
		std::int64_t fromRow) {
	std::int64_t idxName = matrix.requireCol(0, "name", "Table");
	std::string modulePath = modulePaths();	/* N-19：模块根目录单点 */
	std::string scriptPath = firstExistingModule(modulePath, mtName + ".lua");
	for (std::int64_t i = fromRow; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
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
