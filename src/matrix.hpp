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
	friend class MatrixBuilder;

	std::string name_;
	std::vector<std::vector<std::string>> values_;	/* 原始矩阵 */

	/* "-" 占位符继承的解析结果，惰性前向填充（见 inherited()）。
	 * 按值存储而非存指针：MatrixObject 是可拷贝值类型（parse/take 均按值
	 * 交付），指向 values_ 内部字符串的指针在副本里会悬垂。 */
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
				 * 原样保留字面值，由调用方决定含义（inherited 返 "-"、parseMtDouble 返 0.0；
				 * 动作派发另把「首数据行即 '-'」判为未起拍、不投递该参数，见 robot.cpp）。 */
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

	/* 解析期追加一行（R-4）：宽度一致性校验 + 继承缓存作废。
	 * 头行与 INSERT 行都经此入口，保证校验只此一处。 */
	void appendRow(std::vector<std::string> row, std::int64_t count) {
		if (!values_.empty() && row.size() != values_.back().size()) {
			throw DoesNotSync(count, values_.back().size(), row.size());
		}
		values_.push_back(std::move(row));
		inheritedReady_ = false;	/* 行数变了，继承缓存作废 */
	}
};

/* ------------- 解析期增量构建器（R-4）-------------
 * 字符缓冲 + 整行缓冲是解析器状态机的专属物，此前挂在 MatrixObject
 * 上让它同时是「构建器」与「只读值」；拆开后 MatrixObject 只有
 * appendRow 一个收窄的写入口（SQL INSERT 追加行用）。 */
class MatrixBuilder {
	std::string tmp_;				/* 字符缓冲区 */
	std::vector<std::string> grp_;			/* 整行缓冲区 */
	MatrixObject obj_;

public:
	bool hasName() const { return obj_.hasName(); }

	void addRune(char c) { tmp_ += c; }

	void submitName() {
		if (tmp_.empty()) { return; }
		obj_.name_ = tmp_;
		tmp_.clear();
	}

	void submitItem() {
		if (tmp_.empty()) { return; }
		grp_.push_back(tmp_);
		tmp_.clear();
	}

	void submitLine(std::int64_t count) {
		if (grp_.empty()) { return; }
		obj_.appendRow(std::move(grp_), count);
		grp_.clear();
	}

	/* 整串直灌入口（SQL 事件路径：token 本就完整，无需逐字符） */
	void name(const std::string& n) { obj_.name_ = n; }
	void item(const std::string& s) { grp_.push_back(s); }
	void line(std::int64_t count) { submitLine(count); }

	MatrixObject take() { return std::move(obj_); }
};

/* ------------- 矩阵式解析器 ------------- */
class MatrixParser {
	bot::Reader& input_;
	MatrixBuilder builder_;
	bool flag_ = false;	/* 结束标志 */
	bool nl_ = true;	/* new line */
	bool cm_ = false;	/* comment */
	bool nm_ = false;	/* name-line */

	std::int64_t next_(char c, std::int64_t count);

public:
	explicit MatrixParser(bot::Reader& input) : input_(input) {}

	std::optional<MatrixObject> parse();
};

/* ------------- 共享查询与切分（N-10：单一事实源）-------------
 * 此前「按名查矩阵」在 ActionRunner/mtmerge/readSQL 各有一份、「列头切分」
 * 在 ActionRunner/mtmerge 各有一份，规则靠注释人工同步——mtmerge 产物一旦
 * 与运行端解释漂移，合成的脚本会被以不同方式解读。 */

/* 按矩阵名查找（trim 后比较）；不存在返回 nullptr */
inline const MatrixObject* findByName(const std::vector<MatrixObject>& ll,
		const std::string& name) {
	for (const auto& m : ll) {
		if (bot::trim(m.getName()) == name) { return &m; }
	}
	return nullptr;
}

inline MatrixObject* findByName(std::vector<MatrixObject>& ll, const std::string& name) {
	for (auto& m : ll) {
		if (bot::trim(m.getName()) == name) { return &m; }
	}
	return nullptr;
}

/* @Action 列头解析：<device>.<param>（恰好一个点）。ActionRunner 派发与
 * mtmerge 相位归并共用同一切分规则与报错文案。 */
inline std::pair<std::string, std::string> splitActionColumn(const std::string& name_HV) {
	std::size_t dot = name_HV.find('.');
	if (dot == std::string::npos || name_HV.find('.', dot + 1) != std::string::npos) {
		throw bot::IllegalArgumentException("Action column '" + name_HV
			+ "' is not valid (expected format: <device>.<param>)");
	}
	return { name_HV.substr(0, dot), name_HV.substr(dot + 1) };
}

/* 读取矩阵式外表（@Action/@Alias/@Vars…）到 out：自省结局，不破坏读缓存。
 * 原 ScriptLoader::readMatrix——mtmerge 只需这一层，放回 matrix 后
 * mtmerge 不再链接 robot/LuaJIT（N-11）。 */
void readMatrix(bot::Reader& file, std::vector<MatrixObject>& out);

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