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
	/* 硬件变量一律以 unique_ptr 独占持有，且子类持有裸资源句柄
	 * （lua_State*、共享槽）并在析构/destroy 里释放——拷贝会造成双重释放。 */
	MatrixVars_Hardware() = default;
	MatrixVars_Hardware(const MatrixVars_Hardware&) = delete;
	MatrixVars_Hardware& operator=(const MatrixVars_Hardware&) = delete;

	virtual ~MatrixVars_Hardware() = default;

	/* 事件发生器：运行硬件或传感器，返回 Json 结果 */
	virtual Json play(const Json& input) = 0;

	/* 无副作用读（P4/M1）：缺省实现 = play({})（实时读，向后兼容） */
	virtual Json read() { return play(JsonObject()); }

	/* 伪初始化：获取资源（缺省无操作） */
	virtual void build() {}

	/* 伪析构：释放资源（缺省无操作） */
	virtual void destroy() {}

	/* 获得项目值；占位符 "-" 且非首行时继承上一行（直查前向填充缓存，不递归） */
	static std::string getMt(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col) {
		return obj.inherited(row, col);
	}

	/* 解析项目值（占位符语义 + 类型解析）。本工程只用 Float64 */
	static double parseMtDouble(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col);

	/* 硬件调用结果契约检查（halt-on-error）：result=="ERR"（trim 后）即抛；
	 * 结果不是表也即抛（表外读不到 result，静默放行＝把「没动」报成成功） */
	static Json ensureOk(const Json& res, const std::string& ctx = "");
};

/* JsonObject 浅合并：把 src 的所有字段写入 dst（同名覆盖） */
void jsonMergeInto(Json& dst, const Json& src);

/* ---- 别名硬件变量 ----
 * play 返回目标列名字符串（供 @Action 列名二次解析），无资源可 build/destroy。 */
class MtVars_Alias : public MatrixVars_Hardware {
	std::string alias_;

	explicit MtVars_Alias(std::string alias) : alias_(std::move(alias)) {}

public:
	Json play(const Json&) override { return JsonString(alias_); }

	static std::unique_ptr<MtVars_Alias> Factory(const mt::MatrixObject& defs, std::int64_t i);
};

} // namespace bot