#pragma once
/* sqlparse.hpp —— 结构脚本 SQL 解析器（移植 SQL4cj：Lexer / Parser / Event） */

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "util.hpp"

namespace sql4cj {

/* ------------- Token ------------- */
enum class TokenType {
	SELECT, FROM, CREATE, TABLE, STRING, NUMBER,
	INSERT, INTO, VALUES,
	END, COMMA, LEFT, RGHT,
	UNEXPECTED_END, UNKNOWN,
	NAME
};

struct Token {
	TokenType type = TokenType::UNKNOWN;
	std::string name;	/* NAME 载荷 */

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

struct BlankEvent {};

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

enum class EventType { CREATE_TABLE, INSERT_TABLE, END };

struct Event {
	EventType type;
	CreateTableEvent create;
	InsertTableEvent insert;
};

/* ------------- 词法分析器 ------------- */
class Lexer {
	bool flag_ = false;		/* 读到分号结尾 */
	bool comma_ = false;		/* 逗号 */
	bool rght_ = false;		/* 右括号 */
	bool singleLineComment_ = false;
	bool multiLineComment_ = false;
	bool multiLineComment2_ = false;

public:
	Token next(bot::Reader& sr);

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

	/* M-1：CREATE/INSERT 的列段与参数段原为按元素互递归的下降链
	 * （nextTableColumnName↔nextTableColumnsIsEnd 等），递归深度 = 列数/参数数，
	 * 实测 3 万列结构脚本即栈溢出裸崩。改显式循环：token 消费顺序与错误
	 * 消息（Token <serialize()>）与旧实现逐一相同，仅执行模型变化。 */
	void nextCreateTable(CreateTableEvent& eve);
	void nextEnd();

	void nextInsertInto(InsertTableEvent& eve);

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