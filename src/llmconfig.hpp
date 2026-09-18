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

namespace bot {

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
