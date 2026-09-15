#pragma once
/* actionsource.hpp —— 动作输入源（移植 bot4cj/src/ActionSource.cj）
 * 通道优先级：--tcp > --input > 标准输入 stdin。
 */

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "util.hpp"

namespace bot {

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

/* 已连接 TCP 客户端（recv 逐字节；EOF=0/错误=-1） */
class TcpStreamReader : public Reader {
	int fd_;

public:
	explicit TcpStreamReader(int fd) : fd_(fd) {}
	~TcpStreamReader() override { if (fd_ >= 0) { ::close(fd_); } }

	int read() override {
		if (fd_ < 0) { return -1; }
		char c;
		ssize_t n = ::recv(fd_, &c, 1, 0);
		if (n == 1) { return static_cast<unsigned char>(c); }
		return -1;
	}
};

/* 打开动作输入源：tcp 优先 > 文件 > 标准输入（MemoryReader 整读） */
std::unique_ptr<Reader> openActionSource(const std::map<std::string, std::string>& options);

} // namespace bot