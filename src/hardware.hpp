#pragma once
/* hardware.hpp —— 硬件变量基类（移植 bot4cj/src/HV/MatrixVars.cj)
 * 通过 Json 这种通用数据结构为多种硬件提供同一接口（play/build/destroy/read）。
 * MtVars_Alias：给某个参数取更简单别名的硬件变量（返回 JsonString）。
 */

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "expr.hpp"
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

	/* 程序化调用入口（agent 模式）：语义与 play 完全相同（含 ERR 契约），
	 * 唯一差别是**不回显结果到 stdout**。Lua 驱动覆写它来抑制顶层结果打印
	 * ——模型响应绝不能混进「stdout 只输出最终回答」这条约定。 */
	virtual Json playQuiet(const Json& input) { return play(input); }

	/* 种类标识（重构计划 2.3）：语义判定走虚调用，消灭调用方各自的
	 * dynamic_cast 开关——此前 robot/agenttools/agentdriver 五处按子类
	 * 判语义，认识全部硬件子类；新增种类时漏改一处即静默误判。
	 * 叶子类返回稳定 id（"alias"/"vars"/"lua"/"daemon"），展示文案由
	 * 消费方映射（agenttools kindOf）。 */
	virtual const char* kindName() const = 0;

	/* 标量文本变量（@Alias/@Vars）：参数位名字替换语义的判据。
	 * 别名与数值变量共用 MtVars_Text 基类，一处覆盖覆盖两个种类。 */
	virtual bool isScalarText() const { return false; }

	/* 伪初始化：获取资源（缺省无操作） */
	virtual void build() {}

	/* 伪析构：释放资源（缺省无操作） */
	virtual void destroy() {}

	/* 获得项目值；占位符 "-" 且非首行时继承上一行——直接用
	 * mt::MatrixObject::inherited(row, col)（N-15：删去无谓的 getMt 包装） */

	/* 解析项目值（占位符语义 + 类型解析）。本工程只用 Float64 */
	static double parseMtDouble(const mt::MatrixObject& obj, std::int64_t row, std::int64_t col);

	/* 硬件调用结果契约检查（halt-on-error）：result=="ERR"（trim 后）即抛；
	 * 结果不是表也即抛（表外读不到 result，静默放行＝把「没动」报成成功） */
	static Json ensureOk(const Json& res, const std::string& ctx = "");
};

/* ERR 契约结果的单点组装：result="ERR" + message。与 ensureOk（消费侧）
 * 同属契约形态的单一事实源——此前 call_lua 三条错误路径与守护 readSample
 * 的放弃路径各自手写两行 put，形态靠约定同步。 */
inline Json makeErrResult(const std::string& message) {
	Json ret = JsonObject();
	ret.put("result", JsonString("ERR"));
	ret.put("message", JsonString(message));
	return ret;
}

/* ---- 注册表只读查询接口（重构计划 2.1）----
 * luavar 与 robot 装配层之间只依赖此抽象：MtVars_Lua 经它解析 flow 目标
 * （N-6 首用锁定指针，虚调用每目标至多一次）与 $(...) 表达式标识符。
 * 测试可注入假实现建 controller/trigger 夹具，不必装配完整注册表。
 * 实现方：HwRegistry（robot.hpp）。装配/派发热路径（createHardVars、
 * ActionRunner 逐行 find）仍持具体 HwRegistry&，不经此接口。 */
class HwLookup {
public:
	virtual ~HwLookup() = default;
	virtual MatrixVars_Hardware* find(const std::string& name) const = 0;
	virtual bool contains(const std::string& name) const = 0;
	/* N-15：表达式标识符求值器（只认 @Vars 数值；未定义/非 @Vars 即抛） */
	virtual std::function<double(const std::string&)> makeVarsResolver() const = 0;
};

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
	bool isScalarText() const override { return true; }
};

/* ---- 别名硬件变量 ---- */
class MtVars_Alias : public MtVars_Text {
	explicit MtVars_Alias(std::string alias) : MtVars_Text(std::move(alias)) {}

public:
	const char* kindName() const override { return "alias"; }
	static std::unique_ptr<MtVars_Alias> Factory(const mt::MatrixObject& defs, std::int64_t i);
};

/* ---- 命名数值变量 @Vars（列 name / value）----
 * 与 @Alias 同构，但值语义是「数值常量」而非参数名：value 必须是数值字面量
 * （仅支持 Number），或形如 $(<表达式>) 的算术表达式（+ - * / %、括号、
 * 标识符引用其它 @Vars）。表达式惰性求值（读取时才解析标识符），因此支持
 * 前向引用与动作脚本覆盖；结果以文本驻留（formatNumber）。
 * 除 @Action 参数位替换外，独特的用途是被结构脚本元数据列以 $<name> 引用，
 * 或元数据列/@Action 单元格直接写 $(...)。 */
class MtVars_Vars : public MtVars_Text {
	std::string literal_;				/* 字面量原文（表达式为空时有效） */
	std::optional<Expression> expr_;		/* 表达式 AST（惰性求值） */
	std::function<double(const std::string&)> resolve_;	/* 标识符 → 数值 */

	MtVars_Vars(std::string literal, std::optional<Expression> expr,
			std::function<double(const std::string&)> resolve)
		: MtVars_Text(std::string()), literal_(std::move(literal)),
		  expr_(std::move(expr)), resolve_(std::move(resolve)) {}

public:
	/* 数值求值：字面量直接解析，表达式经 resolve_ 惰性求值（含环检测）。 */
	double numericValue() const;

	const char* kindName() const override { return "vars"; }

	/* play/read 返回格式化后的数值文本（保持「值按文本驻留」的既有语义） */
	Json play(const Json&) override;

	/* resolve：表达式标识符求值回调；纯字面量 Vars 可省略，表达式 Vars 必须提供。 */
	static std::unique_ptr<MtVars_Vars> Factory(const mt::MatrixObject& defs, std::int64_t i,
			std::function<double(const std::string&)> resolve = {});
};

} // namespace bot