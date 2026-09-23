#pragma once
/* sqlparse.hpp —— 结构脚本 SQL 解析器（移植 SQL4cj：Lexer / Parser / Event） */

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

namespace sql4cj {

/* ------------- Token ------------- */
enum class TokenType {
	SELECT, FROM, CREATE, TABLE, STRING, NUMBER,
	INSERT, INTO, VALUES, WHERE, DELETE, AND,
	END, COMMA, LEFT, RGHT, EQ,
	UNEXPECTED_END, UNKNOWN,
	NAME
};

struct Token {
	TokenType type = TokenType::UNKNOWN;
	std::string name;	/* NAME 载荷 */
	/* F5-1：词元首字符的 1 起行号/列号（列按字节计，与词法器逐字符推进一致；
	 * 0 = 位置未知——手工构造的词元，报错时省略定位段）。 */
	int line = 0;
	int col = 0;

	static Token parse(const std::string& s);

	std::string serialize() const;
	bool isTokenEnd() const { return type == TokenType::UNEXPECTED_END; }
};

/* ------------- 事件载荷 ------------- */
/* 列类型：本方言的值一律以字符串驻留（MatrixObject 只存 String），Number 目前
 * 仅在解析期对字面量做一次数值校验，运行期仍按文本传递。 */
enum class VarType { String_ = 0, Number };

struct TableColumn {
	std::string name;
	VarType type_;
};


struct CreateTableEvent {
	std::string tableName;
	std::vector<TableColumn> tableColumns;
};

struct VarRaw {
	std::string columnName;
	std::string value;
};

struct InsertTableEvent {
	std::string targetTabName;
	std::vector<std::vector<VarRaw>> tableRows;
};

/* WHERE 等值条件：`WHERE <列名> = <值>`。共享载荷——SELECT 与 DELETE 共用，
 * 未来的 UPDATE t SET ... WHERE ... 复用同一结构与同一解析入口
 * （Parser::nextConditionList），不各造一份。
 * 只承载结构（列名 + 值文本），求值（按行匹配）在消费方——与 SELECT 事件
 * 「不执行查询」的职责划分一致。值的引号在词法层已剥（parseName），
 * 运行期按文本精确比较（数值按文本比：'0x40' ≠ '64'，已知边界）。 */
struct WhereEqual {
	std::string column;
	std::string value;
};

/* 条件列表（2026-09-23 热删除）：一个或多个等值条件，**AND 语义**。
 * 空 conds = 无条件（`DELETE FROM t;` 删全表；SELECT 的 WHERE 缺省即不构造
 * WhereClause，二者的「无」都由 optional 表达，不另设标志位）。
 * 为什么不设 OR：本方言的取向是「能用一句话说清就够」——OR 会让「删哪几行」
 * 变成需要脑内求值的集合运算，而删除是物理不可逆操作，操作者/模型必须能一眼
 * 读出命中范围。需要 OR 时拆成两条语句即可（每条各自独立校验与报数）。 */
struct WhereClause {
	std::vector<WhereEqual> conds;
};

/* SELECT 事件：承载目标表名（`SELECT * FROM t`）与可选等值条件
 * （`WHERE col = value [AND col2 = value2 ...]`）。真正的查询执行（从运行时
 * 矩阵取数、按条件过滤、渲染）在消费方（REPL /sql 命令）完成——解析器保持
 * 「只产出结构事件」的最小职责，不引入 SELECT AST。 */
struct SelectTableEvent {
	std::string sourceTableName;	/* FROM 后的表名 */
	std::optional<WhereClause> where;	/* WHERE 条件（缺省无） */
};

/* DELETE 事件（2026-09-23 热删除）：承载目标表名与可选条件。
 * where 为空 = 无条件，即删除该表全部行（SQL 语义，也是「整件撤下」的写法）。
 * 本事件同样只承载结构，**不决定删除顺序、不做引用校验**——那是 RobotStruct::
 * applySql 热删除事务的职责（预检 → 物理断电 → 撤注册），解析层无权碰活体。 */
struct DeleteTableEvent {
	std::string targetTabName;
	std::optional<WhereClause> where;
};

enum class EventType { CREATE_TABLE, INSERT_TABLE, SELECT_TABLE, DELETE_TABLE, END };

struct Event {
	EventType type;
	CreateTableEvent create;
	InsertTableEvent insert;
	SelectTableEvent select;
	DeleteTableEvent del;
};

/* ------------- 词法分析器 ------------- */
class Lexer {
	bool flag_ = false;		/* 读到分号结尾 */
	bool comma_ = false;		/* 逗号 */
	bool rght_ = false;		/* 右括号 */
	bool left_ = false;		/* 左括号（紧随一个已提交的 token 时挂起） */
	bool eq_ = false;		/* 等号（同上：WHERE col=value 紧贴写法靠它切分） */
	bool singleLineComment_ = false;
	bool multiLineComment_ = false;
	bool multiLineComment2_ = false;
	/* F5-1：行列追踪。line_/col_ = 下一个将读字符的位置（1 起）；
	 * pendLine_/pendCol_ = 挂起分隔符字符的落点（挂起标志在字符消费时设置，
	 * 词元要到下一次 next() 才产出，落点必须先存）。 */
	int line_ = 1;
	int col_ = 1;
	int pendLine_ = 0;
	int pendCol_ = 0;

public:
	Token next(bot::Reader& sr);

	/* 阶段 3 拆分：注释态字符消费（单行/块注释状态机，见实现处） */
	bool consumeCommentChar(char c);

	static std::vector<Token> parseAll(bot::Reader& sr) {
		Lexer l;
		std::vector<Token> arr;
		while (true) {
			Token t = l.next(sr);
			if (t.isTokenEnd()) { break; }
			arr.push_back(t);
		}
		return arr;
	}
};

/* ------------- 语法分析器 ------------- */
class Parser {
	std::vector<Token> tokenSerial_;
	std::size_t idx_ = 0;
	/* 建表期登记的列类型：表名 -> (列名 -> 类型)。供 INSERT 对 Number 列做
	 * 一次解析期数值校验；运行期不保留类型（值仍是字符串）。 */
	std::map<std::string, std::map<std::string, VarType>> columnTypes_;

	/* 消费下一个 token 或抛错 */
	const Token& expect() {
		if (idx_ >= tokenSerial_.size()) {
			throw bot::IllegalArgumentException("Token <unexpected end of script>");
		}
		return tokenSerial_[idx_++];
	}

	/* 不消费的前瞻判据（nextSelect/nextDelete 判断有没有 WHERE、
	 * nextConditionList 判断 AND 之后还有没有条件）。越界一律 false——
	 * 脚本意外结束交由 expect() 报「unexpected end of script」，本函数不抢那句话。 */
	bool peekIs(TokenType want) const {
		return idx_ < tokenSerial_.size() && tokenSerial_[idx_].type == want;
	}

	/* M-1：CREATE/INSERT 的列段与参数段原为按元素互递归的下降链
	 * （nextTableColumnName↔nextTableColumnsIsEnd 等），递归深度 = 列数/参数数，
	 * 实测 3 万列结构脚本即栈溢出裸崩。改显式循环：token 消费顺序与错误
	 * 消息（Token <serialize()>）与旧实现逐一相同，仅执行模型变化。 */
	void nextCreateTable(CreateTableEvent& eve);
	void nextEnd();

	/* 阶段 3 拆分：类型期望断言 / 括号名字清单（契约见实现处） */
	Token expectType(TokenType want);
	std::vector<std::string> expectParenNameList();

	void nextInsertInto(InsertTableEvent& eve);
	/* SELECT：`SELECT * FROM t`——只解析星号与目标表名，产出 SelectTableEvent。
	 * 表名后可跟一个等值条件列表：`WHERE col = value [AND ...]`（见 nextConditionList）。 */
	void nextSelect(SelectTableEvent& eve);
	/* DELETE：`DELETE FROM t [WHERE col = value [AND ...]]`——只解析目标表名与
	 * 条件列表（与 SELECT 共用 nextConditionList），产出 DeleteTableEvent。
	 * where 为空即无条件删除全表。执行（断电/撤注册/擦行）不在这里。 */
	void nextDelete(DeleteTableEvent& eve);
	/* WHERE 条件列表：一个或多个 `列名 = 值`，以 AND 连接（大小写不敏感）。
	 * 列名/值各一个 NAME 词元（值支持引号串，引号已在词法层剥）；比较运算符、
	 * OR、IN 不在本方言（负例由本函数的用法级报错或调用方 nextEnd 兜住）。
	 * SELECT 与 DELETE 共用——两条语句的 WHERE 语法与报错锚点因此不会漂移。 */
	void nextConditionList(WhereClause& eve);

public:
	explicit Parser(const std::vector<Token>& tokenSerial) : tokenSerial_(tokenSerial) {}

	Event next();

	static std::vector<Event> parseAll(const std::vector<Token>& arr) {
		Parser p(arr);
		std::vector<Event> eves;
		while (true) {
			Event e = p.next();
			if (e.type == EventType::END) { break; }
			eves.push_back(std::move(e));
		}
		return eves;
	}
};

} // namespace sql4cj