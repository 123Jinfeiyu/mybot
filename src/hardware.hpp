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

/* ---- 标量文本硬件变量（@Alias / @Vars 共用）----
 * play/read 返回一个 JsonString，无资源可 build/destroy：
 *   @Alias 的值是目标参数名（@Action 列 dev.<name> 的参数位二次解析成真实参数名）；
 *   @Vars  的值是命名数值（仅支持 Number；同样可在 @Action 参数位替换，
 *          也可被结构脚本用 $<name> 引用）。
 * 框架据此两者做「参数名替换」：见 robot.cpp parseActionCell 的 MtVars_Text 判定。 */
class MtVars_Text : public MatrixVars_Hardware {
protected:
	std::string text_;

	explicit MtVars_Text(std::string text) : text_(std::move(text)) {}

public:
	Json play(const Json&) override { return JsonString(text_); }
};

/* ---- 别名硬件变量 ---- */
class MtVars_Alias : public MtVars_Text {
	explicit MtVars_Alias(std::string alias) : MtVars_Text(std::move(alias)) {}

public:
	static std::unique_ptr<MtVars_Alias> Factory(const mt::MatrixObject& defs, std::int64_t i);
};

/* ---- 命名数值变量 @Vars（列 name / value）----
 * 与 @Alias 同构，但值语义是「数值常量」而非参数名：value 必须是数值字面量
 * （仅支持 Number，非数值在 Factory 处即报错），但内部仍按文本驻留、不做转换。
 * 除 @Action 参数位替换外，独特的用途是被结构脚本元数据列以 $<name> 引用。 */
class MtVars_Vars : public MtVars_Text {
	explicit MtVars_Vars(std::string value) : MtVars_Text(std::move(value)) {}

public:
	static std::unique_ptr<MtVars_Vars> Factory(const mt::MatrixObject& defs, std::int64_t i);
};

} // namespace bot