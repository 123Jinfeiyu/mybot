#pragma once
/* actionsource.hpp —— 动作输入源（移植 bot4cj/src/ActionSource.cj）
 * 通道优先级：--tcp > --input > 标准输入 stdin。
 */

#include <cstddef>
#include <cstdio>
#include <chrono>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "matrix.hpp"
#include "util.hpp"

namespace bot {

/* 命令行选项的类型化视图（main 经白名单校验后构造）。
 * tcpPort/tcpIdle 保留原始字符串：数值形态与范围校验在 openActionSource
 * 内完成（报错文案与退出路径不变），本结构只消灭 stringly-typed 键与查找。 */
struct CliOptions {
	std::optional<std::string> robot;
	bool help = false;
	bool tcp = false;
	std::string tcpPort;
	std::string tcpHost = "127.0.0.1";
	std::string tcpToken;
	std::string tcpIdle;
	std::optional<std::string> input;

	/* ---- agent 模式（--agent）----
	 * agent 模式与动作输入源（--tcp / --input / stdin）互斥：前者让智能体
	 * 工具循环成为主流程，后者是「读一段 .mt 就跑完退出」的一次性流程。
	 * 数值型选项沿用 tcpPort/tcpIdle 的惯例保留原始字符串，统一在
	 * agent::buildAgentConfig 内校验（报错文案与退出路径单点）。 */
	bool agent = false;
	bool agentOptionsUsed = false;	/* 出现过 agent 专属选项（非 agent 场景据此报错） */
	std::string agentTask;		/* --task：非空 = 一次性任务，空 = 交互 REPL */
	bool agentNoShell = false;	/* --agent-no-shell：不注册 run_command */
	std::string llmEndpoint;
	std::string llmModel;
	std::string llmKey;		/* 取出后立刻 setenv，不落进驱动参数表/日志 */
	std::string llmConfig;		/* --llm-config：配置文件路径（缺省自动查找） */
	std::string llmDriver;		/* --llm-driver：协议驱动表名（= modules/<名>.lua） */
	std::string llmVar;		/* --llm-var：承载驱动变量的名字 */
	std::string llmMaxTurns;
	std::string llmMaxTokens;
	std::string llmTimeout;
	std::string scriptsDir;
	/* ---- 摄像头（capture_image） ---- */
	std::string cameraVar;		/* --camera-var：承载 modules/Camera.lua 的变量名 */
	std::string cameraDevice;	/* --camera-device：重设设备路径 */
	std::string cameraSrc;		/* --camera-src：模拟图片来源文件 */
	bool noCamera = false;		/* --no-camera：不提供拍照工具 */
};

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

/* 整段受限读取：分块累计，超过 cap 立即抛 ScriptTooLarge（inline：mtmerge
 * 目标不链接本 .cpp，只复用头内定义——与 kMaxScriptBytes/ScriptTooLarge
 * 同一惯例）。mtmerge 文件输入与动作 stdin 通道共用；此前两份手写循环
 * 且超限处理不一致（mtmerge 抛、stdin 打印后放弃）——同一上限不该有
 * 两种用户可见行为。先整读再判上限对超大输入形同虚设（限流之前就把
 * 全部字节吃进内存），故块读过程中即时拒绝。 */
inline std::string readAllLimited(std::istream& in, std::size_t cap,
		const std::string& kind = "action script") {
	std::string data;
	char buf[65536];
	for (;;) {
		in.read(buf, static_cast<std::streamsize>(sizeof buf));
		std::streamsize got = in.gcount();
		if (got <= 0) { break; }
		data.append(buf, static_cast<std::size_t>(got));
		if (data.size() > cap) { throw ScriptTooLarge(cap, kind); }
	}
	return data;
}

/* 以 EOF(-1) 表示输入结束的字符流 */
class FileReader : public Reader {
	std::FILE* file_;

public:
	explicit FileReader(const std::string& path) : file_(std::fopen(path.c_str(), "rb")) {}
	~FileReader() override { if (file_) { std::fclose(file_); } }

	/* 持 FILE* 句柄：拷贝会造成双重 fclose（F-25） */
	FileReader(const FileReader&) = delete;
	FileReader& operator=(const FileReader&) = delete;

	bool good() const { return file_ != nullptr; }

	int read() override {
		if (!file_) { return -1; }
		int c = std::fgetc(file_);
		/* R3：区分 EOF 与读错误——磁盘 I/O 错误静默当作脚本结束会截断动作 */
		if (c == EOF && std::ferror(file_)) {
			std::cerr << "警告：动作脚本读取发生 I/O 错误，按 EOF 处理（脚本可能被截断）" << std::endl;
		}
		return c;
	}
};

/* 已连接 TCP 客户端（内部 64KiB 缓冲，一次 recv 填满后逐字节出队，审计 M2；
 * EOF=0/错误=-1）
 * 空闲超时：对端连上后不再发字节时不能永久挂死进程（秒级由调用方给定）。
 * 总时长预算（C-M2）：空闲超时按「每字节」重置——慢滴对端每 <idle 秒发
 * 1 字节即可无限期挂住进程。从连接建立时刻起算 wall-clock 上限，超限按
 * 输入结束处理（脚本按截断语义走后续报错路径）。 */
class TcpStreamReader : public Reader {
	int fd_;
	int idleSec_;
	std::chrono::steady_clock::time_point deadline_;
	bool timedOut_ = false;
	bool overBudget_ = false;
	std::string buf_;
	std::size_t pos_ = 0;
	static constexpr std::size_t kBufCap = 64u << 10;

public:
	explicit TcpStreamReader(int fd, int idleSec,
			std::chrono::steady_clock::time_point deadline)
		: fd_(fd), idleSec_(idleSec), deadline_(deadline) {}
	~TcpStreamReader() override { if (fd_ >= 0) { ::close(fd_); } }

	/* 持 socket fd：拷贝会造成双重 close（F-25） */
	TcpStreamReader(const TcpStreamReader&) = delete;
	TcpStreamReader& operator=(const TcpStreamReader&) = delete;

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
std::unique_ptr<Reader> openActionSource(const CliOptions& options);

} // namespace bot