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
	/* SAN-4（2026-09-22）：这一次调用**是否真的被派发给了工具**。
	 * false = 根本没执行（策略拒绝 / 未知工具 / 参数错误），因此**副作用必然未发生**；
	 * true（缺省）= 执行过，可能产生副作用——哪怕 ok=false（工具内失败、脚本跑了一半）
	 * 也一样，那种情况机器人可能停在半途，必须如实告知模型。
	 * 用途单点：TaskBudget::admit 据它决定"要不要说那句副作用已发生"。
	 * 为什么不用 ok 代替：`ok` 把"没执行"与"执行了但失败"混在一起，而这两件事
	 * 对模型的意义完全相反。 */
	bool executed = true;
	/* SAN-8②（2026-09-22）：这一次失败是**参数问题**（缺参数/类型不符/越界），
	 * 而不是执行问题。判据由工具层按**异常类型**给出（IllegalArgumentException），
	 * 派发漏斗据此把回执改成「工具参数错误: …」并把 executed 置 false——
	 * 模型看到的应当是"没执行、参数不对"，而不是"脚本执行失败、机器人可能停在半途"。 */
	bool paramError = false;
	/* 带构造函数（非聚合）：存量代码大量使用两字段初始化 {text, img}，聚合
	 * 形态下它们会触发 -Wmissing-field-initializers（本仓库 -Wall -Wextra
	 * 零告警基线）；构造函数让两字段形态显式表示「ok=true 缺省」。 */
	AgentToolResult() = default;
	AgentToolResult(std::string t, std::string img, bool ok_ = true, std::string err_ = "",
			bool executed_ = true, bool paramError_ = false)
		: text(std::move(t)), imageDataUrl(std::move(img)), ok(ok_), error(std::move(err_)),
		  executed(executed_), paramError(paramError_) {}
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
	/* 热插表（2026-09-23）：true = 注册 apply_sql（结构 SQL 带电生效）。
	 * 与 allowShell 同一条门控链（--agent-hot-sql，配置文件不能开）——
	 * 它同样让「模型的一次回复」直接改变带电的硬件结构。只读的 query_sql
	 * 无条件注册，不受这里影响。 */
	bool allowHotSql = false;
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
	/* ---- H1：模拟图片白名单的两个独立输入 ----
	 * 此前 src_root（"允许读哪个目录"）是由 cameraSrc 自己 `canonical` 后取
	 * parent_path 推导的 —— 而 src 的所在目录当然包含 src，判据**恒真**，
	 * 等于没有白名单：一份落在 CWD 的 ./mybot.llm.json 写 camera_src=/etc/shadow
	 * 就能把它 base64 读进模型上下文，再由同一份配置可控的 endpoint 外发；
	 * 而 allow_camera 缺省为真、cam0 自动注入，**不需要 --agent-shell**。
	 * 现在两者解耦：
	 *   cameraSrcRoot  = 显式根目录（--camera-src-dir，**只能命令行给**）；
	 *   cameraSrcFromCli = cameraSrc 是否来自命令行。只有命令行来源才允许用
	 *                     "src 的所在目录"当根（操作者自己指的文件，他本就能读，
	 *                     这也保住了 --camera-src 的快速上手路径）；配置文件来源
	 *                     一律不推导，交由驱动的 BOT4CJ_CAMERA_SRC_DIR 判定，
	 *                     两者都没有即驱动侧"未放行"拒绝。不推导、不猜、不回落 "."。 */
	bool cameraSrcFromCli = false;
	std::string cameraSrcRoot;
};

/* 构造标准工具集：list_action_scripts / run_action_script / list_hardware / query_sql，
 * （camera 非空时）capture_image，（allowHotSql 时）apply_sql，
 * 以及（allowShell 时）run_command。 */
AgentToolRegistry makeAgentTools(const AgentToolContext& ctx);

/* ---- 供工具与系统提示词共用的描述函数 ---- */

/* 动作脚本清单：递归收集 scriptsDir 下的 *.mt，输出「相对路径 — 首行注释」。
 * 目录不存在时返回一行说明而非抛（提示词里如实告知模型即可）。 */
std::string describeActionScripts(const std::string& scriptsDir);

/* 已注册硬件变量清单。只读名字与种类，绝不调用 read()/play()
 * （对舵机等执行器，play({}) 会真的下发指令）。 */
std::string describeHardware(RobotStruct& robot);

/* 解析一条结构 SQL 并渲染成文本（REPL /sql 与 agent 工具 query_sql 共用的
 * 单一事实源）。只读、无副作用：不装配硬件、不改结构矩阵。
 *   * SELECT * FROM t —— 从运行时结构矩阵（robot.matrices()）取数渲染；
 *   * CREATE/INSERT —— 只解析回显（语法验证），不落任何状态。
 * 失败返回"错误: …"前缀的人话报错（解析器带行列定位）。 */
std::string runSqlStatement(const std::string& sql, const RobotStruct& robot);

/* REPL `/sql` 的完整语义（2026-09-23 热插表）：整批都是 SELECT 时走上面的
 * 只读渲染；只要含 CREATE/INSERT，就交给 RobotStruct::applySql **真正带电生效**
 * （装配 + 上电，失败全回滚），返回其生效摘要。
 * 与 query_sql 的分工是刻意的：模型那条路只读（工具一旦能改结构，就绕过了
 * 确认与审计面），这条路是操作者本人坐在终端上敲的。失败同样返回"错误: …"。 */
std::string runSqlCommand(const std::string& sql, RobotStruct& robot);

/* 动作脚本路径解析：请求必须是 scriptsDir 之下的相对路径。
 * 拒绝绝对路径、空段、"."/".."、以及含 NUL 的名字（NUL 会让
 * fopen 截断路径，是「校验的路径 ≠ 打开的路径」经典绕过）。 */
std::string resolveActionScriptPath(const std::string& scriptsDir, const std::string& requested);

} // namespace agent
} // namespace bot
