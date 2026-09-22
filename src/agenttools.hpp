#pragma once
/* agenttools.hpp —— agent 模式工具层
 *
 * 工具全部作用于「常驻内存的机器人实例」（RobotStruct 已 active 且未 deactivate），
 * 这是 agent 模式相对于管道模式的关键差别：动作脚本一跑完硬件不释放，
 * 智能体可以在多轮工具调用之间保持机器人姿态与总线句柄。
 *
 * 工具层不认识 HTTP/模型：execute(args) 只吃 Json、吐文本，便于离线单测。
 */

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "agentproto.hpp"
#include "json.hpp"

namespace bot {

class RobotStruct;
class MatrixVars_Hardware;

namespace agent {

/* 单张图的 base64 上限（宿主侧封顶，与驱动侧 MAX_SIM_BYTES 各管一段）。
 * 会话历史的字节预算刻意「不封顶单块」（见 agentloop.hpp），而一张照片正是单块的
 * 主项：驱动仿真图允许 16MiB 原始字节（base64 后约 21MB），一次拍照就能击穿
 * 4MiB 历史预算、把请求体顶到常见网关限额之外。故在工具入口先拒一次。 */
constexpr std::size_t kMaxImageBase64Bytes = 2u << 20;

/* 工具执行结果。图片走**独立通道**：OpenAI 协议的 tool 消息只允许文本，
 * 图片必须由工具循环另起一条 user 消息注入（见 agentloop），
 * 这也是 robot_agent 的既有做法。
 * ok/error 供审计轨迹用（agentaudit）：工具异常/未知工具/脚本执行失败置
 * ok=false，文本回传给模型的内容不变——审计只是旁观记账，不改变回执。 */
struct AgentToolResult {
	std::string text;
	std::string imageDataUrl;	/* 非空 = "data:image/jpeg;base64,..." */
	bool ok = true;			/* false = 工具本身失败（参数错/未知工具/异常） */
	std::string error;		/* ok=false 时的人话原因 */
	/* 带构造函数（非聚合）：存量代码大量使用两字段初始化 {text, img}，聚合
	 * 形态下它们会触发 -Wmissing-field-initializers（本仓库 -Wall -Wextra
	 * 零告警基线）；构造函数让两字段形态显式表示「ok=true 缺省」。 */
	AgentToolResult() = default;
	AgentToolResult(std::string t, std::string img, bool ok_ = true, std::string err_ = "")
		: text(std::move(t)), imageDataUrl(std::move(img)), ok(ok_), error(std::move(err_)) {}
};

/* 一个可被模型调用的工具。execute 返回给模型的内容；抛异常表示工具失败
 * （注册表会折成「工具执行失败: ...」文本回传模型，不打断会话）。 */
struct AgentTool {
	std::string name;
	std::string description;
	Json parameters;	/* JSON Schema（object） */
	std::function<AgentToolResult(const Json& args)> execute;
};

class AgentToolRegistry {
	std::vector<AgentTool> tools_;

public:
	void add(AgentTool t) { tools_.push_back(std::move(t)); }
	bool contains(const std::string& name) const;

	/* 派发一次工具调用。未知工具与工具内部异常都折成文本返回
	 * （模型据此换个做法继续，而不是让整轮 agent 会话崩掉）。 */
	AgentToolResult execute(const std::string& name, const Json& args) const;

	std::vector<ToolSpec> specs() const;
};

/* 工具运行环境。robot 非空是前提（agent 模式必带 --robot）。 */
struct AgentToolContext {
	RobotStruct* robot = nullptr;
	std::string scriptsDir = "scripts";	/* 动作脚本根目录（相对路径沙箱边界） */
	std::string structurePath;		/* 结构脚本路径（仅用于工具输出标注） */
	int commandTimeoutSec = 10;		/* run_command 缺省超时 */
	int maxCommandTimeoutSec = 60;		/* run_command 超时上限 */
	bool allowShell = false;		/* true = 注册 run_command（安全阀门，缺省关） */
	/* PL-2（2026-09-22）：动作脚本执行预算（秒）。run_action_script 是本工具集里
	 * **唯一曾无任何上界**的执行路径，而它默认开启；脚本本身是线性矩阵、DSL 无
	 * 循环，挂死只可能来自 Lua 驱动自己的 call()。>0 = 开启（窗口内关 JIT + 挂
	 * 指令计数钩子，见 luaexec.hpp）；<=0 = 明确关闭，退化为此前的无预算行为。 */
	int scriptTimeoutSec = 180;
	/* 摄像头：Lua 驱动变量（modules/Camera.lua）。为空 = 不注册 capture_image。
	 * 抓到的图片经 AgentToolResult.imageDataUrl 进入下一轮对话。 */
	MatrixVars_Hardware* camera = nullptr;
	std::string cameraDevice;	/* 空 = 用驱动缺省（/dev/video0） */
	std::string cameraSrc;		/* 非空 = 模拟图片来源文件（无摄像头也能跑通全链路） */
};

/* 构造标准工具集：list_action_scripts / run_action_script / list_hardware，
 * （camera 非空时）capture_image，以及（allowShell 时）run_command。 */
AgentToolRegistry makeAgentTools(const AgentToolContext& ctx);

/* ---- 供工具与系统提示词共用的描述函数 ---- */

/* 动作脚本清单：递归收集 scriptsDir 下的 *.mt，输出「相对路径 — 首行注释」。
 * 目录不存在时返回一行说明而非抛（提示词里如实告知模型即可）。 */
std::string describeActionScripts(const std::string& scriptsDir);

/* 已注册硬件变量清单。只读名字与种类，绝不调用 read()/play()
 * （对舵机等执行器，play({}) 会真的下发指令）。 */
std::string describeHardware(RobotStruct& robot);

/* 动作脚本路径解析：请求必须是 scriptsDir 之下的相对路径。
 * 拒绝绝对路径、空段、"."/".."、以及含 NUL 的名字（NUL 会让
 * fopen 截断路径，是「校验的路径 ≠ 打开的路径」经典绕过）。 */
std::string resolveActionScriptPath(const std::string& scriptsDir, const std::string& requested);

} // namespace agent
} // namespace bot
