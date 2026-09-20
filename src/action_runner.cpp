/* action_runner.cpp —— @Action 逐行重放（拆自 robot.cpp 阶段 R-1）
 * ActionPlan：列头解析 + 撞设备查重 + 相位设备判定的纯计算产物；
 * runAction：行循环派发（stop 检查 + 单元格解析 + 按 order play）。 */

#include "robot.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
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

/* ---- @Action 逐行重放（play 直通事件发生器；ERR 即 halt-on-error）---- */
void ActionRunner::runAction(std::vector<mt::MatrixObject>& actionMatrices) {
	const mt::MatrixObject* actionDefsPtr = mt::findByName(actionMatrices, "Action");
	if (actionDefsPtr == nullptr) {
		throw IllegalArgumentException("No @Action matrix found in the script. Please define @Action in your .mt file.");
	}
	const mt::MatrixObject& actionDefs = *actionDefsPtr;

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

} // namespace bot
