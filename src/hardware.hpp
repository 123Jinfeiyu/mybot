#pragma once
/* hardware.hpp —— 硬件变量基类（移植 bot4cj/src/HV/MatrixVars.cj)
 * 通过 Json 这种通用数据结构为多种硬件提供同一接口（play/build/destroy/read）。
 * MtVars_Alias：给某个参数取更简单别名的硬件变量（返回 JsonString）。
 */

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "json.hpp"
#include "matrix.hpp"
#include "util.hpp"

namespace bot {

class MatrixVars_Hardware {
public:
	using PlayFn = std::function<Json(const Json&)>;
	using NoArg = std::function<void()>;

	MatrixVars_Hardware(PlayFn things, std::optional<NoArg> rise = std::nullopt,
			std::optional<NoArg> free = std::nullopt)
		: things_(std::move(things)), rise_(std::move(rise)), free_(std::move(free)) {}

	virtual ~MatrixVars_Hardware() = default;

	/* 事件发生器：运行硬件或传感器，返回 Json 结果 */
	Json play(const Json& input) { return things_(input); }

	/* 无副作用读（P4/M1）：缺省实现 = play({})（实时读，向后兼容） */
	virtual Json read() { return play(JsonObject()); }

	/* 伪初始化：获取资源 */
	void build() { if (rise_) { (*rise_)(); } }

	/* 伪析构：释放资源 */
	void destroy() { if (free_) { (*free_)(); } }

	/* 获得项目值；占位符 "-" 且非首行时继承上一行（直查前向填充缓存，不递归） */
	static std::string getMt(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col) {
		return obj.inherited(row, col);
	}

	/* 解析项目值（占位符语义 + 类型解析）。本工程只用 Float64 */
	static double parseMtDouble(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col);

	/* 硬件调用结果契约检查（halt-on-error）：result=="ERR"（trim 后）即抛 */
	static Json ensureOk(const Json& res, const std::string& ctx = "");

protected:
	PlayFn things_;
	std::optional<NoArg> rise_;
	std::optional<NoArg> free_;
};

/* JsonObject 浅合并：把 src 的所有字段写入 dst（同名覆盖） */
void jsonMergeInto(Json& dst, const Json& src);

/* ---- 别名硬件变量 ---- */
class MtVars_Alias : public MatrixVars_Hardware {
	explicit MtVars_Alias(PlayFn things) : MatrixVars_Hardware(std::move(things)) {}

public:
	static std::unique_ptr<MtVars_Alias> Factory(const mt::MatrixObject& defs, std::int64_t i);
};

} // namespace bot