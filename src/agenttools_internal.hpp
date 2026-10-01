#pragma once
/* agenttools_internal.hpp —— agent 工具层的**模块内**接缝（2026-09-25 B3 拆分）
 *
 * agenttools.cpp（工具装配）/ agentview.cpp（只读视图族）/ agentimage.cpp
 * （摄像头簇）/ agenttools_wait.cpp（wait_variable 轮询机器，2026-10-01）四个
 * TU 共享、但不属于对外契约的件。对外契约（AgentToolRegistry/makeAgentTools/
 * runSqlStatement/describe* 等，供 mcpbackend/agentmode 消费）在 agenttools.hpp；
 * 加到这里的东西以「多 TU 都要用」为准，别把单 TU 私有件挪进来。 */

#include "agenttools.hpp"

namespace bot {
namespace agent {

/* JSON Schema 组装（各 make*Tool 共用；agentimage.cpp 的 capture_image 也用） */
Json toolSchema(const std::vector<std::pair<std::string, Json>>& props,
	const std::vector<std::string>& required);
Json intParam(const std::string& desc);
Json stringParam(const std::string& desc);

/* 可选整数参数：缺失/null 取 fallback；类型不符、越界、非有限一律抛
 * IllegalArgumentException（不静默回落——模型会把「被忽略」误读成「已生效」） */
int optionalInt(const Json& args, const std::string& key, int fallback);

/* 必选字符串参数：缺失/空串/非字符串/内嵌 NUL 一律抛（NUL 判据的理由见
 * agenttools.cpp 实现处——run_command 的静默截断防线） */
std::string requireString(const Json& args, const std::string& key);

/* 动作脚本读取（M2/B2-3 单点）：底层 FD 骨架是 util::readFdNoFollowLimited，
 * 中文报错文案被 grep 守门锚定；agentview.cpp（describeActionScripts 取首注释）
 * 与 agenttools.cpp（run_action_script 读脚本）两侧共用。 */
std::string readFileLimited(const std::string& path, std::size_t cap);

/* SHOW TABLES 的只读渲染（runSqlStatement 的 SHOW 分支单点，agentview.cpp） */
std::string renderShowTables(const RobotStruct& robot);

/* capture_image 工具的装配（agentimage.cpp；makeAgentTools 按能力门控挂载） */
AgentTool makeCaptureImageTool(const AgentToolContext& ctx);

/* wait_variable 工具的装配（agenttools_wait.cpp；与 read_variables 同门控，
 * makeAgentTools 按「至少一个可读变量」挂载） */
AgentTool makeWaitVariableTool(const AgentToolContext& ctx);

} // namespace agent
} // namespace bot
