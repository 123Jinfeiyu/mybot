/* robot.cpp —— RobotStruct 结构变更门面：热插 */

#include "robot.hpp"

#include "util_env.hpp"

namespace bot {

/* ==================== RobotStruct：结构变更门面 ==================== */
/* 热插 SQL 仅允许在机器人处于 active 状态时执行。 */
std::string RobotStruct::applySql(const std::string& sql) {
	if (state_ != LifecycleState::Active) {
		throw IllegalArgumentException("热插表需要机器人已上电常驻（active() 之后）；"
			"未上电时请把语句写进结构脚本");
	}
	return hotSql_.apply(sql, deactivateGraceMs_);
}

} // namespace bot