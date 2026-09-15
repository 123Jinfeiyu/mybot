#pragma once
/* sqlparse.hpp —— 结构脚本 SQL 解析器（移植 SQL4cj：Lexer / Parser / Event） */

#include <cstddef>
#include <string>
#include <vector>

#include "util.hpp"

namespace sql4cj {

/* ------------- Token ------------- */
enum class TokenType {
	SELECT, FROM, CREATE, TABLE, STRING,
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
enum class VarType { String_ = 0, Integer };

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
	bool left_ = false;		/* 左括号 */
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

	/* 消费下一个 token 或抛错 */
	const Token& expect() {
		if (idx_ >= tokenSerial_.size()) {
			throw bot::IllegalArgumentException("Token <unexpected end of script>");
		}
		return tokenSerial_[idx_++];
	}

	void nextCreateTable(CreateTableEvent& eve);
	void nextTableColumns(CreateTableEvent& eve);
	void nextTableColumnName(CreateTableEvent& eve);
	void nextTableColumnType(CreateTableEvent& eve, const std::string& colName);
	void nextTableColumnsIsEnd(CreateTableEvent& eve);
	void nextEnd();

	void nextInsertInto(InsertTableEvent& eve);
	void nextInsertParams(InsertTableEvent& eve);
	void nextInsertParamName(InsertTableEvent& eve, std::vector<std::string>& paramNames);
	void nextInsertParamNamesIsEnd(InsertTableEvent& eve, std::vector<std::string>& paramNames);
	void nextInsertParamValues(InsertTableEvent& eve, const std::vector<std::string>& paramNames);
	void nextInsertParamValuesStart(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, std::vector<std::string>& paramValues);
	void nextInsertParamValue(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, std::vector<std::string>& paramValues);
	void nextInsertParamValuesIsEnd(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, std::vector<std::string>& paramValues);
	void mergeAndAddInsertParamsToEve(InsertTableEvent& eve,
		const std::vector<std::string>& paramNames, const std::vector<std::string>& paramValues);

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