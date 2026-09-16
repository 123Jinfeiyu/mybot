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

class DoesNotSync : public MatrixParseExcept {
public:
	DoesNotSync(std::int64_t line, std::size_t expected, std::size_t got)
		: MatrixParseExcept("Matrix Line Does Not Sync. we got line: " + std::to_string(line)
			+ ": this row has " + std::to_string(got) + " cell(s), but the row above has "
			+ std::to_string(expected) + " (every row must match the header column count)") {}
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

	/* "-" 占位符继承的解析结果，惰性前向填充（见 inherited()）。
	 * 按值存储而非存指针：MatrixObject 会被拷贝（findMt 按值返回），
	 * 指向 values_ 内部字符串的指针在副本里会悬垂。 */
	mutable std::vector<std::string> inherited_;
	mutable bool inheritedReady_ = false;

	void buildInherited() const {
		inherited_.clear();
		inheritedReady_ = false;
		const std::size_t nrow = values_.size();
		const std::size_t ncol = cols();
		inherited_.reserve(nrow * ncol);
		for (std::size_t r = 0; r < nrow; ++r) {
			for (std::size_t c = 0; c < ncol; ++c) {
				/* 语义与旧的逐格递归完全一致：row<=1 的 "-" 不继承，
				 * 原样保留字面值，由调用方决定含义（getMt 返 "-"、parseMtDouble 返 0.0）。 */
				if (r > 1 && values_[r][c] == "-") {
					inherited_.push_back(inherited_[(r - 1) * ncol + c]);
				} else {
					inherited_.push_back(values_[r][c]);
				}
			}
		}
		inheritedReady_ = true;
	}

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

	/* C5 防线：int64 下标判负——indexOfCol 未命中返回 -1，若隐式转 size_t
	 * 会回绕成 18446744073709551615，报错毫无指向性（缺 name 列的典型路径）。 */
	const std::string& operator()(std::int64_t row, std::int64_t col) const {
		if (row < 0 || col < 0) {
			throw bot::IllegalArgumentException(
				"Matrix access with negative index: row=" + std::to_string(row) + " col=" + std::to_string(col)
				+ " (a header column such as 'name' is likely missing)");
		}
		return (*this)(static_cast<std::size_t>(row), static_cast<std::size_t>(col));
	}

	/* 取 "-" 占位符解析后的值（首次访问触发一次前向填充，之后 O(1)）。
	 * 取代旧的逐格递归回溯：那既是 O(n²)，递归深度又随行数增长。 */
	const std::string& inherited(std::size_t row, std::size_t col) const {
		if (!inheritedReady_) { buildInherited(); }
		const std::size_t ncol = cols();
		if (row >= values_.size() || col >= ncol) {
			throw bot::IllegalArgumentException(
				"Matrix access out of bounds: row=" + std::to_string(row) + " col=" + std::to_string(col));
		}
		return inherited_[row * ncol + col];
	}

	const std::string& inherited(std::int64_t row, std::int64_t col) const {
		if (row < 0 || col < 0) {
			throw bot::IllegalArgumentException(
				"Matrix access with negative index: row=" + std::to_string(row) + " col=" + std::to_string(col)
				+ " (a header column such as 'name' is likely missing)");
		}
		return inherited(static_cast<std::size_t>(row), static_cast<std::size_t>(col));
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
			throw DoesNotSync(count, values_.back().size(), grp_.size());
		}
		values_.push_back(grp_);
		grp_.clear();
		inheritedReady_ = false;	/* 行数变了，继承缓存作废 */
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