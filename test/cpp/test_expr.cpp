/* test_expr.cpp —— 算术表达式（$(...) 内部）解析与求值
 *
 * 覆盖：优先级/结合性、一元正负、括号、除零与取模为零、语法错误、
 * 标识符收集、嵌套深度上限（防深括号打爆栈）、$(...) 外壳识别。
 */

#include <functional>
#include <map>
#include <set>
#include <string>

#include "expr.hpp"
#include "harness.hpp"
#include "util.hpp"

using bot::Expression;
using bot::exprBody;

namespace {

/* 简单变量表求值器：未定义抛 bot::IllegalArgumentException（与生产 resolver 同契约） */
std::function<double(const std::string&)> varTable(const std::map<std::string, double>& vars) {
	return [vars](const std::string& name) -> double {
		auto it = vars.find(name);
		if (it == vars.end()) {
			throw bot::IllegalArgumentException("undefined: " + name);
		}
		return it->second;
	};
}

double evalWith(const std::string& text, const std::map<std::string, double>& vars) {
	return Expression::parse(text).eval(varTable(vars));
}

} // namespace

TH_TEST(expr_arithmetic_and_precedence) {
	TH_CHECK(evalWith("1+2*3", {}) == 7.0);
	TH_CHECK(evalWith("(1+2)*3", {}) == 9.0);
	TH_CHECK(evalWith("10-2-3", {}) == 5.0);		/* 左结合 */
	TH_CHECK(evalWith("100/5/2", {}) == 10.0);
	TH_CHECK(evalWith("7%4", {}) == 3.0);
	TH_CHECK(evalWith("1+2+10", {}) == 13.0);
}

TH_TEST(expr_unary_and_parentheses) {
	TH_CHECK(evalWith("-5+8", {}) == 3.0);
	TH_CHECK(evalWith("--5", {}) == 5.0);
	TH_CHECK(evalWith("-(2+3)*2", {}) == -10.0);
	TH_CHECK(evalWith("+4", {}) == 4.0);
	TH_CHECK(evalWith("((((1))))", {}) == 1.0);
	TH_CHECK(evalWith("1.5*2", {}) == 3.0);
	TH_CHECK(evalWith("2e3+1", {}) == 2001.0);
}

TH_TEST(expr_identifiers_resolve) {
	std::map<std::string, double> vars{{"v1", 10.0}, {"v2", 5.0}, {"x", 2.5}};
	TH_CHECK(evalWith("v1+v2+10", vars) == 25.0);
	TH_CHECK(evalWith("v1*v2-x", vars) == 47.5);
	TH_CHECK(evalWith("(v1-v2)/(x+0.5)", vars) == 5.0 / 3.0);
}

TH_TEST(expr_undefined_identifier_propagates) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "undefined: ghost",
		evalWith("1+ghost", {}));
}

TH_TEST(expr_division_and_modulo_by_zero) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "division by zero",
		evalWith("1/0", {}));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "modulo by zero",
		evalWith("1%0", {}));
}

TH_TEST(expr_syntax_errors_are_clear) {
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "invalid expression",
		Expression::parse("1+"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "expected ')'",
		Expression::parse("(1+2"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected trailing input",
		Expression::parse("1 2"));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "empty expression",
		Expression::parse("   "));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "unexpected character",
		Expression::parse("1+@"));
}

TH_TEST(expr_deep_nesting_is_rejected_not_crashed) {
	/* 脚本字节上限下，深括号（如数万层 '('）必须在解析期拒绝，而不是打爆宿主栈 */
	std::string deep(5000, '(');
	deep += "1";
	deep.append(5000, ')');
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "nesting too deep",
		Expression::parse(deep));
}

TH_TEST(expr_operator_chain_is_rejected_not_crashed) {
	/* 平铺运算符链不经过括号递归（kMaxDepth 拦不住），左深树会让 eval 的
	 * 递归深度等于树深——无界链在求值期 SIGSEGV（审计 C-1 实证：600 KB
	 * 的 '+1' 链稳定崩溃）。必须在解析期就拒绝。 */
	std::string chain = "1";
	for (int i = 0; i < 2000; ++i) { chain += "+1"; }	/* 2000 个运算符，超 kMaxOps=512 */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "too many operators",
		Expression::parse(chain));
	/* 上限内的合法长链（512 个）必须正常求值，确保防线没有误伤 */
	std::string ok = "0";
	for (int i = 0; i < 500; ++i) { ok += "+1"; }
	TH_CHECK(evalWith(ok, {}) == 500.0);
}

TH_TEST(expr_collect_vars) {
	std::set<std::string> names;
	Expression::parse("v1+v2*3-(-x)").collectVars(names);
	TH_CHECK(names.size() == 3);
	TH_CHECK(names.count("v1") == 1);
	TH_CHECK(names.count("v2") == 1);
	TH_CHECK(names.count("x") == 1);
}

TH_TEST(expr_body_detects_shell) {
	auto body = exprBody("$(v1+v2+10)");
	TH_CHECK(body.has_value());
	TH_CHECK(*body == "v1+v2+10");

	TH_CHECK(!exprBody("$v1").has_value());
	TH_CHECK(!exprBody("v1+v2").has_value());
	TH_CHECK(!exprBody("$(v1) ").has_value());	/* 末尾有空格不算 */
	TH_CHECK(!exprBody("$(").has_value());
}
