/* robot.cpp —— RobotStruct 编排门面（重构 2026-09-20：按类拆分 TU）
 * 原单文件五类（HwRegistry/MetaProvider/ScriptLoader+HwFactory/ActionRunner/
 * RobotStruct）已拆分：hw_registry.cpp / meta_provider.cpp / hw_factory.cpp /
 * action_runner.cpp；本 TU 只剩装配流程 + 生命周期编排。 */

#include "robot.hpp"

/* 2.2：robot.hpp 不再传导 Lua 头——本 TU 编排守护生命周期，显式引入 */
#include "daemonvar.hpp"
#include "mtinclude.hpp"

#include <iostream>
#include <utility>

namespace bot {

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
	/* @Include 展开（无 @Include 时逐字不动）：先于 @Alias/@Vars 注册与
	 * @Action 派发——被并入的标量行与动作列与本体的走完全相同的后续
	 * 路径，展开只发生在矩阵层。归并内核与离线合成器 mtmerge 共用
	 * （mtmerge_core），两条入口的冲突裁决不会漂移。 */
	mtinclude::expandIncludes(actionMatrices, actionIncludeBaseDir_);

	std::set<std::string> session;
	for (auto& matrix : actionMatrices) {
		std::string mtName = trim(matrix.getName());
		if (mtName == "Alias") { factory_.registerAliasMatrix(matrix, &session); }
		else if (mtName == "Vars") { factory_.registerVarsMatrix(matrix, &session); }
	}
	runner_.runAction(actionMatrices);
}

/* ---- agent 模式：变量存在性判定（active() 之前可判定）---- */
bool RobotStruct::declaresVariable(const std::string& name) const {
	for (const auto& matrix : matrices_) {
		std::int64_t idx = matrix.indexOfCol(0, "name");
		if (idx < 0) { continue; }
		for (std::int64_t i = 1; i < static_cast<std::int64_t>(matrix.rows()); ++i) {
			if (trim(matrix(i, idx)) == name) { return true; }
		}
	}
	return false;
}

/* ---- agent 模式：注入 Lua 驱动变量（LLM / 摄像头共用）---- */
void RobotStruct::injectLuaDriver(const std::string& driverTable, const std::string& varName) {
	/* 只接受安全标识符：这两个名字会被拼进合成 SQL，且 driverTable 还会被
	 * HwFactory 当成 modules/<表名>.lua 去解析——放行引号/分号即等于让
	 * 命令行参数改写结构脚本。判据在 util::isSafeIdent 单点（与 agentmode
	 * 的 CLI 预检同一防线）。 */
	if (!isSafeIdent(driverTable) || !isSafeIdent(varName)) {
		throw IllegalArgumentException("驱动表名与变量名必须是安全标识符（字母/数字/下划线）: '"
			+ driverTable + "' / '" + varName + "'");
	}

	std::string sql = "CREATE TABLE " + driverTable + " (name String);\n"
		+ "INSERT INTO " + driverTable + " (name) VALUES ('" + varName + "');\n";
	MemoryReader reader(std::move(sql));
	std::vector<mt::MatrixObject> extra;
	ScriptLoader::readSQL(reader, extra);
	for (auto& m : extra) { matrices_.push_back(std::move(m)); }
}

} // namespace bot
