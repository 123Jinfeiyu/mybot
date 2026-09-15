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

/* ---- 词元引号状态：首字符为引号且未闭合 → 内部字符按字面处理。
 * SQL 值可用 ' 或 " 包裹；引号内的 "--" 与块注释起始符都是数据而非注释，
 * 注释识别只在引号外生效（修复引号内 '--' 被当注释吞掉后续 token）。 ---- */
static bool insideQuotes(const std::string& s) {
	if (s.empty()) { return false; }
	char q = s[0];
	if (q != '\'' && q != '"') { return false; }
	bool open = true;
	for (std::size_t i = 1; i < s.size(); ++i) {
		if (s[i] == q) { open = !open; }
	}
	return open;
}

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
	if (left_) { left_ = false; return {TokenType::LEFT, ""}; }
	if (rght_) { rght_ = false; return {TokenType::RGHT, ""}; }

	std::string s;
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

		if (insideQuotes(s)) { s += c; continue; }
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
	nextTableColumns(eve);
}

void Parser::nextTableColumns(CreateTableEvent& eve) {
	Token t = expect();
	if (t.type != TokenType::LEFT) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	nextTableColumnName(eve);
}

void Parser::nextTableColumnName(CreateTableEvent& eve) {
	Token t = expect();
	if (t.type != TokenType::NAME) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	nextTableColumnType(eve, t.name);
}

void Parser::nextTableColumnType(CreateTableEvent& eve, const std::string& colName) {
	Token t = expect();
	if (t.type != TokenType::STRING) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	eve.tableColumns.push_back({colName, VarType::String_});
	nextTableColumnsIsEnd(eve);
}

void Parser::nextTableColumnsIsEnd(CreateTableEvent& eve) {
	Token t = expect();
	switch (t.type) {
		case TokenType::RGHT: nextEnd(); break;
		case TokenType::COMMA: nextTableColumnName(eve); break;
		default: throw bot::IllegalArgumentException("Token " + t.serialize());
	}
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
	nextInsertParams(eve);
}

void Parser::nextInsertParams(InsertTableEvent& eve) {
	Token t = expect();
	if (t.type != TokenType::LEFT) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	std::vector<std::string> paramNames;
	nextInsertParamName(eve, paramNames);
}

void Parser::nextInsertParamName(InsertTableEvent& eve, std::vector<std::string>& paramNames) {
	Token t = expect();
	if (t.type != TokenType::NAME) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	paramNames.push_back(t.name);
	nextInsertParamNamesIsEnd(eve, paramNames);
}

void Parser::nextInsertParamNamesIsEnd(InsertTableEvent& eve, std::vector<std::string>& paramNames) {
	Token t = expect();
	switch (t.type) {
		case TokenType::RGHT: nextInsertParamValues(eve, paramNames); break;
		case TokenType::COMMA: nextInsertParamName(eve, paramNames); break;
		default: throw bot::IllegalArgumentException("Token " + t.serialize());
	}
}

void Parser::nextInsertParamValues(InsertTableEvent& eve, const std::vector<std::string>& paramNames) {
	Token t = expect();
	if (t.type != TokenType::VALUES) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	std::vector<std::string> paramValues;
	nextInsertParamValuesStart(eve, paramNames, paramValues);
}

void Parser::nextInsertParamValuesStart(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, std::vector<std::string>& paramValues) {
	Token t = expect();
	if (t.type != TokenType::LEFT) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	nextInsertParamValue(eve, paramNames, paramValues);
}

void Parser::nextInsertParamValue(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, std::vector<std::string>& paramValues) {
	Token t = expect();
	if (t.type != TokenType::NAME) {
		throw bot::IllegalArgumentException("Token " + t.serialize());
	}
	paramValues.push_back(t.name);
	nextInsertParamValuesIsEnd(eve, paramNames, paramValues);
}

void Parser::nextInsertParamValuesIsEnd(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, std::vector<std::string>& paramValues) {
	Token t = expect();
	switch (t.type) {
		case TokenType::RGHT:
			mergeAndAddInsertParamsToEve(eve, paramNames, paramValues);
			nextEnd();
			break;
		case TokenType::COMMA: nextInsertParamValue(eve, paramNames, paramValues); break;
		default: throw bot::IllegalArgumentException("Token " + t.serialize());
	}
}

void Parser::mergeAndAddInsertParamsToEve(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, const std::vector<std::string>& paramValues) {
	if (paramNames.size() != paramValues.size()) {
		throw bot::IllegalArgumentException("Insert params");
	}
	std::vector<VarRaw> arr;
	for (std::size_t i = 0; i < paramNames.size(); ++i) {
		arr.push_back({paramNames[i], paramValues[i]});
	}
	eve.tableRows.push_back(std::move(arr));
}

} // namespace sql4cj