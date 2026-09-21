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
} // namespace llm_limits

struct LlmFileConfig {
	std::optional<std::string> driver;	/* 协议驱动表名 = modules/<名>.lua */
	std::optional<std::string> var;		/* 承载驱动的变量名 */
	std::optional<std::string> endpoint;
	std::optional<std::string> model;
	std::optional<std::string> apiKey;
	std::optional<std::string> apiKeyFile;	/* 从文件读密钥（支持 ~ 展开、去空白） */
	std::optional<long> timeoutSec;
	std::optional<int> maxTokens;
	std::optional<double> temperature;
	std::optional<double> topP;
	std::optional<std::string> reasoningEffort;	/* 驱动特有：透传给驱动 */
	std::optional<std::string> thinking;		/* "enabled"/"disabled"，也接受 true/false */
	std::optional<int> maxTurns;
	std::optional<std::string> scriptsDir;
	std::optional<bool> allowShell;
	/* 提示词包输入清单（每项为文件或目录，语义同 --prompt-file）。空数组按
	 * 未配置；类型不符（非数组/元素非字符串）报错——拼错形态不能静默失效。 */
	std::optional<std::vector<std::string>> promptFiles;
	std::optional<std::string> cameraVar;
	std::optional<std::string> cameraDevice;
	std::optional<std::string> cameraSrc;
	std::optional<bool> allowCamera;
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

} // namespace bot
