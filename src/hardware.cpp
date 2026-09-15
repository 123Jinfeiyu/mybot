#include "hardware.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace bot {

double MatrixVars_Hardware::parseMtDouble(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col) {
	const std::string& raw = obj(row, col);
	if (raw == "-" && row > 1) { return parseMtDouble(obj, row - 1, col); }
	if (raw == "-") { return 0.0; }

	char* end = nullptr;
	const char* start = raw.c_str();
	double v = std::strtod(start, &end);
	if (end == start || *end != '\0') {
		throw std::invalid_argument("cannot parse matrix cell '" + raw + "' as Float64");
	}
	return v;
}

Json MatrixVars_Hardware::ensureOk(const Json& res, const std::string& ctx) {
	if (!res.isObj()) { return res; }
	const Json& obj = res;
	auto rs = obj.get("result");
	if (rs && rs->isStr() && trim(rs->getValue()) == "ERR") {
		std::string msg = "Hardware call failed (" + ctx + ")";
		auto m = obj.get("message");
		if (m && m->isStr()) { msg += ": " + m->getValue(); }
		throw std::runtime_error(msg);
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

	std::string name = defs(i, idxName);
	std::string alias = MatrixVars_Hardware::getMt(defs, i, idxAlias);

	std::cout << "Create an Alias vars: " << name << ", which means " << alias << std::endl;

	return std::unique_ptr<MtVars_Alias>(new MtVars_Alias(
		[alias](const Json&) { return JsonString(alias); }));
}

} // namespace bot