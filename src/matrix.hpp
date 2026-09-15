#pragma once
/* matrix.hpp —— 矩阵式解析器（移植 mt2：MatrixObject / MatrixParser / MatrixSaver）
 * @ 开块、; 行尾、# 注释、空格/制表符分隔项、- 占位符由上层解析
 */

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "util.hpp"

namespace mt {

class MatrixParseExcept : public std::runtime_error {
public:
	explicit MatrixParseExcept(const std::string& m) : std::runtime_error(m) {}
};

class BufOverTheLine : public MatrixParseExcept {
public:
	explicit BufOverTheLine(std::int64_t line)
		: MatrixParseExcept("Buffer length over the line. we got line: " + std::to_string(line)) {}
};

class DoesNotSync : public MatrixParseExcept {
public:
	explicit DoesNotSync(std::int64_t line)
		: MatrixParseExcept("Matrix Line Does Not Sync. we got line: " + std::to_string(line)) {}
};

class UnknowExcept : public MatrixParseExcept {
public:
	explicit UnknowExcept(std::int64_t line)
		: MatrixParseExcept("I don't understand what happened. we got line: " + std::to_string(line)) {}
};

/* ------------- 矩阵对象 ------------- */
class MatrixObject {
	std::string name_;
	std::string tmp_;				/* 字符缓冲区 */
	std::vector<std::string> grp_;			/* 整行缓冲区 */
	std::vector<std::vector<std::string>> values_;	/* 原始矩阵 */

public:
	bool hasName() const { return !name_.empty(); }
	const std::string& getName() const { return name_; }

	std::size_t rows() const { return values_.size(); }
	std::size_t cols() const {
		return rows() > 0 ? values_[0].size() : 0;
	}

	/* 取矩阵元素，只能取，不能修改 */
	const std::string& operator()(std::size_t row, std::size_t col) const {
		if (row >= values_.size() || col >= values_[row].size()) {
			throw bot::IllegalArgumentException(
				"Matrix access out of bounds: row=" + std::to_string(row) + " col=" + std::to_string(col));
		}
		return values_[row][col];
	}

	/* 在某一行的列头中寻找 key 的列号 */
	std::int64_t indexOfCol(std::size_t row, const std::string& key) const {
		if (row >= values_.size()) { return -1; }
		const auto& line = values_[row];
		for (std::size_t col = 0; col < line.size(); ++col) {
			if (line[col] == key) { return static_cast<std::int64_t>(col); }
		}
		return -1;
	}

	/* 浮点数写入缓冲区（INSERT 语句 Float 值，mt2 风格数值直写） */
	void submitFloat(double fp64);

	/* 储存对象 */
	void save(std::ostream& out) const {
		out << "@" << getName() << "\n";
		for (const auto& vec : values_) {
			for (const auto& item : vec) {
				out << item << " ";
			}
			out << ";\n";
		}
		out << "@\n";
	}

	void addRune(char c) { tmp_ += c; }

	void submitName() {
		if (tmp_.empty()) { return; }
		name_ = tmp_;
		tmp_.clear();
	}

	void submitItem() {
		if (tmp_.empty()) { return; }
		grp_.push_back(tmp_);
		tmp_.clear();
	}

	void submitLine(std::int64_t count) {
		if (grp_.empty()) { return; }
		if (!values_.empty() && grp_.size() != values_.back().size()) {
			throw DoesNotSync(count);
		}
		values_.push_back(grp_);
		grp_.clear();
	}
};

/* ------------- 矩阵式解析器 ------------- */
class MatrixParser {
	bot::Reader& input_;
	MatrixObject obj_;
	bool flag_ = false;	/* 结束标志 */
	bool nl_ = true;	/* new line */
	bool cm_ = false;	/* comment */
	bool nm_ = false;	/* name-line */

	std::int64_t next_(char c, std::int64_t count);

public:
	explicit MatrixParser(bot::Reader& input) : input_(input) {}
	void reset(bot::Reader& input) { input_ = input; }

	std::optional<MatrixObject> parse();
};

/* ------------- 矩阵式储存器 ------------- */
class MatrixSaver {
public:
	static void save(const std::vector<MatrixObject>& objects, std::ostream& out) {
		for (const auto& item : objects) {
			item.save(out);
			out << "\n";	/* 对象之间空一行 */
			out.flush();
		}
	}
};

} // namespace mt