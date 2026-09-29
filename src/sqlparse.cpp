#include "sqlparse.hpp"

#include <stdexcept>

#include "expr.hpp"
#include "util_num.hpp"

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

/* ---- 去掉首尾空白与引号（对应 parseName）----
 * 只剥**首尾各一个**引号字符（且仅当该处是引号）：#33 双写转义后值内可含
 * 字面引号（如 '''' = 一个字面引号，s = '''），旧实现贪婪剥掉全部前导/尾随
 * 引号会把字面引号也剥掉（'''' 被剥成空串）。 */
static std::string parseName(const std::string& s) {
	std::size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
	if (b < e && (s[b] == '\'' || s[b] == '"')) { ++b; }
	if (e > b && (s[e-1] == '\'' || s[e-1] == '"')) { --e; }
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
	if (equalWithoutCase(s, "where")) { return {TokenType::WHERE, s}; }
	if (equalWithoutCase(s, "delete")) { return {TokenType::DELETE, s}; }
	if (equalWithoutCase(s, "drop")) { return {TokenType::DROP, s}; }
	if (equalWithoutCase(s, "update")) { return {TokenType::UPDATE, s}; }
	if (equalWithoutCase(s, "set")) { return {TokenType::SET, s}; }
	if (equalWithoutCase(s, "and")) { return {TokenType::AND, s}; }
	if (equalWithoutCase(s, "show")) { return {TokenType::SHOW, s}; }
	if (equalWithoutCase(s, "tables")) { return {TokenType::TABLES, s}; }
	if (s == ";") { return {TokenType::END, s}; }
	if (s == ",") { return {TokenType::COMMA, s}; }
	if (s == "(") { return {TokenType::LEFT, s}; }
	if (s == ")") { return {TokenType::RGHT, s}; }
	if (s == "=") { return {TokenType::EQ, s}; }
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
		case TokenType::WHERE: return "WHERE";
		case TokenType::DELETE: return "DELETE";
		case TokenType::DROP: return "DROP";
		case TokenType::UPDATE: return "UPDATE";
		case TokenType::SET: return "SET";
		case TokenType::AND: return "AND";
		case TokenType::SHOW: return "SHOW";
		case TokenType::TABLES: return "TABLES";
		case TokenType::END: return ";";
		case TokenType::COMMA: return ",";
		case TokenType::LEFT: return "(";
		case TokenType::RGHT: return ")";
		case TokenType::EQ: return "=";
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

/* F-02/#33：引号态内单字符处理（2026-09-29 A3 拆自 next，语义逐字保持）。
 * 同种引号再现时预读下一字符：仍是同种引号 → '' 双写转义，追加一个字面
 * 引号并保持引号态；否则本引号闭合，预读字符缓存到 pendingChar_ 交由
 * next() 主循环按常规处理（注释/分隔符/新 token）。其余字符按字面追加。
 * 位置推进（含预读字符）与原实现一致。 */
void Lexer::appendQuoted(bot::Reader& sr, char c, bool& inQuote, char quoteChar,
		std::string& s) {
	if (c == quoteChar) {
		int ci2 = sr.read();
		if (ci2 >= 0) {
			char c2 = static_cast<char>(ci2);
			if (c2 == '\n') { line_ += 1; col_ = 1; } else { col_ += 1; }
			if (c2 == quoteChar) { s += quoteChar; return; }
			pendingChar_ = ci2;
		}
	}
	s += c;
	if (c == quoteChar) { inQuote = false; }
}

Token Lexer::next(bot::Reader& sr) {
	if (pendSep_) {
		const TokenType t = *pendSep_;
		pendSep_.reset();
		return positionedToken(t, pendLine_, pendCol_);
	}

	std::string s;
	int tokLine = 0, tokCol = 0;	/* 当前 token 首字符落点（1 起） */
	bool inQuote = false;		/* F-02：引号态由本函数增量维护（O(n)），
					 * 取代旧实现每字符重扫 token 的 O(n²) */
	char quoteChar = 0;		/* 打开引号态的字符：' 或 " */
	while (true) {
		int ci;
		if (pendingChar_ >= 0) { ci = pendingChar_; pendingChar_ = -1; }
		else { ci = sr.read(); }
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
		 * 只可能发生在 token 首字符处（与旧 insideQuotes 的 s[0] 判据等价）。
		 * #33：SQL 标准 '' 双写转义——同种引号再现时预读下一字符：仍是同种
		 * 引号 → 追加一个字面引号并**保持引号态**；否则本引号闭合，预读字符
		 * 缓存到 pendingChar_ 交由主循环按常规处理（注释/分隔符/新 token）。 */
		if (inQuote) {
			appendQuoted(sr, c, inQuote, quoteChar, s);
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
		/* C2 防线：';'/','/'('/')'/'=' 在空 token 时直接产出对应词元（原实现借
		 * return next(sr) 尾递归绕回顶部消费 pending 标志，连续 N 个分隔符
		 * 即 N 层真实递归，--tcp 下可被远程输入打爆栈）。语义等价改直返。
		 * 评估 2026-09-20：四个分隔符一律「空 token 直返 / 非空 token 挂
		 * pending 标志」，此前 '(' 只处理空 token 一侧、')' 只处理非空
		 * 一侧——`INSERT INTO T(a,b) VALUES(1,2)`（SQL 空白不敏感的常见
		 * 写法）里 '(' 会被并进名字（`T(a` 之类），解析以 `Token ,` 报错
		 * 收场，而仓内夹具一律写成 `T (a, b)` 故从未暴露。
		 * '=' 加入（2026-09-23 WHERE）：同模式第五个分隔符——`WHERE col=value`
		 * 紧贴写法靠 pending 切分。仓内 robots/ 全部 .sql 的 '=' 都在引号
		 * 值或注释里（144 个文件逐一扫描），引号态在分隔符判定之前消费，
		 * 零行为变化。 */
		else if (s.empty() && c == ';') { return positionedToken(TokenType::END, curLine, curCol); }
		else if (!s.empty() && c == ';') {
			pendSep_ = TokenType::END; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
		else if (s.empty() && c == ',') { return positionedToken(TokenType::COMMA, curLine, curCol); }
		else if (!s.empty() && c == ',') {
			pendSep_ = TokenType::COMMA; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
		else if (s.empty() && c == '(') { return positionedToken(TokenType::LEFT, curLine, curCol); }
		else if (!s.empty() && c == '(') {
			pendSep_ = TokenType::LEFT; pendLine_ = curLine; pendCol_ = curCol;
			return finishToken(s, tokLine, tokCol);
		}
			else if (s.empty() && c == ')') { return positionedToken(TokenType::RGHT, curLine, curCol); }
			else if (!s.empty() && c == ')') {
				pendSep_ = TokenType::RGHT; pendLine_ = curLine; pendCol_ = curCol;
				return finishToken(s, tokLine, tokCol);
			}
			else if (s.empty() && c == '=') { return positionedToken(TokenType::EQ, curLine, curCol); }
			else if (!s.empty() && c == '=') {
				pendSep_ = TokenType::EQ; pendLine_ = curLine; pendCol_ = curCol;
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
		case TokenType::SHOW: {
			nextShowTables();
			Event e;
			e.type = EventType::SHOW_TABLES;
			return e;
		}
		case TokenType::DELETE: {
			DeleteTableEvent eve;
			nextDelete(eve);
			Event e;
			e.type = EventType::DELETE_TABLE;
			e.del = std::move(eve);
			return e;
		}
		case TokenType::DROP: {
			DropTableEvent eve;
			nextDrop(eve);
			Event e;
			e.type = EventType::DROP_TABLE;
			e.drop = std::move(eve);
			return e;
		}
		case TokenType::UPDATE: {
			UpdateTableEvent eve;
			nextUpdate(eve);
			Event e;
			e.type = EventType::UPDATE_TABLE;
			e.update = std::move(eve);
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
	validateNumberValues(eve.targetTabName, arr);
	eve.tableRows.push_back(std::move(arr));
	nextEnd();
}

/* Number 列的解析期校验（单点，INSERT 的 VALUES 与 UPDATE 的 SET 共用）：
 * 值必须是数值字面量，否则结构脚本在装配前就以清晰报错结束，而不是把 'abc'
 * 当字符串静默传给驱动。表未在本脚本中 CREATE（拼错/外置）则无从校验，
 * 留给消费方的「表未定义」处理。 */
void Parser::validateNumberValues(const std::string& tableName,
		const std::vector<VarRaw>& values) {
	auto schemaIt = columnTypes_.find(tableName);
	if (schemaIt == columnTypes_.end()) { return; }
	for (const VarRaw& p : values) {
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
				+ tableName + "' is declared Number, but value '" + p.value
				+ "' is not numeric.");
		}
	}
}

/* SELECT：`SELECT * FROM t [WHERE col = value [AND ...]]`。只支持星号选择列
 * （* 经 Token::parse 落入 NAME，载荷为 '*'）；`SELECT <name>` 与非星号/缺 FROM/
 * 缺表名一律报错，报错形态对齐链路里既有的 tokenMsg（带行列定位）。 */
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
	/* 可选 WHERE：peek 不消费，命中才进条件列表；分号仍由 nextEnd 消费 */
	if (peekIs(TokenType::WHERE)) {
		++idx_;
		WhereClause w;
		nextConditionList(w);
		eve.where = std::move(w);
	}
	nextEnd();
}

/* SHOW TABLES：`SHOW TABLES;`——只消费 TABLES 关键字与分号，无载荷（表名清单在
 * 消费方枚举运行时矩阵得到）。不带 WHERE/LIKE；`SHOW TABLE;`（漏 S）在此以
 * expectType 的「Token TABLE」报错，尾随多余词元（含误写的 LIKE）由 nextEnd 兜住，
 * 两者都自带行列定位。 */
void Parser::nextShowTables() {
	expectType(TokenType::TABLES);
	nextEnd();
}

/* DELETE：`DELETE FROM t [WHERE col = value [AND ...]]`。与 SELECT 同构，
 * 差别只在于「无条件」是合法且有意义的（删全表），故 where 缺省即空。 */
void Parser::nextDelete(DeleteTableEvent& eve) {
	Token from = expectType(TokenType::FROM);
	(void)from;
	Token name = expectType(TokenType::NAME);
	eve.targetTabName = name.name;
	if (peekIs(TokenType::WHERE)) {
		++idx_;
		WhereClause w;
		nextConditionList(w);
		eve.where = std::move(w);
	}
	nextEnd();
}

/* DROP：`DROP TABLE t`。语法比 DELETE 更简（无 FROM、**无 WHERE**）：DROP 是 DDL，
 * 作用于整张表；WHERE 是 DML 的行级筛选，加在这里会「长得像在筛行、实际却整表删」。
 * 尾随的额外词元（含误写的 WHERE）由 nextEnd 以词元级报错兜住，自带行列定位。 */
void Parser::nextDrop(DropTableEvent& eve) {
	expectType(TokenType::TABLE);
	Token name = expectType(TokenType::NAME);
	eve.targetTabName = name.name;
	nextEnd();
}

/* UPDATE：`UPDATE t SET col = value [, col2 = value2 ...] [WHERE ...]`。
 * 赋值列表与 WHERE 条件列表同构（NAME EQ NAME），差别只在连接词（逗号 vs AND）
 * 与终止判据——因此终止同样用 peek 而非 expect：只有 COMMA 才继续下一轮，
 * 其余（';' / EOF / 多写的 OR 之类）一律留给调用方的 nextEnd 报词元级错。
 * 赋值列的重复/未知属「对着结构才能判的错」，与 INSERT 同落点（readSQL），
 * 这里只做纯语法与 Number 列校验。语义（断电重建）见 UpdateTableEvent 注释。 */
void Parser::nextUpdate(UpdateTableEvent& eve) {
	Token name = expectType(TokenType::NAME);
	eve.targetTabName = name.name;
	expectType(TokenType::SET);
	auto setErr = [](const Token& t) {
		return bot::IllegalArgumentException("SET supports only '<column> = <value>'"
			" assignments joined by ',' (got " + tokenMsg(t) + ")");
	};
	for (;;) {
		Token col = expect();
		if (col.type != TokenType::NAME) { throw setErr(col); }
		Token eq = expect();
		if (eq.type != TokenType::EQ) { throw setErr(eq); }
		Token val = expect();
		if (val.type != TokenType::NAME) { throw setErr(val); }
		eve.assignments.push_back({col.name, val.name});
		if (!peekIs(TokenType::COMMA)) { break; }
		++idx_;
	}
	validateNumberValues(eve.targetTabName, eve.assignments);
	/* 可选 WHERE：与 SELECT/DELETE 逐字同构（peek 不消费，命中才进条件列表） */
	if (peekIs(TokenType::WHERE)) {
		++idx_;
		WhereClause w;
		nextConditionList(w);
		eve.where = std::move(w);
	}
	nextEnd();
}

/* WHERE 条件列表：`col = value [AND col2 = value2 ...]`（AND 大小写不敏感）。
 * 单条件与多条件共用本函数——SELECT/DELETE 的 WHERE 语法、报错锚点、词元消费
 * 顺序都只此一处，两语句不会漂移。
 * 报错形态：三个位置（列名/等号/值）共用一句**用法级**报错，从「Token ,」
 * 反推 WHERE 语法对人对模型都太绕（取向同 isUnsupportedStatement）。
 * 终止判据是 peek 而非 expect：只有 AND 才继续下一轮，其余（';' / EOF /
 * 多写的 OR 之类）一律把词元留给调用方的 nextEnd——那里既有「Token ...」
 * 报错自带行列，比在本函数里猜「用户还想写什么」更准确。 */
void Parser::nextConditionList(WhereClause& eve) {
	auto whereErr = [](const Token& t) {
		return bot::IllegalArgumentException("WHERE supports only '<column> = <value>'"
			" conditions joined by AND (got " + tokenMsg(t) + ")");
	};
	for (;;) {
		Token col = expect();
		if (col.type != TokenType::NAME) { throw whereErr(col); }
		Token eq = expect();
		if (eq.type != TokenType::EQ) { throw whereErr(eq); }
		Token val = expect();
		if (val.type != TokenType::NAME) { throw whereErr(val); }
		eve.conds.push_back({col.name, val.name});
		if (!peekIs(TokenType::AND)) { break; }
		++idx_;
	}
}

} // namespace sql4cj