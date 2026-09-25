#pragma once
/* agentmode.hpp —— agent 工具后端的配置组装（--mcp-backend）
 *
 * C++ 侧在 agent 体系里的职责是「工具后端」：main 在 --mcp-backend 下先
 * active() 机器人，再由 mcpbackend 在 AF_UNIX socket 上同步服务工具调用，
 * 全程硬件常驻；会话结束才 deactivate。agent 循环本体（REPL/模型调用/预算/
 * 历史裁剪）在 Rust 侧 agent/src（mybot-agent），经帧协议调用这里的工具层。
 *
 * 本模块只做配置组装与工具上下文：buildAgentConfig（命令行/环境/配置文件/
 * 内置缺省四层合并 + 能力门控 + 审计/策略装载）与 makeToolContext（工具表
 * 的单一落点）。模型调用不在这里——LLM 请求由 Rust agent 直连 provider。
 */

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "actionsource.hpp"
#include "agentaudit.hpp"
#include "agentpolicy.hpp"
#include "agentproto.hpp"
#include "agentprompt.hpp"	/* kMaxSystemPromptBytes（A2 拆出；体积预检用） */
#include "agenttools.hpp"	/* AgentToolContext（makeToolContext 的返回类型） */

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
	/* 协议驱动：driverTable 是 Rust agent 内建的协议族名
	 * （LlmOpenAI / LlmDeepSeek / LlmAnthropic——2026-09-25 #8 起不再是
	 * modules/<表名>.lua 的驱动表名，宿主不加载任何 LLM 驱动，只把名字
	 * 透传给 agent）；llmVar 仅是变量名标签，供提示词/诊断引用。 */
	std::string driverTable = "LlmOpenAI";
	std::string llmVar = "llm0";
	/* 空 = 未配置：Rust 侧 fail-closed（endpoint 与 model 都**无内置缺省**，
	 * 首次调用即失败并点名三种配置途径——B2 口径，2026-09-22 起） */
	std::string endpoint;
	std::string model;
	int maxTurns = 8;
	int maxTokens = 4096;
	double temperature = 0.7;
	double topP = 0.9;
	long timeoutSec = 180;		/* 单次模型调用超时（秒） */
	/* 单个任务的墙钟预算（秒，A4）。与 timeoutSec 不同维度：那条管一次模型
	 * 调用，这条管「一个任务从头到尾能让硬件带电多久」。0 = 不限。 */
	long maxTaskSeconds = 900;
	/* 驱动特有字段：原样透传给协议驱动（如 DeepSeek 的 thinking / reasoning_effort） */
	std::string reasoningEffort;
	std::string thinking;
	/* 摄像头（capture_image 工具）。allowCamera=false 时不提供该工具；
	 * 驱动变量解析不到时也只是不提供（摄像头对机器人是可选外设）。 */
	std::string cameraVar = "cam0";
	std::string cameraDevice;	/* 空 = 驱动缺省 /dev/video0 */
	std::string cameraSrc;		/* 非空 = 模拟图片来源文件（无摄像头也能跑通全链路） */
	/* H1：cameraSrc 是否来自命令行（--camera-src）。只有命令行来源才允许用
	 * "src 的所在目录"当白名单根；配置文件来源一律不推导（详见 agenttools.hpp
	 * 的 AgentToolContext 注释——旧实现无条件推导，判据恒真，等于没有白名单）。 */
	bool cameraSrcFromCli = false;
	/* H1：capture_image 模拟图片的白名单根目录（--camera-src-dir）。
	 * **只能命令行给**：它决定"能读哪个目录"，等价于放宽读权限，故与 allow_shell
	 * 同判据——配置文件里写 camera_src_dir 一律拒绝加载并指名原因
	 * （缺省查找会读 CWD 的 ./mybot.llm.json，「这个文件存在」不构成授权）。 */
	std::string cameraSrcDir;
	bool allowCamera = true;
	std::string scriptsDir = "scripts";
	std::string structurePath;
	std::string task;		/* 非空 = 一次性执行；空 = 交互 REPL */
	bool allowShell = false;	/* true = 注册 run_command（缺省关，见 actionsource.hpp 注） */
	/* 热插表（2026-09-23）：true = 注册 apply_sql（结构 SQL 带电生效）。
	 * 与 allowShell 同一条门控链：只能命令行 --agent-hot-sql 开启，配置文件
	 * 的 allow_hot_sql 只能关不能开（见 actionsource.hpp 注）。 */
	bool allowHotSql = false;
	/* 回读（2026-09-23，发现 2 的 A 步）：允许模型**主动读**的硬件变量名清单
	 * （--read-var，可重复）。**命令行独占**：配置文件写 read_vars 会被拒绝加载
	 * （与 allow_shell / camera_src_dir 同判据）。空 = 不注册 read_variables。 */
	std::vector<std::string> readVars;
	/* 断连安全动作（2026-09-25，--on-disconnect）：连接结束后由后端自行执行的
	 * 动作脚本（scriptsDir 之下的相对路径）。命令行独占（配置文件不能给）；
	 * buildAgentConfig 在上电前经 loadActionScriptFile 预检（越界/不存在/为空
	 * 即配置错误）。空 = 不执行。执行判据与 run_action_script 同源
	 * （runActionScriptFile），但**不经 DispatchGate/策略确认**：这是操作者
	 * 预先配置的安全动作，不是模型的请求，等外部闸门反而违背它的目的。 */
	std::string onDisconnectScript;
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
	 * 由 Rust 侧 build_system_prompt 追加（经 context.get 送达）——加载放配置
	 * 组装期，路径写错这类
	 * 配置错误在结构装载与上电之前就失败（「配置错误不碰真机」）。 */
	std::vector<std::string> promptFiles;
	std::string promptPack;

	/* ---- 提示词素材缓存（C6，2026-09-22）----
	 * 第 9 步的体积预检要读这两段，读出来的成品存这里供 MCP 后端的
	 * context.get 复用（单一读法，单一事实源）：每会话少一次全树遍历 + N 次
	 * 文件读，且
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

	/* ---- 会话持久化（批次 1，2026-09-23）----
	 * sessionFile：会话文件路径（--session-file / 配置文件 session_file）。
	 * 写入是**纯输出**（判据同 audit_log：可进配置文件）；恢复是**输入通道**
	 * （磁盘内容读进模型上下文），故 --resume 只认命令行给出的路径——配置文件
	 * 来源的路径不能授权一次读取（与 H2 给提示词路径加根约束同判据），
	 * 配置文件里的 session_file 只用于写入。 */
	std::string sessionFile;
	bool sessionFileFromCli = false;	/* H1 同款：路径是否来自命令行 */
	bool resumeSession = false;		/* --resume：必须与命令行 --session-file 联用 */
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

/* 结构脚本展示名（去目录、去 .sql 后缀）：REPL 提示符、会话文件的 robot 指纹
 * 字段与 MCP 后端 context.get 的 robotName 共用——各写一遍迟早分叉。 */
std::string structureDisplayName(const std::string& path);

/* 由 CLI 选项组装配置。失败返回 false 并把「人话报错」写进 err。 */
bool buildAgentConfig(const CliOptions& opts, AgentModeConfig& out, std::string& err);

/* PL-1（2026-09-22，--check-config）：把**已经校验通过的**配置渲染成人类可读摘要。
 * 纯函数、无副作用（密钥只报来源不报值），供 main 在校验模式下打印后退出 0。
 * 注意它**不做校验**——判据单点在 buildAgentConfig（失败时调用方已 return 2），
 * 这里再校验一遍只会制造两处漂移。 */
std::string summarizeAgentConfig(const AgentModeConfig& cfg);

/* 组装工具上下文（AgentToolContext）的**单一落点**：MCP 后端（--mcp-backend）经此
 * 得到工具表，于是「对外提供哪些工具、能读哪些变量、摄像头怎么解析」的判据只此一处。
 * 只读 cfg + robot（调用方须先 active()）；不上电、不发起模型调用。
 * 跳过项（--read-var 名字打错等）如实写进 log，不静默降级。 */
AgentToolContext makeToolContext(const AgentModeConfig& cfg, RobotStruct& robot,
		std::ostream& log);

} // namespace agent
} // namespace bot
