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
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

#include "util.hpp"

namespace bot {

/* 命令行选项的类型化视图（main 经白名单校验后构造）。
 * tcpPort/tcpIdle 保留原始字符串：数值形态与范围校验在 openActionSource
 * 内完成（报错文案与退出路径不变），本结构只消灭 stringly-typed 键与查找。 */
struct CliOptions {
	std::optional<std::string> robot;
	bool help = false;
	std::string helpTopic;  /* 2026-09-22: --help <topic> 分段帮助 */
	bool tcp = false;
	std::string tcpPort;
	std::string tcpHost = "127.0.0.1";
	std::string tcpToken;
	std::string tcpIdle;
	/* --tcp-max-auth-attempts：口令连续失败上限（原写死 kTcpAuthMaxFails=5，
	 * 2026-09-22 F-1 参数化）。字符串保留原始输入，校验在 openActionSource 内
	 * 完成（与 tcpPort/tcpIdle 同惯例）；空串 = 用缺省 5。 */
	std::string tcpMaxAuthAttempts;
	/* --tcp-total-timeout：连接总时长预算下限（原写死 tcpTotalBudgetSec 里的
	 * 600，2026-09-22 F-3 参数化）。生效值仍取 max(该下限, idleSec*20)——
	 * 「总预算」本就是「至少这么多，且随 idle 放大」的语义，不是可能比
	 * idle*20 更小的硬上限。空串 = 用缺省 600。 */
	std::string tcpTotalTimeout;
	std::optional<std::string> input;

	/* --daemon-grace-ms：RobotStruct::deactivate 二段屏障的宽限上限（原写死
	 * kMaxGraceMs=30000，2026-09-22 F-2 参数化）。全局选项（不限 --agent/
	 * --tcp/--input 任一模式——任何声明了 @Daemon 的结构脚本都可能触发弃管
	 * 路径）。字符串保留原始输入，校验在 main.cpp 内完成（与 tcpPort 同惯例，
	 * 报错文案单点）；空串 = 用缺省 30000。 */
	std::string daemonGraceMs;

	/* ---- agent 模式（--agent）----
	 * agent 模式与动作输入源（--tcp / --input / stdin）互斥：前者让智能体
	 * 工具循环成为主流程，后者是「读一段 .mt 就跑完退出」的一次性流程。
	 * 数值型选项沿用 tcpPort/tcpIdle 的惯例保留原始字符串，统一在
	 * agent::buildAgentConfig 内校验（报错文案与退出路径单点）。 */
	bool agent = false;
	bool agentOptionsUsed = false;	/* 出现过 agent 专属选项（非 agent 场景据此报错） */
	std::string agentTask;		/* --task：非空 = 一次性任务，空 = 交互 REPL */
	/* 安全缺省（2026-09-20 评估 C12 收口）：run_command 涉及任意 shell 执行，
	 * 故**缺省不提供**，且只能由命令行 --agent-shell 开启；配置文件的
	 * allow_shell **只能关不能开**（写 true 直接拒绝加载并指名原因，见
	 * agentmode.cpp 的 buildAgentConfig 第 7 步）
	 * （旧行为是缺省提供 + --agent-no-shell 关闭，安全侧默认值反了）。 */
	bool agentShell = false;	/* --agent-shell：显式开启 run_command */
	std::string llmEndpoint;
	std::string llmModel;
	std::string llmConfig;		/* --llm-config：配置文件路径（缺省自动查找） */
	std::string llmDriver;		/* --llm-driver：协议驱动表名（= modules/<名>.lua） */
	std::string llmVar;		/* --llm-var：承载驱动变量的名字 */
	std::string llmMaxTurns;
	std::string llmMaxTokens;
	std::string llmTimeout;
	/* --llm-task-timeout：单个任务的墙钟预算（秒，A4）。与 llmTimeout 不同维度：
	 * 那条管一次模型调用，这条管"一个任务能让硬件带电多久"。0 = 不限。
	 * 同 llmTimeout 一样只存原始串，校验在 buildAgentConfig（配置错误不碰真机）。 */
	std::string llmTaskTimeout;
	std::string scriptsDir;
	/* --prompt-file：提示词包输入，可重复（每项为文件或目录）。加载与拼接
	 * 在 agent::buildAgentConfig 完成（配置错误不碰真机），本结构只收集。 */
	std::vector<std::string> promptFiles;
	/* ---- 摄像头（capture_image） ---- */
	std::string cameraVar;		/* --camera-var：承载 modules/Camera.lua 的变量名 */
	std::string cameraDevice;	/* --camera-device：重设设备路径 */
	std::string cameraSrc;		/* --camera-src：模拟图片来源文件 */
	/* --camera-src-dir：capture_image 模拟图片的**白名单根目录**（H1）。
	 * 命令行独占：它决定"能读哪个目录"，等价于放宽读权限，故配置文件不能给
	 * （写了直接拒绝加载，与 --agent-shell 同判据）。不给时：命令行的 --camera-src
	 * 用其所在目录当根，配置文件来源的 camera_src 则不推导（驱动侧默认拒绝）。 */
	std::string cameraSrcDir;
	bool noCamera = false;		/* --no-camera：不提供拍照工具 */
	/* --audit-log：审计轨迹落点（JSON Lines 追加；"-" = stderr）。
	 * 缺省空 = 不记录（保持既有语义）。打开失败的报错与退出在
	 * agent::buildAgentConfig（配置错误不碰真机）。 */
	std::string auditLog;
	/* --agent-policy：策略钩子文件（严格 JSON；空 = 不启用，全部放行）。
	 * 加载与校验失败在 agent::buildAgentConfig 报错退出（配置错误不碰真机）。 */
	std::string agentPolicy;
	/* --check-config（PL-1，2026-09-22）：只校验配置并打印摘要，不装载结构脚本、
	 * 不上电、不联网。配置校验本身仍由 buildAgentConfig 单点负责（这里不另起
	 * 一套判据），故 main 里只是「打印摘要后 return 0」一个分支。属 agent 专属
	 * 选项：置 agentOptionsUsed，非 agent 模式由既有逻辑拒绝；未配 --robot 同样
	 * 被「agent 模式需要 --robot」那条拒绝（不会静默打印帮助退出 0）。 */
	bool checkConfig = false;
};

/* 脚本单次输入的字节上限：动作脚本（.mt）与结构脚本（.sql）统一适用。
 * run()/readSQL() 会把输入整体驻留后再装配（@Alias 必须先于 @Action 生效、
 * 矩阵一次性建表），不设上限即等于「输入多大、进程就吃多大内存」；
 * 实测 100MB 动作输入 → RSS 4.2GB。真实脚本为 KB 级，8MiB 已留三个数量级余量。 */
inline constexpr std::size_t kMaxScriptBytes = 8u << 20;

/* 脚本字节数超限。kind 指明输入类别（"action script" / "structure script"）。
 * 重构计划 2.5：直接继承单一根异常 bot::IllegalArgumentException——此前
 * 挂 mt::MatrixParseExcept 让纯 I/O 层倒挂依赖解析层。原文保留性质不变：
 * 抛出点（整段读取 / LimitedReader::read）都在 MatrixParser::parse() 的
 * try 之外，不经其「带行号包装」路径；即便未来 read() 移进 try，IAE 分支
 * 也是包装保留原文而非降级。 */
class ScriptTooLarge : public bot::IllegalArgumentException {
public:
	explicit ScriptTooLarge(std::size_t cap, const std::string& kind = "action script")
		: IllegalArgumentException(kind + " exceeds the byte limit of " + std::to_string(cap)
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

/* ---- TCP 口令校验三态（2026-09-20 重构阶段 4.3）----
 * 必须区分「对端给了凭据但不匹配」与「对端什么都没给」：
 *   Ok           口令正确
 *   Rejected     收到凭据行但内容不匹配 / 超长 —— 计入爆破计数
 *   NoCredential 未收到凭据行（空闲超时 EAGAIN、EOF、EINTR）—— **不得**计为鉴权失败，
 *                否则任意未认证对端连上不发字节、重复 5 次即可终止进程
 *                （旧实现把 EAGAIN 与不匹配混为一谈，且误报「连续 5 次校验失败」）。
 * 暴露为公开函数便于白盒单测（与 ActionRunner::resolveActionDevice 同惯例）。 */
enum class TcpAuthStatus { Ok, Rejected, NoCredential };

TcpAuthStatus checkTcpToken(int client, const std::string& token);

/* 打开动作输入源：tcp 优先 > 文件 > 标准输入（MemoryReader 整读）。
 * 返回的读取器一律带字节上限；TCP 通道另含绑定地址、空闲超时与可选口令。 */
std::unique_ptr<Reader> openActionSource(const CliOptions& options);

} // namespace bot