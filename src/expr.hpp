#pragma once
/* expr.hpp —— 算术表达式（用于 $(...) 引用/定义）
 *
 * 语法：+ - * / %、一元正负、括号、十进制字面量、标识符（@Vars 名）。
 * 仅算术运算，无函数调用/赋值，避免扩大攻击面。标识符求值经 resolve 回调
 * （通常解析 @Vars），实现与调用方解耦。解析在构造期完成（语法错误早报），
 * 求值惰性（可安全支持前向引用与动作脚本覆盖）。
 */

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "util.hpp"

namespace bot {

class Expression {
public:
	/* 内部 AST 实现（定义在 expr.cpp；公开仅为 pimpl 派生实现，非使用接口）。 */
	struct Impl;

	/* 解析表达式文本；语法错误抛 IllegalArgumentException（含出错位置）。 */
	static Expression parse(const std::string& text);

	/* 求值：标识符经 resolve 取数值；除零/取模为零由本函数抛错，
	 * 标识符未定义由 resolve 抛错。 */
	double eval(const std::function<double(const std::string&)>& resolve) const;

	/* 收集全部标识符（供调用方做存在性校验）。 */
	void collectVars(std::set<std::string>& out) const;

	bool empty() const { return !impl_; }

private:
	std::shared_ptr<const Impl> impl_;
};

/* 若 value 形如 $(<expr>) 则返回内部表达式文本，否则 nullopt。
 * 注意：$(...) 内不得含空格（矩阵按空白切格），SQL 结构脚本内需写成
 * 引号形式 '$(...)'（SQL 词法把 '(' ')' 当分隔符）。 */
std::optional<std::string> exprBody(const std::string& value);

} // namespace bot
