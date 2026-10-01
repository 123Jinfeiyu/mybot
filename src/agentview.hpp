#pragma once
/* agentview.hpp —— 只读视图族（agentview.cpp 的对外契约）
 *
 * 2026-10-01 自 agenttools.hpp 析出（行为保持，robot.hpp A2 窄头模式）：
 * 可读变量清单判据与回读工具的渲染/描述归 agentview.cpp 一个 TU，消费者
 * （agenttools/agenttools_wait/agentmode/mcpbackend）经本头引用。
 * 工具装配契约（AgentToolRegistry/makeAgentTools 等）仍在 agenttools.hpp。 */

#include <cstddef>
#include <string>
#include <vector>

namespace bot {

class RobotStruct;

namespace agent {

/* ---- 发现 2 的 A 步（2026-09-23）：可读变量清单与回读工具 ----
 * 动机：D 步只让模型看见"动作脚本顺带读到的值"，它仍无法**主动**读。而 agent
 * 无法靠"种类"区分传感器与执行器——`Servo.lua` / `Relay.lua` 同样是 `kind=lua`，
 * 对它们调 `play()` 会真驱动硬件（Relay 的 `call({})` 会把 delay 转发给 controller）。
 * 故可读集合由**操作者显式声明**（`--read-var`，可重复，命令行独占；配置文件写
 * `read_vars` 会被拒绝加载，与 `--agent-shell` / `camera_src_dir` 同判据）。
 * 清单为空 = **不注册** `read_variables`（缺省即无此能力）。 */
inline constexpr std::size_t kMaxReadResultBytes = 8u << 10;

/* 解析声明清单 → 当前机器人上**真正可读**的变量：必须已注册，且种类为 `lua`
 * （驱动）或 `daemon`（守护快照——读的是已发布的采样，无 I2C 流量、无副作用）。
 * 其余（别名 / 数值变量 / 名字不存在）一律跳过：既不因一个拼错的名字让整条能力
 * 失效，也不放行"种类不对"的变量（`playQuiet` 对非 lua/daemon 会退化成 `play()`，
 * 与 `capture_image` 的种类防线同一理由）。@p skipped 非空时收集被跳过的名字，
 * 供启动时如实告知（不静默降级）。 */
std::vector<std::string> resolvableReadVars(const std::vector<std::string>& requested,
		RobotStruct& robot, std::vector<std::string>* skipped = nullptr);

/* 渲染回读工具的回执：截断（`kMaxReadResultBytes`）+ UTF-8 消毒 +
 * 「数据非指令」定界（单点，便于离线断言）。 */
std::string formatReadResult(const std::string& body);

/* 系统提示词里的可读变量清单（已解析；空 = 不列）。与工具注册同一解析函数，
 * 两处不会漂移。 */
std::string describeReadableVars(const std::vector<std::string>& requested, RobotStruct& robot);

} // namespace agent
} // namespace bot
