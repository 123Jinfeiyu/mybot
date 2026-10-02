#pragma once

#include <string>

#include "unix_line_server.h"

// mybot 下发的语义动作。持续型（forward/backward/left/right/stop）保存在桥的
// 状态里由 command()/speed() 读出；一次性（fdist/turn/pick/place）也共用同一
// 枚举，靠主循环按 seq() 边沿触发。
enum class SimCommand {
    Stop,
    Forward,
    Backward,
    Left,
    Right,
    FwdDist,   // forward N meters: param() = meters (positive)
    Turn,      // turn N degrees: param() = degrees (positive=left, negative=right)
    Pick,      // pick up obstacle in front
    Place,     // place carried obstacle in front
};

const char* commandName(SimCommand cmd);

// AF_UNIX 行文本服务端。mybot 侧 modules/Sim.lua 每发一条动作写一行：
//   forward | backward | left | right | stop   [速度 0.0~1.0]
//   fdist <meters>
//   turn <degrees>
// 例如 "forward 0.8"、"fdist 2.0"、"turn 90"。
// 选行文本是为了能用 socat 直接手测：
//   socat - UNIX-CONNECT:/tmp/mybot-sim.sock
//
// 非阻塞、无独立线程：主渲染循环每帧 poll() 一次。同一帧内到达多条指令时
// 以最后一条为准（动作是持续型状态，中间态没有渲染意义）。
class CommandBridge : public UnixLineServer {
public:
    explicit CommandBridge(const std::string& socketPath);

    SimCommand command() const { return command_; }
    float speed() const { return speed_; }
    // fdist/turn 的参数值；其他指令返回 0。
    float param() const { return param_; }

    // 已接受指令的序号，每解析成功一条自增。一次性指令（fdist/turn/pick/place）
    // 同枚举重复下发也必须重新触发，主循环按「序号变化」而非「枚举变化」取边沿。
    unsigned seq() const { return seq_; }

    // 最近一条解析过的原始行，仅用于窗口标题上给人看
    const std::string& lastLine() const { return lastLine_; }

private:
    void handleLine(const std::string& line) override;
    // 控制器走了蜘蛛就该停：mybot 跑完动作脚本退出、或中途崩掉，都表现为连接
    // 断开。让一只没人管的蜘蛛继续走比让它站住危险得多。
    void onClientGone() override;

    SimCommand command_ = SimCommand::Stop;
    float speed_ = 1.0f;
    float param_ = 0.0f;
    unsigned seq_ = 0;
    std::string lastLine_;
};
