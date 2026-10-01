#pragma once
/* daemonconf.hpp —— @Daemon 配置校验/装配域的对外判据
 *
 * 2026-10-01 自 daemonvar.hpp 析出（行为保持，随 daemonconf.cpp 的 TU 拆分
 * 对齐声明归属）：interval 列解析与 chunk 路径寻址由 daemonconf.cpp 提供，
 * 消费方（daemonvar 线程机 / hw_factory / hotsql_plan / 表驱动单测）经本头
 * 引用。共享槽布局、线程机与守护运行器仍在 daemonvar.hpp。 */

#include <cstdint>
#include <string>

namespace bot {

/* D/C4：解析 Daemon 的 interval 列（毫秒）。非法值（非整数/溢出/负/0/超 1h）
 * 抛 IllegalArgumentException 并点名表名与行号；成功返回毫秒数。 */
int64_t parseInterval(const std::string& iv, const std::string& name,
	const std::string& tabName, int64_t row);

/* ---- chunk 路径寻址（issue #19 多路径；实现 daemonconf.cpp）---- */
std::string firstExistingModule(const std::string& paths, const std::string& rel);
std::string resolveDaemonChunk(const std::string& chunk, const std::string& modulePath);

} // namespace bot
