#pragma once
/* fixtures.hpp —— 单测共用的最小构造助手
 *
 * 只放「把字符串变成被测对象」这一类样板，不放断言逻辑；
 * 每个 helper 都直接走生产代码路径（MatrixParser / Lexer / RobotStruct 构造函数），
 * 不自己拼装内部状态，这样测试与实现的耦合点保持在公开接口上。
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hardware.hpp"
#include "matrix.hpp"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "sqlparse.hpp"
#include "util.hpp"

namespace fx {

/* 测试用 lambda 硬件变量：play 委托给注入的函数对象（基类已是纯虚接口，
 * 测试不能再直接 new MatrixVars_Hardware(lambda)） */
class LambdaHw : public bot::MatrixVars_Hardware {
	std::function<bot::Json(const bot::Json&)> fn_;
public:
	explicit LambdaHw(std::function<bot::Json(const bot::Json&)> fn) : fn_(std::move(fn)) {}
	bot::Json play(const bot::Json& in) override { return fn_(in); }
	const char* kindName() const override { return "test-lambda"; }
};

/* 解析出恰好一个矩阵对象；解析失败或多解都视为用例前置条件不成立 */
inline mt::MatrixObject oneMatrix(const std::string& text) {
	bot::MemoryReader reader(text);
	mt::MatrixParser parser(reader);
	auto mat = parser.parse();
	if (!mat) { throw std::runtime_error("前置条件失败：parse() 未产出矩阵（输入格式有误）"); }
	if (parser.parse()) {
		throw std::runtime_error("前置条件失败：输入含多个矩阵，请改用 manyMatrices()");
	}
	return std::move(*mat);
}

/* 按声明序解析出全部矩阵对象（等价 RobotStruct::readMatrix 的公开等价物） */
inline std::vector<mt::MatrixObject> manyMatrices(const std::string& text) {
	bot::MemoryReader reader(text);
	mt::MatrixParser parser(reader);
	std::vector<mt::MatrixObject> out;
	while (auto mat = parser.parse()) { out.push_back(std::move(*mat)); }
	return out;
}

/* 结构脚本词元序列（Lexer::parseAll 是 readSQL 的前半段，本身即被测面） */
inline std::vector<sql4cj::Token> tokens(const std::string& text) {
	bot::MemoryReader reader(text);
	return sql4cj::Lexer::parseAll(reader);
}

/* 词元类型序列的可读指纹，便于一次性比对整条 token 流 */
inline std::string fingerprint(const std::vector<sql4cj::Token>& ts) {
	std::string out;
	for (const auto& t : ts) {
		if (!out.empty()) { out += ' '; }
		out += t.serialize();
	}
	return out;
}

/* 单元格取值的窄化助手
 * MatrixObject::operator() 同时提供 (size_t,size_t) 与 (int64_t,int64_t) 两个重载，
 * 传字面量 int 会命中「两条同为整型转换」的二义性；测试里统一走这两个
 * 显式 int64_t 形参的包装，顺带让「负下标防线」在调用点读起来就是负数。 */
inline const std::string& cell(const mt::MatrixObject& m, std::int64_t row, std::int64_t col) {
	return m(row, col);
}

/* 同上，走 "-" 前向填充视图 */
inline const std::string& inh(const mt::MatrixObject& m, std::int64_t row, std::int64_t col) {
	return m.inherited(row, col);
}


/* ---- 共享测试基建（阶段 4：自 test_llmconfig/test_agenttools 上收）---- */

/* 临时目录：构造即建，析构即清（getpid 隔离并行进程） */
class TempDir {
	std::filesystem::path dir_;

public:
	explicit TempDir(const std::string& tag) {
		dir_ = std::filesystem::temp_directory_path()
			/ ("mybot_test_" + tag + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
		std::filesystem::create_directories(dir_);
	}
	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
	const std::filesystem::path& path() const { return dir_; }
};

/* 环境变量守卫：析构恢复原值，用例之间不得互相污染 */
class EnvVar {
	std::string name_;
	std::string old_;
	bool had_ = false;

public:
	EnvVar(const std::string& name, const char* value) : name_(name) {
		const char* v = std::getenv(name.c_str());
		had_ = v != nullptr;
		if (had_) { old_ = v; }
		::setenv(name.c_str(), value, 1);
	}
	~EnvVar() {
		if (had_) { ::setenv(name_.c_str(), old_.c_str(), 1); }
		else { ::unsetenv(name_.c_str()); }
	}
	EnvVar(const EnvVar&) = delete;
	EnvVar& operator=(const EnvVar&) = delete;
};

} // namespace fx
