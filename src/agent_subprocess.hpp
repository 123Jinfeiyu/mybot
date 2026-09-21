#pragma once
/* agent_subprocess.hpp —— run_command 的子进程实现（拆自 agenttools.cpp）
 * 超时 / 进程组终止 / 输出截断 / 环境清洗，与五个工具的装配逻辑无耦合，
 * 独立成 TU 便于单测与复用。 */

#include <cstddef>
#include <string>
#include <vector>

namespace bot {
namespace agent {

/* run_command 单流输出上限（字节）；formatRunResult 的截断文案取自该值 */
constexpr std::size_t kMaxCommandOutput = 64u * 1024;

/* 抓取当前环境里敏感变量的**原始字符串指针**。必须在 setenv 之前调用：
 * setenv(overwrite=1) 会把数组槽位换成堆上的新串，exec 时那份原串就再也找不回。 */
std::vector<char*> collectEnvSecrets();

/* 就地把这些字符串抹零。动机：/proc/<pid>/environ 对同 UID 进程可读，
 * 只清洗子进程继承的那份环境挡不住它——常驻 agent 的密钥会一直躺在那儿。
 * 残余：同 UID 若有 ptrace 权限，仍可从宿主内存读出密钥；本函数是提高门槛
 * 而非对抗边界，要硬隔离得把 agent 放进专用用户 / 容器 / 网络命名空间。 */
void scrubEnvSecrets(const std::vector<char*>& pointers);

struct RunResult {
	int exitCode = -1;
	bool timedOut = false;
	bool interrupted = false;
	/* 硬上限兜底触发（超时后 kill 仍未能让管道 EOF，例如子进程卡在 D 态）：
	 * 此时**不确定**子进程是否已死，也不能声称「已终止」——那句话会直接进模型
	 * 上下文，说错了模型就基于错误前提继续决策。旧实现走这条路径时一个标记都
	 * 不带，回执看起来像「正常结束但退出码 -1」，真实成因完全丢失。 */
	bool hardStopped = false;
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
