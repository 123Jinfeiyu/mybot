#pragma once
/* agent_subprocess.hpp —— run_command 的子进程实现（拆自 agenttools.cpp）
 * 超时 / 进程组终止 / 输出截断 / 环境清洗，与五个工具的装配逻辑无耦合，
 * 独立成 TU 便于单测与复用。 */

#include <cstddef>
#include <string>

namespace bot {
namespace agent {

/* run_command 单流输出上限（字节）；formatRunResult 的截断文案取自该值 */
constexpr std::size_t kMaxCommandOutput = 64u * 1024;

struct RunResult {
	int exitCode = -1;
	bool timedOut = false;
	bool interrupted = false;
	/* 截断按流独立记账：kMaxCommandOutput 是「每流」上限（总量可达 2 倍），
	 * 原实现用单一 truncated 标志，stdout 触顶会连带把 stderr 也报成截断。 */
	bool outTruncated = false;
	bool errTruncated = false;
	std::string out;
	std::string err;
};

/* 经 /bin/sh -c 执行命令；stopRequested()（util）提前触发整组终止。
 * pipe / fork 失败抛 std::runtime_error。 */
RunResult runShell(const std::string& cmd, int timeoutSec);

/* RunResult → 面向模型的文本回执（退出码/超时/中断/输出与截断标记） */
std::string formatRunResult(const RunResult& r);

} // namespace agent
} // namespace bot
