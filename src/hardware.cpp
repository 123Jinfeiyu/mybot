#include "hardware.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace bot {

double MatrixVars_Hardware::parseMtDouble(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col) {
	/* 继承结果直查前向填充缓存。旧的逐格递归既是 O(n²)（每行每格重走整条链），
	 * 递归深度又等于连续 "-" 的行数：Release 仅靠 GCC 尾调用优化才没崩，
	 * Debug/ASAN 构建同输入实测 stack-overflow。 */
	const std::string& raw = obj.inherited(row, col);
	if (raw == "-") { return 0.0; }	/* 首数据行即 "-"，无可继承 */

	char* end = nullptr;
	const char* start = raw.c_str();
	double v = std::strtod(start, &end);
	if (end == start || *end != '\0') {
		throw std::invalid_argument("cannot parse matrix cell '" + raw + "' as Float64");
	}
	/* L2：strtod 接受 nan/inf 字面量，会以非有限值流入驱动（舵机角度 NaN
	 * 比较恒假，脉冲宽度计算不可预测）——解析层显式拒绝。 */
	if (!std::isfinite(v)) {
		throw std::invalid_argument("matrix cell '" + raw + "' is not a finite Float64");
	}
	return v;
}

Json MatrixVars_Hardware::ensureOk(const Json& res, const std::string& ctx) {
	if (!res.isObj()) {
		/* N3 防线：结果不是表就没有 result 键可读，旧实现原样放行等同「成功」。
		 * 典型两种成因：驱动少写 return（call_lua 已就形态报 ERR），
		 * 以及把 @Alias 名当 @Action 列前缀用——alias 的 play 返回标量字符串。 */
		static const char* const kKindName[] =
			{"null", "bool", "int", "float", "string", "array", "object"};
		throw std::runtime_error("Hardware call (" + ctx + ") returned a non-table result ("
			+ kKindName[static_cast<int>(res.type)] + "). Every driver build/call/destroy must"
			+ " return a table with result=\"OK\" or result=\"ERR\". If the name in the context"
			+ " above is an @Alias or @Vars entry, note that they belong in the parameter part"
			+ " of an @Action column (dev.alias), not the device part.");
	}
	const Json& obj = res;
	/* L1：空表拦截——驱动 call 返回 {} 说明「什么都没干」，旧实现见无 result 键
	 * 即判成功（call_lua 的 N3 只拦得住「漏写 return」）。注意不能一刀切要求
	 * result 键必在：Relay/Servo 的 call 刻意返回无 result 的数据表（controller
	 * 转发语义），daemonPlay 输出 status/count 也无 result 键——两者均为非空表。 */
	if (obj.objSize() == 0) {
		throw std::runtime_error("Hardware call (" + ctx + ") returned an empty table ({}),"
			" which reads as success although the driver produced nothing. Every driver"
			" build/call/destroy must return a table with result=\"OK\" or result=\"ERR\""
			" (data fields may accompany).");
	}
	auto rs = obj.get("result");
	if (rs) {
		if (!rs->isStr()) {
			throw std::runtime_error("Hardware call (" + ctx + ") returned a non-string"
				" 'result' field; the driver contract requires result=\"OK\" or result=\"ERR\".");
		}
		std::string v = trim(rs->getValue());
		if (v == "ERR") {
			std::string msg = "Hardware call failed (" + ctx + ")";
			auto m = obj.get("message");
			if (m && m->isStr()) { msg += ": " + m->getValue(); }
			throw std::runtime_error(msg);
		}
		if (v != "OK") {
			throw std::runtime_error("Hardware call (" + ctx + ") returned result=\"" + v
				+ "\"; the driver contract only allows result=\"OK\" or result=\"ERR\".");
		}
	}
	return res;
}

void jsonMergeInto(Json& dst, const Json& src) {
	const Json& s = src.asObject();
	for (const auto& kv : s.items()) {
		dst.put(kv.first, kv.second);
	}
}

/* ---- 别名硬件变量 ---- */
std::unique_ptr<MtVars_Alias> MtVars_Alias::Factory(const mt::MatrixObject& defs, std::int64_t i) {
	std::int64_t idxName = defs.indexOfCol(0, "name");
	std::int64_t idxAlias = defs.indexOfCol(0, "alias");
	if (idxName < 0) {
		throw IllegalArgumentException("Alias matrix '" + defs.getName() + "' has no 'name' column.");
	}
	if (idxAlias < 0) {
		throw IllegalArgumentException("Alias matrix '" + defs.getName() + "' has no 'alias' column.");
	}

	std::string name = defs(i, idxName);
	std::string alias = MatrixVars_Hardware::getMt(defs, i, idxAlias);

	std::cerr << "Create an Alias vars: " << name << ", which means " << alias << std::endl;

	return std::unique_ptr<MtVars_Alias>(new MtVars_Alias(std::move(alias)));
}

/* ---- 命名数值变量 @Vars ---- */
std::unique_ptr<MtVars_Vars> MtVars_Vars::Factory(const mt::MatrixObject& defs, std::int64_t i) {
	std::int64_t idxName = defs.indexOfCol(0, "name");
	std::int64_t idxValue = defs.indexOfCol(0, "value");
	if (idxName < 0) {
		throw IllegalArgumentException("Vars matrix '" + defs.getName() + "' has no 'name' column.");
	}
	if (idxValue < 0) {
		throw IllegalArgumentException("Vars matrix '" + defs.getName() + "' has no 'value' column.");
	}

	std::string name = defs(i, idxName);
	std::string value = MatrixVars_Hardware::getMt(defs, i, idxValue);

	/* @Vars 仅支持 Number：值必须是数值字面量（整数/小数/带符号/科学计数法）。
	 * 仍按文本驻留（不转换），与结构脚本 Number 列同一判定。 */
	if (!isNumericLiteral(value)) {
		throw IllegalArgumentException("Vars '" + name + "' in matrix '" + defs.getName()
			+ "': value '" + value + "' is not a valid Number (only Number is supported).");
	}

	std::cerr << "Create a Vars: " << name << " = " << value << std::endl;

	return std::unique_ptr<MtVars_Vars>(new MtVars_Vars(std::move(value)));
}

} // namespace bot