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
	if (equalWithoutCase(s, "where")) { return {TokenType::WHERE, s}; }
	if (equalWithoutCase(s, "delete")) { return {TokenType::DELETE, s}; }
	if (equalWithoutCase(s, "drop")) { return {TokenType::DROP, s}; }
	if (equalWithoutCase(s, "and")) { return {TokenType::AND, s}; }
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
		case TokenType::AND: return "AND";
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

Token Lexer::next(bot::Reader& sr) {
	if (flag_) { flag_ = false; return positionedToken(TokenType::END, pendLine_, pendCol_); }
	if (comma_) { comma_ = false; return positionedToken(TokenType::COMMA, pendLine_, pendCol_); }
	if (rght_) { rght_ = false; return positionedToken(TokenType::RGHT, pendLine_, pendCol_); }
	if (left_) { left_ = false; return positionedToken(TokenType::LEFT, pendLine_, pendCol_); }
	if (eq_) { eq_ = false; return positionedToken(TokenType::EQ, pendLine_, pendCol_); }

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
			else if (s.empty() && c == '=') { return positionedToken(TokenType::EQ, curLine, curCol); }
			else if (!s.empty() && c == '=') {
				eq_ = true; pendLine_ = curLine; pendCol_ = curCol;
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

/* 方言支持语句的**单一事实源**：拒绝文案、SELECT/DELETE 的用法级报错都从这里
 * 取，避免「支持了什么」在四五条消息里各写一份而随语句增减漂移（本文件此前
 * 已有 `SELECT supports only ...` 与 `WHERE supports only ...` 两处各说一遍）。
 * 措辞刻意用「方言支持」而非「已实现」：读的人（含模型）要的是「能写什么」，
 * 不是变更史。 */
constexpr const char* kDialectStatements =
	"CREATE TABLE / INSERT INTO / SELECT ... FROM ... [WHERE col = value [AND ...]] /"
	" DELETE FROM ... [WHERE col = value [AND ...]] / DROP TABLE ...";

/* 明确拒绝的「看似合法」语句关键字（大小写不敏感）。本方言只认上面那五条，
 * 而 UPDATE 是**改写结构定义**的语句：在带电机器人上它会让结构矩阵与硬件注册表
 * 失去一致性（见 RobotStruct::applySql 的契约），不属于「解析了就能安全执行」的
 * 范畴，暂不支持（等价于「删旧行 + 建新行」，将来可复用 DELETE + INSERT 的设施）。
 *
 * DELETE（2026-09-23）与 DROP（2026-09-23）都已出列：DELETE 删的是**行**，DROP
 * 删的是**表**（行 = 表的全部数据行 + 表壳）。两者共用同一条「断电 → 撤注册 → 擦行」
 * 的逆序路径，落点在 applySql；DROP 额外要把表壳连同该表的元数据提供者/守护登记
 * 一起撤下，并处理「表撤销后同名重建」的语义面。
 *
 * 为什么要专门认出来：它不是关键字（词法层落成 NAME），过去的失败形态是
 * "Token name 'update'"——层级是词元的，读的人（含模型）无法判断这是「语法写错」
 * 还是「方言不支持」，只会改写措辞反复重试。这里给一句含正确用法的拒绝。
 * 判据是**语句首词元**：出现在后续位置的同名标识符（列名/表名/值）不受影响。
 * 将来支持时的落点：EventType 加对应事件 → 此清单逐个摘除（DELETE/DROP 即按此路径
 * 摘除）→ runSqlCommand 的只读/变更分类扩展；WHERE 设施（WhereClause +
 * nextConditionList）已按共享写成，UPDATE 可直接复用。事务语义警示：UPDATE 改
 * 结构定义，必须走 applySql 式「校验全过才动手」。 */
bool isUnsupportedStatement(const std::string& name) {
	return equalWithoutCase(name, "update");
}

/* 回显统一大写（ASCII）：既是 SQL 的惯例写法，也让报错文本成为**稳定锚点**——
 * 否则同一条语句因输入大小写不同而形态各异，单测/E2E 与日志检索都得写两套。 */
std::string upperAscii(const std::string& s) {
	std::string o = s;
	for (char& c : o) {
		if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); }
	}
	return o;
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
		default:
			/* 语句首词元是改写/删表类语句 → 明确的「不支持」，而不是词元级报错 */
			if (t.type == TokenType::NAME && isUnsupportedStatement(t.name)) {
				throw bot::IllegalArgumentException("Unsupported statement " + upperAscii(t.name)
					+ ": this dialect understands " + kDialectStatements);
			}
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