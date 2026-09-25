#pragma once
/* agenttools_internal.hpp —— agent 工具层的**模块内**接缝（2026-09-25 B3 拆分）
 *
 * agenttools.cpp（工具装配）/ agentview.cpp（只读视图族）/ agentimage.cpp
 * （摄像头簇）三个 TU 共享、但不属于对外契约的件。对外契约（AgentToolRegistry/
 * makeAgentTools/runSqlStatement/describe* 等，供 mcpbackend/agentmode 消费）
 * 在 agenttools.hpp；加到这里的东西以「三 TU 都要用」为准，别把单 TU 私有件
 * 挪进来。 */

#include "agenttools.hpp"

namespace bot {
namespace agent {

/* JSON Schema 组装（各 make*Tool 共用；agentimage.cpp 的 capture_image 也用） */
Json toolSchema(const std::vector<std::pair<std::string, Json>>& props,
	const std::vector<std::string>& required);

/* 动作脚本读取（M2/B2-3 单点）：底层 FD 骨架是 util::readFdNoFollowLimited，
 * 中文报错文案被 grep 守门锚定；agentview.cpp（describeActionScripts 取首注释）
 * 与 agenttools.cpp（run_action_script 读脚本）两侧共用。 */
std::string readFileLimited(const std::string& path, std::size_t cap);

/* SHOW TABLES 的只读渲染（runSqlStatement 的 SHOW 分支单点，agentview.cpp） */
std::string renderShowTables(const RobotStruct& robot);

/* capture_image 工具的装配（agentimage.cpp；makeAgentTools 按能力门控挂载） */
AgentTool makeCaptureImageTool(const AgentToolContext& ctx);

} // namespace agent
} // namespace bot
