#pragma once

#include <string>

// mybot 下发的语义动作。刻意只有「走哪个方向 / 停」这一档：
// 上游不发关节角，蜘蛛的腿部摆动由 Spider 自己按身体速度生成。
enum class SimCommand {
    Stop,
    Forward,
    Backward,
    Left,
    Right,
};

const char* commandName(SimCommand cmd);

// AF_UNIX 行文本服务端。mybot 侧 modules/Sim.lua 每发一条动作写一行：
//   forward | backward | left | right | stop   [速度 0.0~1.0]
// 例如 "forward 0.8"。选行文本是为了能用 socat 直接手测：
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
    std::string lastLine_;
};
