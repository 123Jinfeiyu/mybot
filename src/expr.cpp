#include "expr.hpp"

#include <cmath>
#include <cstdlib>

namespace bot {

/* ==================== AST 节点 ==================== */

struct Expression::Impl {
	virtual ~Impl() = default;
	virtual double eval(const std::function<double(const std::string&)>& resolve) const = 0;
	virtual void collect(std::set<std::string>& out) const = 0;
};

namespace {

struct NumNode : Expression::Impl {
	double v;
	explicit NumNode(double x) : v(x) {}
	double eval(const std::function<double(const std::string&)>&) const override { return v; }
	void collect(std::set<std::string>&) const override {}
};

struct VarNode : Expression::Impl {
	std::string name;
	explicit VarNode(std::string n) : name(std::move(n)) {}
	double eval(const std::function<double(const std::string&)>& resolve) const override {
		return resolve(name);
	}
	void collect(std::set<std::string>& out) const override { out.insert(name); }
};

struct UnNode : Expression::Impl {
	bool negate;
	std::shared_ptr<const Expression::Impl> child;
	UnNode(bool n, std::shared_ptr<const Expression::Impl> c)
		: negate(n), child(std::move(c)) {}
	double eval(const std::function<double(const std::string&)>& resolve) const override {
		double v = child->eval(resolve);
		return negate ? -v : v;
	}
	void collect(std::set<std::string>& out) const override { child->collect(out); }
};

struct BinNode : Expression::Impl {
	char op;
	std::shared_ptr<const Expression::Impl> l, r;
	BinNode(char o, std::shared_ptr<const Expression::Impl> a,
			std::shared_ptr<const Expression::Impl> b)
		: op(o), l(std::move(a)), r(std::move(b)) {}
	double eval(const std::function<double(const std::string&)>& resolve) const override {
		double a = l->eval(resolve);
		double b = r->eval(resolve);
		switch (op) {
		case '+': return a + b;
		case '-': return a - b;
		case '*': return a * b;
		case '/':
			if (b == 0.0) { throw IllegalArgumentException("expression division by zero"); }
			return a / b;
		case '%':
			if (b == 0.0) { throw IllegalArgumentException("expression modulo by zero"); }
			return std::fmod(a, b);
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
		throw IllegalArgumentException("invalid expression '" + s_ + "': " + msg
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
		char* end = nullptr;
		double v = std::strtod(tok.c_str(), &end);
		if (end != tok.c_str() + tok.size() || !std::isfinite(v)) {
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

double Expression::eval(const std::function<double(const std::string&)>& resolve) const {
	if (!impl_) { throw IllegalArgumentException("evaluation of an empty expression"); }
	double v = impl_->eval(resolve);
	/* 有限值防线：字面量在 parseNumber 已拒 inf/nan，但中间结果仍可能溢出
	 * （如 1e308*1e308 → inf；inf-inf → nan）。IEEE 语义下任何非有限中间
	 * 结果必然传播到根节点，故在出口处一次检查即可封死旁路——否则 inf/nan
	 * 会经 JsonFloat/lua_pushnumber 流入驱动参数（正是 hardware.cpp 字面量
	 * 防线要拦的形态，TCP 模式下可由远程动作脚本触发）。 */
	if (!std::isfinite(v)) {
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
