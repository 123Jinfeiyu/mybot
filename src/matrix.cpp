#include "matrix.hpp"

#include <charconv>

namespace mt {

/* 浮点数写入缓冲区（INSERT 语句 Float 值）：最短往返，整数型浮点补 ".0" */
void MatrixObject::submitFloat(double fp64) {
	char buf[64];
	auto res = std::to_chars(buf, buf + sizeof(buf), fp64);
	if (res.ec != std::errc()) {
		tmp_ += std::to_string(fp64);
		return;
	}
	std::string s(buf, res.ptr);
	auto dot = s.find_first_of(".eE");
	if (dot == std::string::npos) { s += ".0"; }
	tmp_ += s;
}

/* 解析器主体：一个 _next 消费一个字符，返回更新后的行号 */
std::int64_t MatrixParser::next_(char c, std::int64_t count) {
	/* ------------ COMMENTS ------------ */
	if (!cm_ && c == '#') { cm_ = true; }
	else if (cm_ && c == '\n') {
		nl_ = true;
		cm_ = false;
		count += 1;
	}
	else if (cm_ && c != '\n') { ; }
	/* ------------ new line ------------ */
	else if (nl_ && (c == ' ' || c == '\t' || c == '\r')) { ; }
	else if (nl_ && c == '\n') { count += 1; }
	else if (nl_ && c == '@' && !obj_.hasName()) {
		nl_ = false;
		nm_ = true;
	}
	else if (nl_ && c == '@' && obj_.hasName()) {
		flag_ = true;
		nl_ = false;
		nm_ = true;
	}
	else if (nl_ && c != '@') {
		nl_ = false;
		obj_.addRune(c);
	}
	/* ------------- not nl ------------- */
	else if (!nl_ && nm_ && (c == ' ' || c == '\t' || c == '\n')) {
		obj_.submitName();
		nl_ = true;
		nm_ = false;
	}
	else if (!nl_ && !nm_ && (c == ' ' || c == '\t' || c == '\n')) {
		obj_.submitItem();
		if (c == '\n') { count += 1; }
	}
	else if (!nl_ && c == ';') {
		obj_.submitItem();
		obj_.submitLine(count);
		nl_ = true;
	}
	else if (!nl_) {
		obj_.addRune(c);
	}
	return count;
}

/* 一次只解析一个对象；无新的矩阵式对象（意外终止）返回空 */
std::optional<MatrixObject> MatrixParser::parse() {
	std::int64_t count = 1;
	flag_ = false;
	obj_ = MatrixObject();

	/* 读取初始状态，循环直到 flag 或 EOF */
	for (;;) {
		int c = input_.read();
		if (c < 0) { break; }
		if (flag_) { break; }	/* 对应 Cangjie while(!flag && hasValue(so)) 的终止检查 */
		try {
			count = next_(static_cast<char>(c), count);
		} catch (const BufOverTheLine& exp) {
			throw;
		} catch (const bot::IllegalArgumentException&) {
			throw bot::IllegalArgumentException("Parse failed at line " + std::to_string(count));
		} catch (const std::exception&) {
			throw UnknowExcept(count);
		}
	}

	if (!obj_.hasName()) { return std::nullopt; }
	return obj_;
}

} // namespace mt