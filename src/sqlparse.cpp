#include "sqlparse.hpp"

#include <stdexcept>

#include "expr.hpp"

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
		case TokenType::NAME: return "name '" + name + "'";
	}
	return "?";
}

/* 注释态机（阶段 3 拆分）：单行 `--` 与块注释的状态转移，字符被注释
 * 态消费即返回 true（调用方 continue）。转移语义与原内联 if/else 链逐项一致。 */
bool Lexer::consumeCommentChar(char c) {
	if (singleLineComment_ && c != '\n') { return true; }
	if (singleLineComment_ && c == '\n') { singleLineComment_ = false; return true; }
	if (multiLineComment_ && !multiLineComment2_ && c != '*') { return true; }
	if (multiLineComment_ && !multiLineComment2_ && c == '*') { multiLineComment2_ = true; return true; }
	if (multiLineComment_ && multiLineComment2_ && c != '/') { multiLineComment2_ = false; return true; }
	if (multiLineComment_ && multiLineComment2_ && c == '/') {
		multiLineComment_ = false;
		multiLineComment2_ = false;
		return true;
	}
	return false;
}

/* ------------- Lexer ------------- */

namespace {

/* F5-1：给词元补落点（Token::parse 是无位置的静态分类器） */
Token finishToken(const std::string& s, int line, int col) {
	Token t = Token::parse(s);
	t.line = line;
	t.col = col;
	return t;
}

Token positionedToken(TokenType type, int line, int col) {
	Token t;
	t.type = type;
	t.line = line;
	t.col = col;
	return t;
}

} // namespace

Token Lexer::next(bot::Reader& sr) {
	if (flag_) { flag_ = false; return positionedToken(TokenType::END, pendLine_, pendCol_); }
	if (comma_) { comma_ = false; return positionedToken(TokenType::COMMA, pendLine_, pendCol_); }
	if (rght_) { rght_ = false; return positionedToken(TokenType::RGHT, pendLine_, pendCol_); }
	if (left_) { left_ = false; return positionedToken(TokenType::LEFT, pendLine_, pendCol_); }

	std::string s;
	int tokLine = 0, tokCol = 0;	/* 当前 token 首字符落点（1 起） */
	bool inQuote = false;		/* F-02：引号态由本函数增量维护（O(n)），
					 * 取代旧实现每字符重扫 token 的 O(n²) */
	char quoteChar = 0;		/* 打开引号态的字符：' 或 " */
	while (true) {
		int ci = sr.read();
		if (ci < 0) {
			if (inQuote) {
				/* F5-1：引号态跨越 EOF——旧实现把半截串剥引号后当名字
				 * 静默放行（'abc 结尾 → 名字 abc），这里改为报错并给出
				 * 开引号落点。注意只在真 EOF 抛：闭合引号已在下方处理。 */
				throw bot::IllegalArgumentException("Unterminated string starting at line "
					+ std::to_string(tokLine) + ", column " + std::to_string(tokCol));
			}
			if (s.empty()) { return {TokenType::UNEXPECTED_END, ""}; }
			return finishToken(s, tokLine, tokCol);
		}
		char c = static_cast<char>(ci);
		const int curLine = line_, curCol = col_;
		if (c == '\n') { line_ += 1; col_ = 1; } else { col_ += 1; }
		if (consumeCommentChar(c)) { continue; }

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
			tokLine = curLine;
			tokCol = curCol;
			s += c;
			continue;
		}
		if (s.empty() && blank) { continue; }
		else if (!s.empty() && blank) { return finishToken(s, tokLine, tokCol); }
		/* C2 防线：';'/','/'('/')' 在空 token 时直接产出对应词元（原实现借
		 * return next(sr) 尾递归绕回顶部消费 pending 标志，连续 N 个分隔符
		 * 即 N 层真实递归，--tcp 下可被远程输入打爆栈）。语义等价改直返。
		 * 评估 2026-09-20：四个分隔符一律「空 token 直返 / 非空 token 挂
		 * pending 标志」，此前 '(' 只处理空 token 一侧、')' 只处理非空
		 * 一侧——`INSERT INTO T(a,b) VALUES(1,2)`（SQL 空白不敏感的常见
		 * 写法）里 '(' 会被并进名字（`T(a` 之类），解析以 `Token ,` 报错
		 * 收场，而仓内夹具一律写成 `T (a, b)` 故从未暴露。 */
		else if (s.empty() && c == ';') { return positionedToken(TokenType::END, curLine, curCol); }
		else if (!s.empty() && c == ';') {
			flag_ = true; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
		else if (s.empty() && c == ',') { return positionedToken(TokenType::COMMA, curLine, curCol); }
		else if (!s.empty() && c == ',') {
			comma_ = true; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
		else if (s.empty() && c == '(') { return positionedToken(TokenType::LEFT, curLine, curCol); }
		else if (!s.empty() && c == '(') {
			left_ = true; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
		else if (s.empty() && c == ')') { return positionedToken(TokenType::RGHT, curLine, curCol); }
		else if (!s.empty() && c == ')') {
			rght_ = true; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
		else {
			if (s.empty()) { tokLine = curLine; tokCol = curCol; }
			s += c;
			if (s.size() >= 2 && s.compare(s.size() - 2, 2, "--") == 0) {
				s.resize(s.size() - 2);
				singleLineComment_ = true;
				/* C3-x：注释起始须先提交挂起 token，否则注释结束后文本拼回同一
				 * token（"50--c\n60" → "5060"），静默产出错误数值。 */
				if (!s.empty()) { return finishToken(s, tokLine, tokCol); }
			}
			if (s.size() >= 2 && s.compare(s.size() - 2, 2, "/*") == 0) {
				s.resize(s.size() - 2);
				multiLineComment_ = true;
				multiLineComment2_ = false;
				if (!s.empty()) { return finishToken(s, tokLine, tokCol); }
			}
		}
	}
}

/* ------------- Parser ------------- */

namespace {

/* F5-1：报错形态 = 既有「Token <词元>」+ 落点段。落点缺失（手工构造词元）
 * 时省略，保持旧形态——E2E 与单测的「Token …」子串锚点因此都不受影响。 */
std::string tokenMsg(const Token& t) {
	std::string m = "Token " + t.serialize();
	if (t.line > 0) {
		m += " (line " + std::to_string(t.line) + ", column " + std::to_string(t.col) + ")";
	}
	return m;
}

} // namespace

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
				throw bot::IllegalArgumentException(tokenMsg(u));
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
		case TokenType::SELECT: {
			SelectTableEvent eve;
			nextSelect(eve);
			Event e;
			e.type = EventType::SELECT_TABLE;
			e.select = std::move(eve);
			return e;
		}
		default:
			throw bot::IllegalArgumentException(tokenMsg(t));
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
		throw bot::IllegalArgumentException(tokenMsg(t));
	}
	for (;;) {
		t = expect();
		if (t.type != TokenType::NAME) {
			throw bot::IllegalArgumentException(tokenMsg(t));
		}
		std::string colName = t.name;
		t = expect();
		if (t.type != TokenType::STRING && t.type != TokenType::NUMBER) {
			throw bot::IllegalArgumentException(tokenMsg(t));
		}
		VarType vt = (t.type == TokenType::NUMBER) ? VarType::Number : VarType::String_;
		eve.tableColumns.push_back({colName, vt});
		columnTypes_[eve.tableName][colName] = vt;
		t = expect();
		if (t.type == TokenType::RGHT) { break; }
		if (t.type != TokenType::COMMA) {
			throw bot::IllegalArgumentException(tokenMsg(t));
		}
	}
	nextEnd();
}

void Parser::nextEnd() {
	Token t = expect();
	if (t.type != TokenType::END) {
		throw bot::IllegalArgumentException(tokenMsg(t));
	}
}

/* 期望下一个词元为指定类型，否则抛既有的「Token ...」报错（阶段 3 收敛：
 * 此前在本函数内五段逐字重复） */
Token Parser::expectType(TokenType want) {
	Token t = expect();
	if (t.type != want) {
		throw bot::IllegalArgumentException(tokenMsg(t));
	}
	return t;
}

/* 括号包裹的名字列表：( n1, n2, ... )——INSERT 的列名清单与 VALUES 清单
 * 两段逐字同构（阶段 3 收敛） */
std::vector<std::string> Parser::expectParenNameList() {
	std::vector<std::string> names;
	expectType(TokenType::LEFT);
	for (;;) {
		Token t = expectType(TokenType::NAME);
		names.push_back(t.name);
		t = expect();
		if (t.type == TokenType::RGHT) { break; }
		if (t.type != TokenType::COMMA) {
			throw bot::IllegalArgumentException(tokenMsg(t));
		}
	}
	return names;
}

void Parser::nextInsertInto(InsertTableEvent& eve) {
	expectType(TokenType::INTO);
	Token n = expectType(TokenType::NAME);
	eve.targetTabName = n.name;

	std::vector<std::string> paramNames = expectParenNameList();
	expectType(TokenType::VALUES);
	std::vector<std::string> paramValues = expectParenNameList();

	if (paramNames.size() != paramValues.size()) {
		/* fix-plan 3-4：报错带表名与两侧列数，定位质量对齐同文件 Number 列报错 */
		throw bot::IllegalArgumentException("Insert into table '" + eve.targetTabName
			+ "' has " + std::to_string(paramNames.size()) + " column(s) but "
			+ std::to_string(paramValues.size()) + " value(s).");
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
			if (typeIt == schemaIt->second.end() || typeIt->second != VarType::Number) { continue; }
			/* Number 列允许数值字面量或 $(<表达式>)（表达式语法在此早校验，
			 * 标识符的可达性由 @Vars/元数据层在装配后判定）。 */
			if (auto body = bot::exprBody(p.value)) {
				bot::Expression::parse(*body);
				continue;
			}
			if (!bot::isNumericLiteral(p.value)) {
				throw bot::IllegalArgumentException("Column '" + p.columnName + "' of table '"
					+ eve.targetTabName + "' is declared Number, but value '" + p.value
					+ "' is not numeric.");
			}
		}
	}
	eve.tableRows.push_back(std::move(arr));
	nextEnd();
}

/* SELECT：`SELECT * FROM t`。只支持星号选择列（* 经 Token::parse 落入 NAME，
 * 载荷为 '*'）；`SELECT <name>` 与非星号/缺 FROM/缺表名一律报错，报错形态
 * 对齐链路里既有的 tokenMsg（带行列定位）。 */
void Parser::nextSelect(SelectTableEvent& eve) {
	Token star = expect();
	if (star.type != TokenType::NAME || star.name != "*") {
		throw bot::IllegalArgumentException(
			"SELECT supports only 'SELECT * FROM <table>' (got "
			+ tokenMsg(star) + ")");
	}
	Token from = expectType(TokenType::FROM);
	(void)from;
	Token name = expectType(TokenType::NAME);
	eve.sourceTableName = name.name;
	nextEnd();
}

} // namespace sql4cj