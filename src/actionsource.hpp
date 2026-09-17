#pragma once
/* actionsource.hpp —— 动作输入源（移植 bot4cj/src/ActionSource.cj）
 * 通道优先级：--tcp > --input > 标准输入 stdin。
 */

#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "matrix.hpp"
#include "util.hpp"

namespace bot {

/* 脚本单次输入的字节上限：动作脚本（.mt）与结构脚本（.sql）统一适用。
 * run()/readSQL() 会把输入整体驻留后再装配（@Alias 必须先于 @Action 生效、
 * 矩阵一次性建表），不设上限即等于「输入多大、进程就吃多大内存」；
 * 实测 100MB 动作输入 → RSS 4.2GB。真实脚本为 KB 级，8MiB 已留三个数量级余量。 */
inline constexpr std::size_t kMaxScriptBytes = 8u << 20;

/* 脚本字节数超限。kind 指明输入类别（"action script" / "structure script"）。
 * 归入 MatrixParseExcept 家族：MatrixParser::parse() 对该族是原样 rethrow
 * （读取点 input_.read() 本就在其 try 之外，两种路径都不会把报错降级成
 * 无信息的 "Parse failed at line N"），调用方按脚本解析期错误统一处理即可
 * 拿到原文。 */
class ScriptTooLarge : public mt::MatrixParseExcept {
public:
	explicit ScriptTooLarge(std::size_t cap, const std::string& kind = "action script")
		: MatrixParseExcept(kind + " exceeds the byte limit of " + std::to_string(cap)
			+ ". Real bot4cj scripts are KB-sized; a larger input almost certainly means a runaway"
			+ " or malicious producer (the whole script is buffered before playback).") {}
};

/* 以 EOF(-1) 表示输入结束的字符流 */
class FileReader : public Reader {
	std::FILE* file_;

public:
	explicit FileReader(const std::string& path) : file_(std::fopen(path.c_str(), "rb")) {}
	~FileReader() override { if (file_) { std::fclose(file_); } }
	bool good() const { return file_ != nullptr; }

	int read() override {
		if (!file_) { return -1; }
		int c = std::fgetc(file_);
		/* R3：区分 EOF 与读错误——磁盘 I/O 错误静默当作脚本结束会截断动作 */
		if (c == EOF && std::ferror(file_)) {
			std::cout << "警告：动作脚本读取发生 I/O 错误，按 EOF 处理（脚本可能被截断）" << std::endl;
		}
		return c;
	}
};

/* 已连接 TCP 客户端（recv 逐字节；EOF=0/错误=-1）
 * 空闲超时：对端连上后不再发字节时不能永久挂死进程（秒级由调用方给定）。 */
class TcpStreamReader : public Reader {
	int fd_;
	int idleSec_;
	bool timedOut_ = false;

public:
	explicit TcpStreamReader(int fd, int idleSec) : fd_(fd), idleSec_(idleSec) {}
	~TcpStreamReader() override { if (fd_ >= 0) { ::close(fd_); } }

	int read() override;
};

/* 字节数上限包装器：所有脚本输入通道统一限流（见 openActionSource 与 main.cpp） */
class LimitedReader : public Reader {
	std::unique_ptr<Reader> inner_;
	std::size_t cap_;
	std::string kind_;
	std::size_t seen_ = 0;

public:
	LimitedReader(std::unique_ptr<Reader> inner, std::size_t cap,
			std::string kind = "action script")
		: inner_(std::move(inner)), cap_(cap), kind_(std::move(kind)) {}

	int read() override {
		int c = inner_->read();
		if (c >= 0 && ++seen_ > cap_) { throw ScriptTooLarge(cap_, kind_); }
		return c;
	}
};

/* 打开动作输入源：tcp 优先 > 文件 > 标准输入（MemoryReader 整读）。
 * 返回的读取器一律带字节上限；TCP 通道另含绑定地址、空闲超时与可选口令。 */
std::unique_ptr<Reader> openActionSource(const std::map<std::string, std::string>& options);

} // namespace bot