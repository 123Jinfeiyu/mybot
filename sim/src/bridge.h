#pragma once

#include <string>

// mybot 下发的语义动作。持续型（forward/backward/left/right/stop）和一次性
// 相对运动（fdist/turn）共用同一枚举。相对运动带参数，由 param() 返回。
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
class CommandBridge {
public:
    explicit CommandBridge(const std::string& socketPath);
    ~CommandBridge();

    CommandBridge(const CommandBridge&) = delete;
    CommandBridge& operator=(const CommandBridge&) = delete;

    bool listening() const { return listenFd_ >= 0; }
    const std::string& path() const { return path_; }

    void poll();

    SimCommand command() const { return command_; }
    float speed() const { return speed_; }
    // fdist/turn 的参数值；其他指令返回 0。
    float param() const { return param_; }

    // 最近一条解析过的原始行，仅用于窗口标题上给人看
    const std::string& lastLine() const { return lastLine_; }

private:
    void acceptPending();
    void readPending();
    void handleLine(const std::string& line);
    void dropClient();

    std::string path_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string buffer_;
    SimCommand command_ = SimCommand::Stop;
    float speed_ = 1.0f;
    float param_ = 0.0f;
    std::string lastLine_;
    bool clientEof_ = false;  // recv() 返回 0 时置 true，buffer 处理完再断开
};
