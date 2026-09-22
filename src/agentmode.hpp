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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "actionsource.hpp"
#include "agentaudit.hpp"
#include "agentdriver.hpp"
#include "agentloop.hpp"	/* AgentSession（C3：runAgentRepl 签名需要） */
#include "agentpolicy.hpp"
#include "agentproto.hpp"
#include "agentprompt.hpp"	/* buildAgentSystemPrompt / kMaxSystemPromptBytes（A2 拆出） */

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

	/* ---- 上下文字节预算（B1）----
	 * 0 = 不限；三类全部可由配置文件覆盖（llmconfig 新字段），未配置时保持
	 * 编译期缺省并透传给 AgentOptions。原为 AgentOptions 的三个编译期常量，
	 * 现场调不了（docs/agent.md 已知限制），B1 把「配置 > 缺省」通道接通。 */
	std::size_t maxHistoryBytes = 4 * 1024 * 1024;
	std::size_t maxTaskToolResultBytes = 2 * 1024 * 1024;
	std::size_t maxTaskImageBytes = 4 * 1024 * 1024;

	/* ---- 运行期数值（B4，2026-09-22）----
	 * 配置文件同名小写字段的落点（区间单点见 llm_limits）：缺省即原先写死在
	 * 下游结构体里的值，未配置时不改变任何行为。去向见各注释。 */
	int maxCallsPerRound = 16;	/* 单轮工具调用数上限 → AgentOptions */
	int maxRetries = 3;		/* 网络重试尝试次数（含首次）→ AgentDriverConfig */
	int retryBackoffMs = 1000;	/* 退避基础时长（指数增长，单次钳到 60s）→ 同上 */
	int commandTimeoutSec = 10;	/* run_command 缺省超时 → AgentToolContext */
	int maxCommandTimeoutSec = 60;	/* run_command 超时上限 → AgentToolContext */
	/* PL-2：动作脚本执行预算（秒）→ AgentToolContext。0 = 显式关闭预算。 */
	int scriptTimeoutSec = 180;

	/* ---- 提示词包（--prompt-file / 配置文件 prompt_files）----
	 * promptFiles 是合并后的输入清单（命令行给过就整体覆盖配置文件列表）；
	 * promptPack 是 buildAgentConfig 加载拼接好的成品文本（空 = 未使用），
	 * buildAgentSystemPrompt 直接追加——加载放配置组装期，路径写错这类
	 * 配置错误在结构装载与上电之前就失败（「配置错误不碰真机」）。 */
	std::vector<std::string> promptFiles;
	std::string promptPack;

	/* ---- 提示词素材缓存（C6，2026-09-22）----
	 * 下面第 9 步的体积预检本来就要读这两段，读出来的成品存这里供
	 * buildAgentSystemPrompt 复用：每会话少一次全树遍历 + N 次文件读，且
	 * 「预检口径」与「提示词口径」按构造一致（原先两处各读一次，两次读之间
	 * 文件若变动会分叉——预检按旧内容放行、提示词按新内容生成）。
	 * 用 optional 而非空串：空串是**合法的读取结果**（结构脚本不可读、脚本
	 * 目录为空），拿它表示「没缓存」会把已读到的空结果误判成未命中再读一遍；
	 * 离线单测手搓配置时没有值，走实时读取（与缓存引入前完全一致）。 */
	std::optional<std::string> structureHead;	/* = structureScriptHead(…, 16 KiB) */
	std::optional<std::string> scriptsList;		/* = describeActionScripts(…) */

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

	/* ---- 审计轨迹（--audit-log / 配置文件 audit_log；空 = 关闭）----
	 * shared_ptr：buildAgentConfig 在这里打开（打不开即配置错误，上电前失败），
	 * AgentOptions 与本配置共享同一实例。auditLogPath 只用于会话开始的如实告知。 */
	std::shared_ptr<AuditLog> audit;
	std::string auditLogPath;

	/* ---- 策略钩子（--agent-policy / 配置文件 agent_policy；空 = 不启用）----
	 * 在 buildAgentConfig 里加载成成品（策略文件写错属配置错误，上电前失败）；
	 * policyPath 只用于会话开始的如实告知。与 run_command 的开关互补：
	 * --agent-shell 管「工具给不给」，策略管「给了之后每次调用要不要过外部闸门」。 */
	std::shared_ptr<const AgentPolicy> policy;
	std::string policyPath;
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

/* PL-1（2026-09-22，--check-config）：把**已经校验通过的**配置渲染成人类可读摘要。
 * 纯函数、无副作用（密钥只报来源不报值），供 main 在校验模式下打印后退出 0。
 * 注意它**不做校验**——判据单点在 buildAgentConfig（失败时调用方已 return 2），
 * 这里再校验一遍只会制造两处漂移。 */
std::string summarizeAgentConfig(const AgentModeConfig& cfg);

/* SAN-10（2026-09-22，REPL /status）：把会话**当前已有的状态**渲染成人类可读一览。
 * 纯函数、无副作用、可离线单测——与 summarizeAgentConfig 同款契约：
 * **只渲染、不判定**。它回答的是现场最常见的三个问题：
 * "为什么它忘了之前说的？"（历史多大/裁过没）"为什么拍照没进来？"（任务内图片额度用了多少）
 * "这一轮花了多久/失败几次？"（模型轮数、工具调用、失败数、token 用量）。
 * 这些数字**此前只在出问题时才可见**（裁剪那一刻打一行、预算超限时才说），没有查询入口。 */
std::string summarizeSessionStatus(const AgentSession& session, const AgentModeConfig& cfg,
		RobotStruct& robot);

/* REPL 单行输入上限（2026-09-22 审计后添加）。防止恶意输入巨大任务字符串
 * 导致内存耗尽（std::getline 无内建长度限制，只在系统 RLIMIT_AS / 坏分配
 * 时才会 bad_alloc）。10 MiB 远超正常任务描述（几百字节），同时兼顾偶尔
 * 粘贴长文本的合理场景。超限响亮拒绝并提示，不静默截断。 */
inline constexpr std::size_t kMaxReplInputBytes = 10u << 20;

/* runAgentMode：跑 agent 会话。返回进程退出码：0 正常结束；1 模型/执行失败。 */
int runAgentMode(const AgentModeConfig& cfg, RobotStruct& robot, ChatTransport& transport,
		std::istream& in, std::ostream& out, std::ostream& log);

/* REPL 循环本体（C3，2026-09-22）。从 runAgentMode 拆出：装配逻辑（工具表/
 * 审计/上电/一次性任务分支）留在 runAgentMode，循环只依赖已就绪的 session、
 * 只读的配置视图与 robot（/hardware 只读清单用）。I/O 全经参数注入，可离线
 * 单测：内置命令派发、输入超限拒绝、单轮失败不结束会话、/exit 与 EOF 退出码。
 * 返回 0（/exit、/quit、EOF、stop 请求都算正常结束）。 */
int runAgentRepl(AgentSession& session, const AgentModeConfig& cfg,
		RobotStruct& robot, std::istream& in, std::ostream& out, std::ostream& log);

} // namespace agent
} // namespace bot
