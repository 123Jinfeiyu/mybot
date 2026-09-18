#pragma once
/* agentmode.hpp —— agent 模式的配置组装与会话外壳
 *
 * 「主要执行流程」的落点：main 在 --agent 下先 active() 机器人，再把控制权
 * 交给本模块的会话循环（一次性任务或交互 REPL），全程硬件常驻；只有会话结束
 * 才 deactivate()。这与管道模式（读一段 .mt → 跑完 → 断电退出）是两条路径。
 */

#include <iosfwd>
#include <string>

#include "actionsource.hpp"
#include "agentproto.hpp"

namespace bot {

class RobotStruct;

namespace agent {

struct AgentModeConfig {
	std::string endpoint;
	std::string model;
	std::string apiKey;
	int maxTurns = 8;
	int maxTokens = 4096;
	double temperature = 0.7;
	double topP = 0.9;
	long timeoutSec = 180;		/* 单次模型调用超时（秒） */
	std::string scriptsDir = "scripts";
	std::string structurePath;
	std::string task;		/* 非空 = 一次性执行；空 = 交互 REPL */
	bool allowShell = true;		/* false = 不注册 run_command */
	bool verbose = true;
};

/* 环境变量取 API key：BOT4CJ_LLM_KEY → ROBOT_AGENT_API_KEY → DEEPSEEK_API_KEY */
std::string resolveLlmKeyFromEnv();

/* 由 CLI 选项组装配置。失败返回 false 并把「人话报错」写进 err。 */
bool buildAgentConfig(const CliOptions& opts, AgentModeConfig& out, std::string& err);

/* 系统提示词：机器人结构与硬件清单 + 可用动作脚本 + 工作方式与安全约束 */
std::string buildAgentSystemPrompt(const AgentModeConfig& cfg, RobotStruct& robot);

/* 跑 agent 会话。返回进程退出码：0 正常结束；1 模型/执行失败。 */
int runAgentMode(const AgentModeConfig& cfg, RobotStruct& robot, ChatTransport& transport,
		std::istream& in, std::ostream& out, std::ostream& log);

} // namespace agent
} // namespace bot
