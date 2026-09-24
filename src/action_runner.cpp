/* action_runner.cpp —— @Action 逐行重放（拆自 robot.cpp 阶段 R-1）
 * ActionPlan：列头解析 + 撞设备查重 + 相位设备判定的纯计算产物；
 * runAction：行循环派发（stop 检查 + 单元格解析 + 按 order play）。 */

#include "robot.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace bot {

/* ---- 设备列解析单点（契约见 robot.hpp 的声明处）---- */
std::string ActionRunner::resolveActionDevice(const HwLookup& registry, const std::string& dev) {
	/* $(...) 设备位：求值产物作为设备名候选，走与裸名相同的解析。产物本身
	 * 不再递归求值——别名值即使形如 "$(…)" 也只按文本查找（下方 read() 出的
	 * target 直接 find，不过 exprBody），自引用环无从建立。 */
	std::string name = dev;
	if (auto body = exprBody(dev)) {
		ExprValue v = Expression::parse(*body).eval(registry.makeActionExprResolver());
		name = v.rendered();
	}
	MatrixVars_Hardware* hv = registry.find(name);
	if (hv == nullptr || !hv->isScalarText()) { return name; }
	if (std::string_view(hv->kindName()) == "vars") {
		throw IllegalArgumentException("@Vars '" + name
			+ "' is a numeric constant and cannot name a device in an @Action"
			" column; use an @Alias or the device name itself.");
	}
	const std::string target = hv->read().asString().getValue();
	MatrixVars_Hardware* resolved = registry.find(target);
	if (resolved == nullptr) {
		throw IllegalArgumentException("@Action device column '" + dev
			+ "' resolves via @Alias to '" + target + "', but '" + target
			+ "' is not a defined hardware variable.");
	}
	if (resolved->isScalarText()) {
		throw IllegalArgumentException("@Action device column '" + dev
			+ "' resolves via @Alias to '" + target + "', which is itself a"
			" scalar (@Alias/@Vars); the device part must name a driver or daemon"
			" variable (one level of alias indirection is supported).");
	}
	return target;
}

/* ---- 参数位解析单点（契约见 robot.hpp 的声明处）----
 * 派发期内别名/变量不变（标量文本无副作用、$(...) 是纯求值），每列解析一次；
 * 解析产物即参数名，不再解析（参数名不是名字绑定）。 */
std::string ActionRunner::resolveActionParam(const HwLookup& registry, const std::string& param) {
	if (auto body = exprBody(param)) {
		ExprValue v = Expression::parse(*body).eval(registry.makeActionExprResolver());
		return v.rendered();
	}
	MatrixVars_Hardware* hv = registry.find(param);
	if (hv != nullptr && hv->isScalarText()) {
		return hv->read().asString().getValue();
	}
	return param;
}

/* ---- 派发计划构建：列头解析 + 撞设备查重 + 相位设备判定（纯计算）---- */
ActionPlan ActionPlan::build(const HwLookup& registry, const mt::MatrixObject& actionDefs) {
	ActionPlan plan;
	plan.colHeads.reserve(static_cast<std::size_t>(actionDefs.cols()));
	plan.devs.reserve(static_cast<std::size_t>(actionDefs.cols()));
	plan.params.reserve(static_cast<std::size_t>(actionDefs.cols()));
	for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
		plan.colHeads.push_back(mt::splitActionColumn(actionDefs(0, j)));
		plan.devs.push_back(ActionRunner::resolveActionDevice(registry, plan.colHeads.back().first));
		plan.params.push_back(ActionRunner::resolveActionParam(registry, plan.colHeads.back().second));
		if (std::find(plan.order.begin(), plan.order.end(), plan.devs.back()) == plan.order.end()) {
			plan.order.push_back(plan.devs.back());
		}
		auto cell = plan.seenCell.emplace(
			std::make_pair(plan.devs.back(), plan.params.back()),
			actionDefs(0, j));
		if (!cell.second) {
			throw IllegalArgumentException("Action column '" + actionDefs(0, j)
				+ "' targets " + plan.devs.back() + "." + plan.params.back()
				+ ", already targeted by column '" + cell.first->second
				+ "'; a device parameter may appear in only one @Action column"
				+ " (duplicates would silently overwrite each other row by row).");
		}
	}

	for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
		const std::string& name_dev = plan.devs[static_cast<std::size_t>(j)];
		if (plan.phaseDevices.count(name_dev) > 0) { continue; }
		for (std::int64_t r = 1; r < static_cast<std::int64_t>(actionDefs.rows()); ++r) {
			const std::string& v = actionDefs(r, j);
			if (v != "-" && !v.empty()) { plan.phaseDevices.insert(name_dev); break; }
		}
	}
	return plan;
}

/* ---- 单矩阵动作派发核心（@Action 与 #34 @Reflex 共用；runAction 找
 * "Action" 表、runReflex 按触发取反射定义表，派发逻辑完全同一份）---- */
void ActionRunner::dispatchMatrix(const mt::MatrixObject& actionDefs) {
	/* 列头解析 / 撞设备查重 / 相位设备判定（纯计算，单点在 ActionPlan::build） */
	ActionPlan plan = ActionPlan::build(registry_, actionDefs);

	std::vector<ColExprCache> exprCache(static_cast<std::size_t>(actionDefs.cols()));
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(actionDefs.rows()); ++i) {
		/* D4：SIGINT/SIGTERM 后停止派发新动作帧，让 deactivate 清理链正常执行 */
		if (stopRequested()) {
			std::cerr << "Stop requested, halting action dispatch (row " << i << ")" << std::endl;
			break;
		}
		Json set = JsonObject();
		for (std::int64_t j = 0; j < static_cast<std::int64_t>(actionDefs.cols()); ++j) {
			parseActionCell(set, actionDefs, i, j, plan.phaseDevices,
				plan.params[static_cast<std::size_t>(j)],
				plan.devs[static_cast<std::size_t>(j)],
				exprCache[static_cast<std::size_t>(j)]);
		}
		for (const std::string& name_dev : plan.order) {
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

/* ---- @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error）---- */
void ActionRunner::runAction(std::vector<mt::MatrixObject>& actionMatrices) {
	const mt::MatrixObject* actionDefsPtr = mt::findByName(actionMatrices, "Action");
	if (actionDefsPtr == nullptr) {
		/* #34：脚本动作源可以是 @Reflex/@Trigger（无 @Action 时反射独立成立）；
		 * 两者都没有时才维持既有报错。 */
		if (mt::findByName(actionMatrices, "Trigger") != nullptr) { return; }
		throw IllegalArgumentException("No @Action matrix found in the script. Please define @Action in your .mt file.");
	}
	dispatchMatrix(*actionDefsPtr);
}

/* ---- @Action 单元格解析（数值/文本求值 + 分组）----
 * resolvedDev / resolvedParam 是列头经 resolveActionDevice / resolveActionParam
 * 解析后的派发身份，set 的分组键与未起拍判定都用它们；colHeads 保留原文
 * 仅供诊断。参数值可为数值（JsonFloat）或文本（JsonString，$(...) 拼接
 * 产物）——文本参数与 SQL 元数据列同型，驱动 Lua 侧无感。 */
void ActionRunner::parseActionCell(Json& set, const mt::MatrixObject& actionDefs,
		std::int64_t i, std::int64_t j, const std::set<std::string>& phaseDevices,
		const std::string& resolvedParam,
		const std::string& resolvedDev, ColExprCache& exprCache) {
	const std::string& name_param = resolvedParam;

	/* 未起拍：本格 "-" 且无前值可继承（首数据行），且该设备属于相位设备
	 * （后续行有真实数值，典型来自 mtmerge 相位前导）。不向该设备投递该参数
	 * ——若整行都未起拍，runAction 会整体跳过它，而不是以 0.0 误发一次指令。
	 * 整列 "-" 的无参设备不属于 phaseDevices，保持旧语义照常 play。 */
	if (phaseDevices.count(resolvedDev) > 0 && actionDefs.inherited(i, j) == "-") { return; }

	/* 单元格：$(...) 表达式按求值结果（数值→JsonFloat、文本→JsonString，
	 * 拼接由 '+' 的类型分派产生）；否则走既有数值/继承解析。
	 * inherited 已解析 "-" 继承（上一行是表达式时按上一行表达式再算一次）。 */
	Json paramVal;
	std::string raw = actionDefs.inherited(i, j);
	if (auto body = exprBody(raw)) {
		/* C-M3：AST 按列缓存——"-" 继承链上同一表达式文本原每行重解析 */
		if (exprCache.body != *body) {
			exprCache.body = *body;
			exprCache.ast = Expression::parse(*body);
		}
		ExprValue v = exprCache.ast.eval(registry_.makeActionExprResolver());
		paramVal = v.isText() ? JsonString(v.text) : JsonFloat(v.num);
	} else {
		paramVal = JsonFloat(MatrixVars_Hardware::parseMtDouble(actionDefs, i, j));
	}

	/* C-M3：getPtr 就地取子对象更新字段，消除逐拍 get 拷出 + put 写回的
	 * 两次深拷贝（原 asObject() 引用临时副本会悬垂，只能整份拷贝写回） */
	Json* set_params = set.getPtr(resolvedDev);
	if (set_params == nullptr) {
		set.put(resolvedDev, JsonObject());
		set_params = set.getPtr(resolvedDev);
	}
	set_params->put(name_param, paramVal);
}


/* ---- #34 条件反射：@Trigger 逐行触发 @Reflex 动作序列 ----
 * 矩阵格式（贴 @Alias 惯例，名字只能吃到行首空白）：
 *   @Reflex                   @Trigger
 *   name  relay0.deg;         trigger;
 *   blink $(bright);          blink(bright=1);
 *   blink 0;                  blink(angle=90);
 * Reflex：列 0 = 反射名（name），列 1.. = 动作列（<设备>.<参数>）；
 * 同 name 多行 = 同一反射的多拍（按行序执行）。
 * Trigger：每行一个触发行 `reflexName(k=v, k=v)`（k = 临时变量名，
 * v = 实参文本；无括号 = 无参数触发）。
 * 执行：每次触发把该反射的行组装成动作矩阵（列头 = 动作列），实参以
 * MtVars_Alias 注册进会话覆盖层（子作用域）——动作行里的 $(k) 经
 * makeActionExprResolver 命中临时别名；dispatchMatrix 派发；无论成败
 * 撤销绑定（不污染后续触发）。无 @Trigger 时反射惰性（不执行不校验）。 */
void ActionRunner::runReflex(std::vector<mt::MatrixObject>& actionMatrices) {
	const mt::MatrixObject* trig = mt::findByName(actionMatrices, "Trigger");
	if (trig == nullptr) { return; }	/* 无触发：反射惰性 */

	const mt::MatrixObject* reflexDefs = mt::findByName(actionMatrices, "Reflex");
	if (reflexDefs == nullptr) {
		throw IllegalArgumentException("@Trigger 矩阵存在但没有任何 @Reflex 定义"
			"（动作序列），请先用 @Reflex 表定义再触发");
	}
	const std::int64_t idxName = reflexDefs->requireCol(0, "name", "Reflex matrix");

	/* 反射分组（保序）：同 name 行 = 多拍 */
	std::map<std::string, std::vector<std::int64_t>> groups;
	std::vector<std::string> order;
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(reflexDefs->rows()); ++i) {
		const std::string nm = bot::trim(reflexDefs->operator()(i, idxName));
		if (nm.empty()) { continue; }
		if (groups.find(nm) == groups.end()) { order.push_back(nm); }
		groups[nm].push_back(i);
	}
	if (groups.empty()) {
		throw IllegalArgumentException("@Reflex 表没有任何定义行（列 0 = 反射名）");
	}

	const std::int64_t idxTrig = trig->requireCol(0, "trigger", "Trigger matrix");
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(trig->rows()); ++i) {
		const std::string line = bot::trim(trig->inherited(i, idxTrig));
		if (line.empty()) { continue; }	/* 空行跳过（宽松） */

		/* 解析 reflexName(k=v, ...) */
		std::string name;
		std::vector<std::pair<std::string, std::string>> args;
		const std::size_t lp = line.find('(');
		if (lp == std::string::npos) {
			name = bot::trim(line);
		} else {
			const std::size_t rp = line.rfind(')');
			if (rp == std::string::npos || rp != line.size() - 1) {
				throw IllegalArgumentException("Trigger 行（row " + std::to_string(i)
					+ "）必须以闭合 ')' 结尾：'" + line + "'");
			}
			name = bot::trim(line.substr(0, lp));
			std::string rest = bot::trim(line.substr(lp + 1, rp - lp - 1));
			if (!rest.empty()) {
				std::stringstream ss(rest);
				std::string seg;
				while (std::getline(ss, seg, ',')) {
					const std::string a = bot::trim(seg);
					if (a.empty()) { continue; }
					const std::size_t eq = a.find('=');
					if (eq == std::string::npos || eq == 0 || eq + 1 >= a.size()) {
						throw IllegalArgumentException("Trigger 参数（row " + std::to_string(i)
							+ "）须为 k=v 形式：'" + a + "'");
					}
					const std::string k = bot::trim(a.substr(0, eq));
					const std::string v = bot::trim(a.substr(eq + 1));
					if (!bot::isSafeIdent(k)) {
						throw IllegalArgumentException("Trigger 参数名（row " + std::to_string(i)
							+ "）不是安全标识符：'" + k + "'");
					}
					args.emplace_back(k, v);
				}
			}
		}
		if (name.empty()) {
			throw IllegalArgumentException("Trigger 行（row " + std::to_string(i) + "）缺少反射名");
		}
		for (std::size_t a = 0; a < args.size(); ++a) {
			for (std::size_t b = a + 1; b < args.size(); ++b) {
				if (args[a].first == args[b].first) {
					throw IllegalArgumentException("Trigger（row " + std::to_string(i)
						+ "）重复绑定参数 '" + args[a].first + "'");
				}
			}
		}
		const auto git = groups.find(name);
		if (git == groups.end()) {
			std::string all;
			for (const auto& n : order) { all += (all.empty() ? "" : ", ") + n; }
			throw IllegalArgumentException("Trigger 引用了未定义的反射 '" + name
				+ "'（row " + std::to_string(i) + "），@Reflex 定义："
				+ (all.empty() ? "（无）" : all));
		}

		/* 组装该反射的动作矩阵（列 1.. 为动作列；组内行序 = 拍序） */
		mt::MatrixBuilder b;
		b.name("Action");
		const std::int64_t ncols = reflexDefs->cols();
		for (std::int64_t j = 1; j < ncols; ++j) { b.item(reflexDefs->operator()(0, j)); }
		b.line(1);
		for (const std::int64_t row : git->second) {
			for (std::int64_t j = 1; j < ncols; ++j) { b.item(reflexDefs->operator()(row, j)); }
			b.line(row);
		}
		mt::MatrixObject actionDefs = b.take();

		/* 子作用域：绑定临时别名 → 执行 → 撤销（异常路径同样撤销） */
		std::vector<std::string> bound;
		try {
			for (const auto& kv : args) {
				registry_.override(kv.first, MtVars_Alias::Make(kv.second));
				bound.push_back(kv.first);
			}
			dispatchMatrix(actionDefs);
		} catch (...) {
			for (const auto& k : bound) { registry_.removeSessionOverride(k); }
			throw;
		}
		for (const auto& k : bound) { registry_.removeSessionOverride(k); }
	}
}

} // namespace bot
