#include "expr.hpp"

#include <cmath>
#include <cstdlib>

namespace bot {

/* ==================== 值语义 ==================== */

std::string ExprValue::rendered() const {
	if (isText()) { return text; }
	if (!std::isfinite(num)) {
		throw IllegalArgumentException("expression evaluates to a non-finite value"
			" (inf/nan), which is rejected");
	}
	return formatNumber(num);
}

ExprValueResolver numericResolver(std::function<double(const std::string&)> f) {
	return [f = std::move(f)](const std::string& name) {
		return ExprValue::makeNum(f(name));
	};
}

double requireNum(ExprValue v) {
	if (v.isText()) {
		throw IllegalArgumentException("expression produced a text result"
			" (an @Alias may only take part in '+' concatenation inside @Action"
			" cells); a numeric value is required here");
	}
	return v.num;
}

/* ==================== AST 节点 ==================== */

struct Expression::Impl {
	virtual ~Impl() = default;
	virtual ExprValue eval(const ExprValueResolver& resolve) const = 0;
	virtual void collect(std::set<std::string>& out) const = 0;
};

namespace {

/* 文本操作数只允许出现在 '+' 里（拼接）；其余运算符对文本无定义。
 * 文案单点：BinNode 与 UnNode 共用。 */
[[noreturn]] void throwTextOperand(char op) {
	throw IllegalArgumentException(std::string("expression operator '") + op
		+ "' cannot be applied to a text result; only '+' may combine text"
		+ " (@Alias concatenation)");
}

struct NumNode : Expression::Impl {
	double v;
	explicit NumNode(double x) : v(x) {}
	ExprValue eval(const ExprValueResolver&) const override { return ExprValue::makeNum(v); }
	void collect(std::set<std::string>&) const override {}
};

struct VarNode : Expression::Impl {
	std::string name;
	explicit VarNode(std::string n) : name(std::move(n)) {}
	ExprValue eval(const ExprValueResolver& resolve) const override { return resolve(name); }
	void collect(std::set<std::string>& out) const override { out.insert(name); }
};

struct UnNode : Expression::Impl {
	bool negate;
	std::shared_ptr<const Expression::Impl> child;
	UnNode(bool n, std::shared_ptr<const Expression::Impl> c)
		: negate(n), child(std::move(c)) {}
	ExprValue eval(const ExprValueResolver& resolve) const override {
		ExprValue v = child->eval(resolve);
		if (v.isText()) { throwTextOperand('-'); }
		return negate ? ExprValue::makeNum(-v.num) : v;
	}
	void collect(std::set<std::string>& out) const override { child->collect(out); }
};

struct BinNode : Expression::Impl {
	char op;
	std::shared_ptr<const Expression::Impl> l, r;
	BinNode(char o, std::shared_ptr<const Expression::Impl> a,
			std::shared_ptr<const Expression::Impl> b)
		: op(o), l(std::move(a)), r(std::move(b)) {}
	ExprValue eval(const ExprValueResolver& resolve) const override {
		ExprValue a = l->eval(resolve);
		ExprValue b = r->eval(resolve);
		/* '+' 类型分派：出现文本操作数即拼接（数值操作数以真值文本参与），
		 * 全数值才是算术加。「两个 @Vars 做运算、有一个 @Alias 就拼接」。 */
		if (op == '+' && (a.isText() || b.isText())) {
			return ExprValue::makeText(a.rendered() + b.rendered());
		}
		if (a.isText() || b.isText()) { throwTextOperand(op); }
		switch (op) {
		case '+': return ExprValue::makeNum(a.num + b.num);
		case '-': return ExprValue::makeNum(a.num - b.num);
		case '*': return ExprValue::makeNum(a.num * b.num);
		case '/':
			if (b.num == 0.0) { throw IllegalArgumentException("expression division by zero"); }
			return ExprValue::makeNum(a.num / b.num);
		case '%':
			if (b.num == 0.0) { throw IllegalArgumentException("expression modulo by zero"); }
			return ExprValue::makeNum(std::fmod(a.num, b.num));
		}
		throw IllegalArgumentException("expression internal error: unknown operator");
	}
	void collect(std::set<std::string>& out) const override { l->collect(out); r->collect(out); }
};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

/* 报错里的原文引用（2026-09-20 重构阶段 4.4 首落地，审计 C4 后收敛到单点）：
 * 旧实现把整条表达式原样拼进报错，而表达式长度只受动作脚本 8 MiB 上限约束
 * ——一条语法错误即可把用户可控的兆级字节灌进 stderr（实测 10k 层括号 → 20KB），
 * 且原文可携带 ANSI 控制序列直写操作者终端。
 * 现统一走 util.hpp 的 echoSource（转义不可打印字节 + 截断到 kMaxEchoChars +
 * 省略标记）：矩阵单元格报错（hardware.cpp）与表达式报错共用同一判据，
 * 不再出现「同类缺陷只修了一条路径」。 */

/* 递归下降解析器。两处深度上限：
 * ① 括号递归（depth_）：8 MiB 脚本字节上限下，数万层 '(' 会打爆宿主栈；
 * ② 运算符链（ops_）：平铺的 '+'/'*' 链不经过括号递归，由 parseExpr/parseTerm
 *    的循环就地折叠成左深树——kMaxDepth 拦不住它，而 BinNode::eval 的递归
 *    深度等于树深，无界链会在求值期 SIGSEGV（且崩溃点在硬件 build 之后，
 *    绕过 deactivate 清理链）。限制单表达式运算符总数后，树深上界为
 *    kMaxDepth + kMaxOps，求值递归安全。 */
class ExprParser {
	const std::string& s_;
	std::size_t pos_ = 0;
	int depth_ = 0;
	int ops_ = 0;
	static constexpr int kMaxDepth = 128;
	static constexpr int kMaxOps = 512;

	[[noreturn]] void fail(const std::string& msg) const {
		throw IllegalArgumentException("invalid expression '" + echoSource(s_) + "': " + msg
			+ " at position " + std::to_string(pos_));
	}
	char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
	void skipWs() {
		while (pos_ < s_.size()) {
			char c = s_[pos_];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++pos_; }
			else { break; }
		}
	}

	using Node = std::shared_ptr<const Expression::Impl>;

	Node parseExpr() {
		Node node = parseTerm();
		for (;;) {
			skipWs();
			char c = peek();
			if (c != '+' && c != '-') { break; }
			++pos_;
			if (++ops_ > kMaxOps) { fail("too many operators in one expression"); }
			node = std::make_shared<BinNode>(c, std::move(node), parseTerm());
		}
		return node;
	}

	Node parseTerm() {
		Node node = parseUnary();
		for (;;) {
			skipWs();
			char c = peek();
			if (c != '*' && c != '/' && c != '%') { break; }
			++pos_;
			if (++ops_ > kMaxOps) { fail("too many operators in one expression"); }
			node = std::make_shared<BinNode>(c, std::move(node), parseUnary());
		}
		return node;
	}

	/* 一元正负：迭代收集符号（避免长串 '-----x' 递归打爆栈） */
	Node parseUnary() {
		bool neg = false;
		for (;;) {
			skipWs();
			char c = peek();
			if (c == '+') { ++pos_; }
			else if (c == '-') { neg = !neg; ++pos_; }
			else { break; }
		}
		Node node = parsePrimary();
		if (neg) { node = std::make_shared<UnNode>(true, std::move(node)); }
		return node;
	}

	Node parsePrimary() {
		skipWs();
		char c = peek();
		if (c == '\0') { fail("unexpected end of expression"); }
		if (c == '(') {
			++pos_;
			if (++depth_ > kMaxDepth) { fail("expression nesting too deep"); }
			Node node = parseExpr();
			skipWs();
			if (peek() != ')') { fail("expected ')'"); }
			++pos_;
			--depth_;
			return node;
		}
		if (isDigit(c) || (c == '.' && isDigit(pos_ + 1 < s_.size() ? s_[pos_ + 1] : '\0'))) {
			return parseNumber();
		}
		if (isIdentStart(c)) { return parseVar(); }
		fail(std::string("unexpected character '") + c + "'");
	}

	Node parseNumber() {
		std::size_t start = pos_;
		while (isDigit(peek())) { ++pos_; }
		if (peek() == '.') {
			++pos_;
			while (isDigit(peek())) { ++pos_; }
		}
		if (peek() == 'e' || peek() == 'E') {
			std::size_t save = pos_;
			++pos_;
			if (peek() == '+' || peek() == '-') { ++pos_; }
			if (isDigit(peek())) {
				while (isDigit(peek())) { ++pos_; }
			} else {
				pos_ = save;	/* 不是指数，回退（如 "1e" 视为 1） */
			}
		}
		std::string tok = s_.substr(start, pos_ - start);
		double v = 0.0;
		if (parseFiniteDouble(tok, v) != ParseFiniteStatus::OK) {
			fail("invalid numeric literal");
		}
		return std::make_shared<NumNode>(v);
	}

	Node parseVar() {
		std::size_t start = pos_;
		++pos_;
		while (isIdentChar(peek())) { ++pos_; }
		return std::make_shared<VarNode>(s_.substr(start, pos_ - start));
	}

public:
	explicit ExprParser(const std::string& s) : s_(s) {}

	Node parse() {
		skipWs();
		if (pos_ >= s_.size()) { fail("empty expression"); }
		Node node = parseExpr();
		skipWs();
		if (pos_ != s_.size()) { fail("unexpected trailing input"); }
		return node;
	}
};

} // namespace

Expression Expression::parse(const std::string& text) {
	Expression e;
	e.impl_ = ExprParser(text).parse();
	return e;
}

ExprValue Expression::eval(const ExprValueResolver& resolve) const {
	if (!impl_) { throw IllegalArgumentException("evaluation of an empty expression"); }
	ExprValue v = impl_->eval(resolve);
	/* 有限值防线：字面量在 parseNumber 已拒 inf/nan，但中间结果仍可能溢出
	 * （如 1e308*1e308 → inf；inf-inf → nan）。IEEE 语义下任何非有限中间
	 * 结果必然传播到根节点，故在出口处一次检查即可封死旁路——否则 inf/nan
	 * 会经 JsonFloat/lua_pushnumber 流入驱动参数（正是 hardware.cpp 字面量
	 * 防线要拦的形态，TCP 模式下可由远程动作脚本触发）。文本结果中不可能
	 * 藏非有限数值：拼接经 rendered() 渲染数值真值，那里同样拒绝。 */
	if (v.isNum() && !std::isfinite(v.num)) {
		throw IllegalArgumentException("expression evaluates to a non-finite value"
			" (inf/nan), which is rejected");
	}
	return v;
}

void Expression::collectVars(std::set<std::string>& out) const {
	if (impl_) { impl_->collect(out); }
}

std::optional<std::string> exprBody(const std::string& value) {
	if (value.size() >= 3 && value[0] == '$' && value[1] == '(' && value.back() == ')') {
		return value.substr(2, value.size() - 3);
	}
	return std::nullopt;
}

} // namespace bot
