#include "sqlparse.hpp"

#include <stdexcept>

namespace sql4cj {

/* ---- ASCII 不区分大小写比较 ---- */
static bool equalWithoutCase(const std::string& s1, const std::string& s2) {
	if (s1.size() != s2.size()) { return false; }
	for (std::size_t i = 0; i < s1.size(); ++i) {
		char c1 = s1[i], c2 = s2[i];
		if (c1 >= 'A' && c1 <= 'Z') { c1 = static_cast<char>(c1 + 32); }
		if (c2 >= 'A' && c2 <= 'Z') { c2 = static_cast<char>(c2 + 32); }
		if (c1 != c2) { return false; }
	}
	return true;
}

/* ---- 词元引号态（F-02）----
 * 首字符为引号且未闭合 → 内部字符一律按字面处理；"--" 与块注释起始符在引号内
 * 是数据而非注释。状态由 Lexer::next 以 (inQuote, quoteChar) 增量维护：旧实现
 * 每追加一个字符就重扫整个 token（O(n²)，100 KB 引号值实测 4.84 s 纯 CPU）。
 * 注意：SQL 标准的 '' 双写转义仍不实现——每个同种引号字符都翻转引号态，
 * 与旧 insideQuotes 语义逐字相同（值内含奇数引号的状态错乱属已知边界）。 ---- */

/* ---- 去掉首尾空白与引号（对应 parseName）---- */
static std::string parseName(const std::string& s) {
	std::size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
	while (b < e && (s[b] == '\'' || s[b] == '"')) ++b;
	while (e > b && (s[e-1] == '\'' || s[e-1] == '"')) --e;
	return s.substr(b, e - b);
}

Token Token::parse(const std::string& s) {
	if (equalWithoutCase(s, "select")) { return {TokenType::SELECT, s}; }
	if (equalWithoutCase(s, "from")) { return {TokenType::FROM, s}; }
	if (equalWithoutCase(s, "create")) { return {TokenType::CREATE, s}; }
	if (equalWithoutCase(s, "table")) { return {TokenType::TABLE, s}; }
	if (equalWithoutCase(s, "string")) { return {TokenType::STRING, s}; }
	if (equalWithoutCase(s, "number")) { return {TokenType::NUMBER, s}; }
	if (equalWithoutCase(s, "insert")) { return {TokenType::INSERT, s}; }
	if (equalWithoutCase(s, "into")) { return {TokenType::INTO, s}; }
	if (equalWithoutCase(s, "values")) { return {TokenType::VALUES, s}; }
	if (s == ";") { return {TokenType::END, s}; }
	if (s == ",") { return {TokenType::COMMA, s}; }
	if (s == "(") { return {TokenType::LEFT, s}; }
	if (s == ")") { return {TokenType::RGHT, s}; }
	Token t = {TokenType::NAME, s};
	t.name = parseName(s);
	return t;
}

std::string Token::serialize() const {
	switch (type) {
		case TokenType::SELECT: return "SELECT";
		case TokenType::FROM: return "FROM";
		case TokenType::CREATE: return "CREATE";
		case TokenType::TABLE: return "TABLE";
		case TokenType::STRING: return "STRING";
		case TokenType::NUMBER: return "NUMBER";
		case TokenType::INSERT: return "INSERT";
		case TokenType::INTO: return "INTO";
		case TokenType::VALUES: return "VALUES";
		case TokenType::END: return ";";
		case TokenType::COMMA: return ",";
		case TokenType::LEFT: return "(";
		case TokenType::RGHT: return ")";
		case TokenType::UNEXPECTED_END: return "";
		case TokenType::UNKNOWN: return "?";
		case TokenType::NAME: return "name'" + name + "'";
	}
	return "?";
}

/* ------------- Lexer ------------- */
Token Lexer::next(bot::Reader& sr) {
	if (flag_) { flag_ = false; return {TokenType::END, ""}; }
	if (comma_) { comma_ = false; return {TokenType::COMMA, ""}; }
	if (rght_) { rght_ = false; return {TokenType::RGHT, ""}; }

	std::string s;
	bool inQuote = false;		/* F-02：引号态由本函数增量维护（O(n)），
					 * 取代旧实现每字符重扫 token 的 O(n²) */
	char quoteChar = 0;		/* 打开引号态的字符：' 或 " */
	while (true) {
		int ci = sr.read();
		if (ci < 0) {
			if (s.empty()) { return {TokenType::UNEXPECTED_END, ""}; }
			return Token::parse(s);
		}
		char c = static_cast<char>(ci);

		if (singleLineComment_ && c != '\n') { continue; }
		else if (singleLineComment_ && c == '\n') {
			singleLineComment_ = false;
			continue;
		}

		if (multiLineComment_ && !multiLineComment2_ && c != '*') { continue; }
		else if (multiLineComment_ && !multiLineComment2_ && c == '*') {
			multiLineComment2_ = true;
			continue;
		}
		else if (multiLineComment_ && multiLineComment2_ && c != '/') {
			multiLineComment2_ = false;
			continue;
		}
		else if (multiLineComment_ && multiLineComment2_ && c == '/') {
			multiLineComment_ = false;
			multiLineComment2_ = false;
			continue;
		}

		bool blank = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0');

		/* F-02：引号态内一律按字面追加；同种引号再现即闭合。进入引号态
		 * 只可能发生在 token 首字符处（与旧 insideQuotes 的 s[0] 判据等价）。 */
		if (inQuote) {
			s += c;
			if (c == quoteChar) { inQuote = false; }
			continue;
		}
		if (s.empty() && (c == '\'' || c == '"')) {
			inQuote = true;
			quoteChar = c;
			s += c;
			continue;
		}
		if (s.empty() && blank) { continue; }
		else if (!s.empty() && blank) { return Token::parse(s); }
		/* C2 防线：';'/','/'(' 在空 token 时直接产出对应词元（原实现借
		 * return next(sr) 尾递归绕回顶部消费 pending 标志，连续 N 个分隔符
		 * 即 N 层真实递归，--tcp 下可被远程输入打爆栈）。语义等价改直返。 */
		else if (s.empty() && c == ';') { return {TokenType::END, ""}; }
		else if (!s.empty() && c == ';') { flag_ = true; return Token::parse(s); }
		else if (s.empty() && c == ',') { return {TokenType::COMMA, ""}; }
		else if (!s.empty() && c == ',') { comma_ = true; return Token::parse(s); }
		else if (s.empty() && c == '(') { return {TokenType::LEFT, ""}; }
		else if (!s.empty() && c == ')') { rght_ = true; return Token::parse(s); }
		else {
			s += c;
			if (s.size() >= 2 && s.compare(s.size() - 2, 2, "--") == 0) {
				s.resize(s.size() - 2);
				singleLineComment_ = true;
			}
			if (s.size() >= 2 && s.compare(s.size() - 2, 2, "/*") == 0) {
				s.resize(s.size() - 2);
				multiLineComment_ = true;
				multiLineComment2_ = false;
			}
		}
	}
}

/* ------------- Parser ------------- */
Event Parser::next() {
	if (idx_ >= tokenSerial_.size()) {
		Event e;
		e.type = EventType::END;
		return e;
	}
	Token t = expect();
	switch (t.type) {
		case TokenType::CREATE: {
			Token u = expect();
			if (u.type != TokenType::TABLE) {
				throw bot::IllegalArgumentException("Token " + u.serialize());
			}
			CreateTableEvent eve;
			nextCreateTable(eve);
			Event e;
			e.type = EventType::CREATE_TABLE;
			e.create = std::move(eve);
			return e;
		}
		case TokenType::INSERT: {
			InsertTableEvent eve;
			nextInsertInto(eve);
			Event e;
			e.type = EventType::INSERT_TABLE;
			e.insert = std::move(eve);
			return e;
		}
		default:
			throw bot::IllegalArgumentException("Token " + t.serialize());
	}
}

void Parser::nextCreateTable(CreateTableEvent& eve) {
	Token t = expect();
	if (t.type != TokenType::NAME) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	eve.tableName = t.name;

	t = expect();
	if (t.type != TokenType::LEFT) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	for (;;) {
		t = expect();
		if (t.type != TokenType::NAME) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
		std::string colName = t.name;
		t = expect();
		if (t.type != TokenType::STRING && t.type != TokenType::NUMBER) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
		VarType vt = (t.type == TokenType::NUMBER) ? VarType::Number : VarType::String_;
		eve.tableColumns.push_back({colName, vt});
		columnTypes_[eve.tableName][colName] = vt;
		t = expect();
		if (t.type == TokenType::RGHT) { break; }
		if (t.type != TokenType::COMMA) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
	}
	nextEnd();
}

void Parser::nextEnd() {
	Token t = expect();
	if (t.type != TokenType::END) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
}

void Parser::nextInsertInto(InsertTableEvent& eve) {
	Token t = expect();
	if (t.type != TokenType::INTO) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	Token n = expect();
	if (n.type != TokenType::NAME) {
		throw bot::IllegalArgumentException("Token " + n.serialize());
	}
	eve.targetTabName = n.name;

	std::vector<std::string> paramNames;
	t = expect();
	if (t.type != TokenType::LEFT) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	for (;;) {
		t = expect();
		if (t.type != TokenType::NAME) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
		paramNames.push_back(t.name);
		t = expect();
		if (t.type == TokenType::RGHT) { break; }
		if (t.type != TokenType::COMMA) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
	}

	t = expect();
	if (t.type != TokenType::VALUES) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	std::vector<std::string> paramValues;
	t = expect();
	if (t.type != TokenType::LEFT) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	for (;;) {
		t = expect();
		if (t.type != TokenType::NAME) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
		paramValues.push_back(t.name);
		t = expect();
		if (t.type == TokenType::RGHT) { break; }
		if (t.type != TokenType::COMMA) {
			throw bot::IllegalArgumentException("Token " + t.serialize());
		}
	}

	if (paramNames.size() != paramValues.size()) {
		throw bot::IllegalArgumentException("Insert params");
	}
	std::vector<VarRaw> arr;
	for (std::size_t i = 0; i < paramNames.size(); ++i) {
		arr.push_back({paramNames[i], paramValues[i]});
	}
	/* Number 列的解析期校验（一次）：值必须是数值字面量，否则结构脚本在装配前
	 * 就以清晰报错结束，而不是把 'abc' 当字符串静默传给驱动。表未在本脚本中
	 * CREATE（拼错/外置）则无从校验，留给消费方的「表未定义」处理。 */
	auto schemaIt = columnTypes_.find(eve.targetTabName);
	if (schemaIt != columnTypes_.end()) {
		for (const VarRaw& p : arr) {
			auto typeIt = schemaIt->second.find(p.columnName);
			if (typeIt != schemaIt->second.end() && typeIt->second == VarType::Number
					&& !bot::isNumericLiteral(p.value)) {
				throw bot::IllegalArgumentException("Column '" + p.columnName + "' of table '"
					+ eve.targetTabName + "' is declared Number, but value '" + p.value
					+ "' is not numeric.");
			}
		}
	}
	eve.tableRows.push_back(std::move(arr));
	nextEnd();
}

} // namespace sql4cj