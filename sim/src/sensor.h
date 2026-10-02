#pragma once

#include "unix_line_server.h"

// AF_UNIX 请求-响应服务：mybot 侧连上来发 "range\n"，仿真器用测距逻辑
// 算出前方障碍距离后回传 "OK <mm>\n"。与 CommandBridge / CaptureBridge 分开第三
// 条 socket——测距是同步查询（要等结果），语义上既不是即发即忘的命令、也不是
// 带二进制体的拍照。
//
// 协议：
//   请求: range\n
//   响应: OK <distance_mm>\n   （整数毫米，无命中时返回 maxRange 对应的毫米值）
//         ERR <reason>\n
class SensorBridge : public UnixLineServer {
public:
    explicit SensorBridge(const std::string& socketPath);

    // 遮蔽基类 poll：先走公共骨架，再报告本帧是否有待处理的 range 请求。
    bool poll();

    // 回传测距结果（毫米）。
    bool sendRange(int distanceMm);
    bool sendError(const char* reason);

private:
    void handleLine(const std::string& line) override;
    void onClientGone() override;

    bool requestPending_ = false;
};
