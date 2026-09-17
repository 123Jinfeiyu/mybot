#include "matrix.hpp"

namespace mt {

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
	else if (nl_ && c == '@' && !builder_.hasName()) {
		nl_ = false;
		nm_ = true;
	}
	else if (nl_ && c == '@' && builder_.hasName()) {
		flag_ = true;
		nl_ = false;
		nm_ = true;
	}
	else if (nl_ && c != '@') {
		nl_ = false;
		builder_.addRune(c);
	}
	/* ------------- not nl ------------- */
	else if (!nl_ && nm_ && (c == ' ' || c == '\t' || c == '\n')) {
		builder_.submitName();
		if (c == '\n') { count += 1; }	/* @Name 行末的换行同样计数，否则后续行号全部少 1 */
		nl_ = true;
		nm_ = false;
	}
	else if (!nl_ && !nm_ && (c == ' ' || c == '\t' || c == '\n')) {
		builder_.submitItem();
		if (c == '\n') { count += 1; }
	}
	else if (!nl_ && c == ';') {
		builder_.submitItem();
		builder_.submitLine(count);
		nl_ = true;
	}
	else if (!nl_) {
		builder_.addRune(c);
	}
	return count;
}

/* 一次只解析一个对象；无新的矩阵式对象（意外终止）返回空 */
std::optional<MatrixObject> MatrixParser::parse() {
	std::int64_t count = 1;
	/* 每个矩阵独立解析：全部状态复位。只复位 flag_/builder_ 会让残留的 nl_/nm_
	 * 把下一个矩阵的起始 '@' 当作数据吞进名字，矩阵名变成 "@Action" 而静默失配。 */
	flag_ = false;
	nl_ = true;
	cm_ = false;
	nm_ = false;
	builder_ = MatrixBuilder();

	/* 读取初始状态，循环直到 flag 或 EOF。flag_ 必须在 read() 之前检查：
	 * 闭合 '@' 置位 flag_ 后若再读一个字符，该字符被丢弃，紧随其后的下一个
	 * 矩阵就少了一个前导字符（单换行分隔时恰好少掉换行，触发上面的名字污染）。 */
	for (;;) {
		if (flag_) { break; }	/* 对应 Cangjie while(!flag && hasValue(so)) 的终止检查 */
		int c = input_.read();
		if (c < 0) { break; }
		try {
			count = next_(static_cast<char>(c), count);
		} catch (const MatrixParseExcept&) {
			throw;	/* DoesNotSync 等已自带行号与语义，原样上抛，不降级成无信息异常 */
		} catch (const bot::IllegalArgumentException&) {
			throw bot::IllegalArgumentException("Parse failed at line " + std::to_string(count));
		} catch (const std::exception&) {
			throw UnknowExcept(count);
		}
	}

	if (!builder_.hasName()) { return std::nullopt; }
	return builder_.take();
}

} // namespace mt