#pragma once
/* agentmode.hpp —— agent 模式的配置组装与会话外壳
 *
 * 「主要执行流程」的落点：main 在 --agent 下先 active() 机器人，再把控制权
 * 交给本模块的会话循环（一次性任务或交互 REPL），全程硬件常驻；只有会话结束
 * 才 deactivate()。这与管道模式（读一段 .mt → 跑完 → 断电退出）是两条路径。
 *
 * 模型调用不在这里，也不在宿主的任何 C++ 代码里：它是一颗 Lua 协议驱动
 * （table 名 = 驱动名，见 --llm-driver），由 agentdriver 适配成 ChatTransport。
 */

#include <iosfwd>
#include <string>
#include <vector>

#include "actionsource.hpp"
#include "agentdriver.hpp"
#include "agentproto.hpp"

namespace bot {

class RobotStruct;

namespace agent {

/* API key 的来源。用于「密钥是否对 agent 自己的 shell 可见」的启动告警。
 * run_command 的子进程与宿主同属一个用户，文件权限位不构成防线。
 * 环境变量来源有两道防线：buildCleanEnvp 剔除子进程继承的敏感名变量，
 * 宿主又在注入密钥后抹掉自己 exec 时环境块里的那些串（否则同 UID 进程
 * cat /proc/<宿主 pid>/environ 就能读走）。两道都只挡「顺手一读」——
 * 能 ptrace 宿主的同 UID 进程照样可以从内存里把密钥捞出来，真要对抗
 * 恶意 shell 得把 agent 放进专用用户 / 容器。
 * 2026-09-20 评估：原 Cli（--llm-key）来源已随该选项一并移除。 */
enum class ApiKeySource {
	None,		/* 尚未解析出（buildAgentConfig 此时会直接失败，仅作初值） */
	Env,		/* 环境变量链——子进程清洗 + 宿主环境块抹零，两道都过 */
	ConfigInline,	/* 配置文件 api_key 字段——配置文件本身可读 */
	ConfigFile,	/* 配置文件 api_key_file 指向的密钥文件——该文件可读 */
};

struct AgentModeConfig {
	/* 协议驱动：表名即 modules/<表名>.lua；llmVar 是承载它的变量名。
	 * 结构脚本声明了就优先用；没声明则宿主按这两个名字自动注入。 */
	std::string driverTable = "LlmOpenAI";
	std::string llmVar = "llm0";
	/* 空 = 用驱动自带的缺省接入点/模型（见 modules/lib/llm_wire_*.lua 的 DEFAULTS） */
	std::string endpoint;
	std::string model;
	int maxTurns = 8;
	int maxTokens = 4096;
	double temperature = 0.7;
	double topP = 0.9;
	long timeoutSec = 180;		/* 单次模型调用超时（秒） */
	/* 驱动特有字段：原样透传给协议驱动（如 DeepSeek 的 thinking / reasoning_effort） */
	std::string reasoningEffort;
	std::string thinking;
	/* 摄像头（capture_image 工具）。allowCamera=false 时不提供该工具；
	 * 驱动变量解析不到时也只是不提供（摄像头对机器人是可选外设）。 */
	std::string cameraVar = "cam0";
	std::string cameraDevice;	/* 空 = 驱动缺省 /dev/video0 */
	std::string cameraSrc;		/* 非空 = 模拟图片来源文件（无摄像头也能跑通全链路） */
	bool allowCamera = true;
	std::string scriptsDir = "scripts";
	std::string structurePath;
	std::string task;		/* 非空 = 一次性执行；空 = 交互 REPL */
	bool allowShell = false;	/* true = 注册 run_command（缺省关，见 actionsource.hpp 注） */
	bool verbose = true;

	/* ---- 提示词包（--prompt-file / 配置文件 prompt_files）----
	 * promptFiles 是合并后的输入清单（命令行给过就整体覆盖配置文件列表）；
	 * promptPack 是 buildAgentConfig 加载拼接好的成品文本（空 = 未使用），
	 * buildAgentSystemPrompt 直接追加——加载放配置组装期，路径写错这类
	 * 配置错误在结构装载与上电之前就失败（「配置错误不碰真机」）。 */
	std::vector<std::string> promptFiles;
	std::string promptPack;

	/* ---- 由 buildAgentConfig 填出的旁路信息 ---- */
	/* 最终解析出的 API key。**只**用于注入进程环境变量，绝不进驱动的参数表
	 * （否则 BOT4CJ_DEBUG 的参数转储会把密钥打进日志）。 */
	std::string resolvedApiKey;
	/* 密钥来源，用于 shellKeyExposureWarning 判定「是否对 agent 的 shell 可见」 */
	ApiKeySource apiKeySource = ApiKeySource::None;
	/* 实际使用的配置文件路径（空 = 没用配置文件），用于在会话开始时如实告知 */
	std::string configPath;
	/* 配置文件相关的告警（如密钥文件权限过松），空表示无 */
	std::string configWarning;
};

/* run_command 启用（cfg.allowShell）且 API key 来自「agent 的 shell 也能读到」
 * 的来源（配置文件 / 密钥文件）时，返回启动告警文本；密钥来自环境变量
 * 或 shell 未启用（缺省）时返回空串。由调用方打印到 stderr。 */
std::string shellKeyExposureWarning(const AgentModeConfig& cfg);

/* API key 只从环境变量链（BOT4CJ_LLM_KEY → ROBOT_AGENT_API_KEY → DEEPSEEK_API_KEY）
 * 或配置文件（api_key / api_key_file）取，命令行不提供密钥选项：
 * 宿主不把密钥写进驱动的参数表——否则 BOT4CJ_DEBUG 的参数转储会把密钥打进日志。
 * 驱动自己也按同一条链读环境变量。 */
std::string resolveLlmKeyFromEnv();

/* 由 CLI 选项组装配置。失败返回 false 并把「人话报错」写进 err。 */
/* AgentModeConfig → AgentDriverConfig 的单一搬运点（重构计划 2.6）：
 * 请求相关字段的清单变更只改两份结构体与本函数，不再散落 main 的手工赋值 */
AgentDriverConfig makeDriverConfig(const AgentModeConfig& cfg);

bool buildAgentConfig(const CliOptions& opts, AgentModeConfig& out, std::string& err);

/* 系统提示词：机器人结构与硬件清单 + 可用动作脚本 + 工作方式与安全约束 */
/* 系统提示词总量上限（F2-9，2026-09-21）。三段来源都各自有界、却都无法自证
 * 整体规模：结构脚本头（16 KiB 截断）、动作脚本清单（条目数不限）、提示词包
 * （单文件 1 MiB 上限、整包不限）。任一段被塞爆都会顶穿 provider 上下文，
 * 或把真正重要的指令区挤出有效注意力；静默截断更糟——模型看到的系统指令与
 * 作者以为的不一致。故 fail-closed：超限即拒绝启动 agent 会话，报错里给出
 * 各段实测字节数。64 KiB ≈ 当前真实用量的 2.5 倍（示例包 6.4 KiB + 结构脚本
 * 头 ≤16 KiB + 两份清单），正常使用不会碰到。 */
inline constexpr std::size_t kMaxSystemPromptBytes = 64u << 10;

/* REPL 单行输入上限（2026-09-22 审计后添加）。防止恶意输入巨大任务字符串
 * 导致内存耗尽（std::getline 无内建长度限制，只在系统 RLIMIT_AS / 坏分配
 * 时才会 bad_alloc）。10 MiB 远超正常任务描述（几百字节），同时兼顾偶尔
 * 粘贴长文本的合理场景。超限响亮拒绝并提示，不静默截断。 */
inline constexpr std::size_t kMaxReplInputBytes = 10u << 20;

std::string buildAgentSystemPrompt(const AgentModeConfig& cfg, RobotStruct& robot);

/* 跑 agent 会话。返回进程退出码：0 正常结束；1 模型/执行失败。 */
int runAgentMode(const AgentModeConfig& cfg, RobotStruct& robot, ChatTransport& transport,
		std::istream& in, std::ostream& out, std::ostream& log);

} // namespace agent
} // namespace bot
