#pragma once

#include <string>

// AF_UNIX 请求-响应服务：mybot 侧连上来发 "range\n"，仿真器用 Robot::queryRange
// 算出前方障碍距离后回传 "OK <mm>\n"。与 CommandBridge / CaptureBridge 分开第三
// 条 socket——测距是同步查询（要等结果），语义上既不是即发即忘的命令、也不是
// 带二进制体的拍照。
//
// 协议：
//   请求: range\n
//   响应: OK <distance_mm>\n   （整数毫米，无命中时返回 maxRange 对应的毫米值）
//         ERR <reason>\n
//
// 非阻塞收、带 deadline 的阻塞式发，与 CaptureBridge 同一套防护。
class SensorBridge {
public:
    explicit SensorBridge(const std::string& socketPath);
    ~SensorBridge();

    SensorBridge(const SensorBridge&) = delete;
    SensorBridge& operator=(const SensorBridge&) = delete;

    bool listening() const { return listenFd_ >= 0; }
    const std::string& path() const { return path_; }

    // 每帧调一次：接受连接、收请求。返回本帧是否有待处理的 range 请求。
    bool poll();

    // 回传测距结果（毫米）。
    bool sendRange(int distanceMm);
    bool sendError(const char* reason);

private:
    void acceptPending();
    void readPending();
    void handleLine(const std::string& line);
    void dropClient();
    bool sendAll(const void* data, std::size_t len);

    std::string path_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string buffer_;
    bool requestPending_ = false;
};
