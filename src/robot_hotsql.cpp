/* robot_hotsql.cpp —— RobotStruct 热 SQL 应用入口 */

#include "robot.hpp"

#include "util_env.hpp"

namespace bot {

std::string RobotStruct::applySql(const std::string& sql) {
	if (state_ != LifecycleState::Active) {
		throw IllegalArgumentException("热插表需要机器人已上电常驻（active() 之后）；"
			"未上电时请把语句写进结构脚本");
	}
	return hotSql_.apply(sql, deactivateGraceMs_);
}

} // namespace bot
