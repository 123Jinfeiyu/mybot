#pragma once
/* llmconfig.hpp —— LLM/agent 配置文件（JSON）读取
 *
 * 为什么用 JSON 而不是 Lua：配置文件是**数据**，不是代码。Lua 配置要由宿主内嵌
 * 一个状态机去执行，等于让一份常被复制/分发的文件获得任意代码执行能力；而仓库
 * 已经有严格 JSON 解析器（src/jsonparse.cpp），复用它零成本，还自带「拒绝尾随
 * 字符/坏转义」的严格性。
 *
 * 字段全部可选：缺省即交给下一层（命令行 > 环境变量 > 本文件 > 驱动内置缺省）。
 * 未知字段一律拒绝——与「未知 --key 退出 2」同一口径，防拼错后静默失效。
 */

#include <optional>
#include <string>
#include <vector>

namespace bot {

/* ---- LLM 选项/配置字段取值区间（B2-4 单点，2026-09-21）----
 * 同一组上下界此前在 llmconfig.cpp（配置文件字段）、agentmode.cpp（命令行选项）
 * 与 docs/reference.md 各写一遍：改一处忘一处就出现「配置文件接受、命令行拒绝」
 * 这类判据漂移。timeout 上限与驱动侧 llm_http.clamp_timeout 的钳制区间一致
 * ——宿主接受什么，驱动就照发什么，不做「先接受再悄悄改写」。 */
namespace llm_limits {
inline constexpr int kTimeoutMinSec = 1;
inline constexpr int kTimeoutMaxSec = 600;
inline constexpr int kMaxTokensMin = 1;
inline constexpr int kMaxTokensMax = 200000;
inline constexpr int kMaxTurnsMin = 1;
inline constexpr int kMaxTurnsMax = 100;
/* 上下文字节预算（B1，2026-09-22）。原先这三个量只写死在 AgentOptions 编译期
 * 缺省（docs/agent.md 已知限制时此地自认「只能改源码缺省」），现场调不了。
 * 现在经配置文件可覆盖；0 = 不限（与 AgentOptions 的既有语义一致）。
 * 上限取「能接受的最大合理值」：历史 4 MiB 缺省、图片单张 2 MiB base64，给
 * 64 MiB 让重度视觉会话也有余量，同时仍挡掉明显打错的数量级（GB 级会把
 * 请求体直接顶穿任何网关）。 */
inline constexpr long kMaxHistoryBytesLimit = 64u << 20;
inline constexpr long kMaxTaskToolResultBytesLimit = 64u << 20;
inline constexpr long kMaxTaskImageBytesLimit = 64u << 20;
/* 单任务墙钟预算（A4）。下界 0 = 不限（逃生口，保留旧行为）；上界取一天，
 * 防"为了保险配大一点"把带电时长写成事实上的无界。 */
inline constexpr int kTaskTimeoutMinSec = 0;
inline constexpr int kTaskTimeoutMaxSec = 86400;

/* ---- 运行期数值（B4，2026-09-22）----
 * 这五个量原先只写死在 AgentOptions / AgentDriverConfig / AgentToolContext 的
 * 编译期缺省里，现场调不了（与 B1 的三个字节预算同款问题）。现经配置文件可覆盖，
 * 仍**不加 CLI 选项**：选项面已 20+，而它们都是「一次定好、长期不变」的部署参数。
 * 区间取「能接受的最大合理值」，逐条理由：
 *   * max_calls_per_round 下限 1：0 在 AgentOptions 里是「不限」的旧语义，但对
 *     物理机器人「一轮不限量下发动作」没有正当用法，故配置文件**拒绝 0**
 *     （与 allow_shell 同源的「能力只能命令行开、配置只能收紧」口径）；1024
 *     远超任何现实场景；
 *   * max_retries 0 合法 = 只发一次不重试（离线/内网直连想立刻失败时用），
 *     6 已远超任何网关抖动所需；
 *   * retry_backoff_ms 上界 10000，此外驱动侧单次退避再钳到 kRetryBackoffCapMs
 *     ——`backoff << attempt` 在放宽区间后会算出小时级等待，等待比失败更糟；
 *   * run_command 两个超时沿用「缺省 ≤ 上限」的既有形态：缺省上限 600（10 分钟），
 *     请求上限 3600（1 小时）已覆盖现场长任务，再长得先想想是不是该用
 *     run_action_script（脚本执行有硬件侧语义，而不是宿主任意命令）。 */
inline constexpr int kMaxCallsPerRoundMin = 1;
inline constexpr int kMaxCallsPerRoundMax = 1024;
inline constexpr int kMaxRetriesMin = 0;
inline constexpr int kMaxRetriesMax = 6;
inline constexpr int kRetryBackoffMsMin = 0;
inline constexpr int kRetryBackoffMsMax = 10000;
inline constexpr int kRetryBackoffCapMs = 60000;
inline constexpr int kRunCommandTimeoutMinSec = 1;
inline constexpr int kRunCommandTimeoutMaxSec = 600;
inline constexpr int kRunCommandMaxTimeoutMinSec = 1;
inline constexpr int kRunCommandMaxTimeoutMaxSec = 3600;
/* PL-2（2026-09-22）：动作脚本执行预算。0 = 显式关闭（退化为此前无预算行为）；
 * 上限 3600 与 run_command 的长任务量级对齐；缺省 180 与单次模型调用缺省
 * （agentdriver.hpp 的 timeoutSec）取齐——两者都是「agent 面向的单次阻塞
 * 操作」的上界，同一个数字便于操作者记忆。 */
inline constexpr int kActionScriptTimeoutMinSec = 0;
inline constexpr int kActionScriptTimeoutMaxSec = 3600;
} // namespace llm_limits
struct LlmFileConfig {
	std::optional<std::string> driver;	/* 协议驱动表名 = modules/<名>.lua */
	std::optional<std::string> var;		/* 承载驱动的变量名 */
	std::optional<std::string> endpoint;
	std::optional<std::string> model;
	std::optional<std::string> apiKey;
	std::optional<std::string> apiKeyFile;	/* 从文件读密钥（支持 ~ 展开、去空白） */
	std::optional<long> timeoutSec;
	/* 单任务墙钟预算（秒）。配置文件键名 max_task_seconds；0 = 不限。 */
	std::optional<int> maxTaskSeconds;
	std::optional<int> maxTokens;
	std::optional<double> temperature;
	std::optional<double> topP;
	std::optional<std::string> reasoningEffort;	/* 驱动特有：透传给驱动 */
	std::optional<std::string> thinking;		/* "enabled"/"disabled"，也接受 true/false */
	std::optional<int> maxTurns;
	std::optional<std::string> scriptsDir;
	std::optional<bool> allowShell;
	/* 热插表（2026-09-23）：apply_sql 会立刻给新硬件上电，与 allow_shell 同判据
	 * ——解析出来**只为了拒绝它**（只能由命令行 --agent-hot-sql 开启）。之所以
	 * 仍列进已知字段，是为了给出指名道姓的原因，而不是让人以为是拼写错误
	 * （与下面的 camera_src_dir 同一处理）。 */
	std::optional<bool> allowHotSql;
	/* 提示词包输入清单（每项为文件或目录，语义同 --prompt-file）。空数组按
	 * 未配置；类型不符（非数组/元素非字符串）报错——拼错形态不能静默失效。 */
	std::optional<std::vector<std::string>> promptFiles;
	std::optional<std::string> cameraVar;
	std::optional<std::string> cameraDevice;
	std::optional<std::string> cameraSrc;
	/* H1：这个字段被解析出来**只为了拒绝它**——capture_image 的白名单根目录
	 * 只能由命令行 --camera-src-dir 提供（配置文件不能放宽读权限，与 allow_shell
	 * 同判据）。之所以仍列进已知字段而不是靠"未知字段"报错，是为了给出指名道姓的
	 * 原因，而不是让人以为是拼写错误。 */
	std::optional<std::string> cameraSrcDir;
	/* 回读（2026-09-23，发现 2 的 A 步）：这个字段被解析出来**只为了拒绝它**——
	 * `read_variables` 的可读清单只能由命令行 `--read-var` 提供（配置文件不能给
	 * 模型开放"主动读硬件"的能力，与 allow_shell / camera_src_dir 同判据）。
	 * 仍列进已知字段而不是靠"未知字段"报错，是为了给出指名道姓的原因。 */
	std::optional<std::vector<std::string>> readVars;
	std::optional<bool> allowCamera;
	/* 审计轨迹落点（JSON Lines；"-" = stderr）。空/未配置 = 不记录。
	 * 与 --audit-log 同义；命令行优先。 */
	std::optional<std::string> auditLog;
	/* 策略钩子文件（--agent-policy 同义；命令行优先）。空/未配置 = 不启用。 */
	std::optional<std::string> agentPolicy;
	/* 会话文件（批次 1，--session-file 同义）。**只用于写入**：恢复是输入
	 * 通道，--resume 只认命令行给的路径（判据见 AgentModeConfig 注释）。 */
	std::optional<std::string> sessionFile;
	/* 上下文字节预算（B1）：会话历史 / 任务内文本 / 任务内图片，单位字节。
	 * 0 = 不限；未配置 = 用 AgentModeConfig 的编译期缺省（见 agentmode.hpp）。 */
	std::optional<long> maxHistoryBytes;
	std::optional<long> maxTaskToolResultBytes;
	std::optional<long> maxTaskImageBytes;
	/* 运行期数值（B4）：单轮工具调用数上限 / 网络重试（尝试次数与退避基础时长）/
	 * run_command 的缺省与最大超时。未配置 = 用各结构体的编译期缺省；
	 * 区间与决策理由见 llm_limits。跨字段关系（上限 ≥ 缺省）在 agentmode 按
	 * **生效值**判定——只配一个字段时也要挡住「上限低于缺省」。 */
	std::optional<int> maxCallsPerRound;
	std::optional<int> maxRetries;
	std::optional<int> retryBackoffMs;
	std::optional<int> runCommandTimeoutSec;
	std::optional<int> runCommandMaxTimeoutSec;
	/* PL-2：动作脚本执行预算（秒）。0 = 显式关闭预算。 */
	std::optional<int> actionScriptTimeoutSec;
};

/* 解析配置文件。失败返回 false 并把「带字段名的人话报错」写进 err：
 * 文件不可读、不是合法 JSON、字段类型/范围不符、未知字段全部拒绝。 */
bool loadLlmConfigFile(const std::string& path, LlmFileConfig& out, std::string& err);

/* 缺省配置文件查找：./mybot.llm.json → ./mybot.llm.conf.json →
 * $HOME/.config/mybot/llm.json。都不存在返回空串（配置文件是可选的）。 */
std::string findDefaultLlmConfig();

/* 读密钥文件：~ 展开、去首尾空白（末尾换行最容易变成 401）、64 KiB 上限。 */
bool readApiKeyFile(const std::string& path, std::string& out, std::string& err);

/* 密钥文件权限体检：对 group/other 可读时返回一句告警，否则返回空串。
 * 只告警不阻断——容器/只读挂载等场景下权限不由使用者控制。 */
std::string secretFilePermissionWarning(const std::string& path);

/* API key 字符集判据（2026-09-21 审计 AG-14 单点）：拒绝控制字符（<0x20 与
 * 0x7F）。密钥会被拼进 `Authorization: Bearer <key>` 交给 curl_slist_append，
 * 而 libcurl 不拒绝 slist 里的 CR/LF——实测一个多行密钥文件会把换行后的内容
 * 变成**额外的请求头**发给网关（头注入）。
 * 合法返回 true；非法返回 false 并把原因（含位置与字节值）写进 err。 */
bool validateApiKeyChars(const std::string& key, std::string& err);

} // namespace bot
