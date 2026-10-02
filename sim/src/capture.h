#pragma once

#include <cstddef>

#include "unix_line_server.h"

// AF_UNIX 请求-响应服务：mybot 侧 modules/Camera.lua 连上来发一行 "capture\n"，
// 仿真器抓当前帧、编码成 JPEG 回传。刻意与 CommandBridge 分成两条 socket：
//   1) 语义不同——命令是持续型即发即忘（无回执），拍照是一问一答带二进制体；
//   2) Sim.lua 与 Camera.lua 是两个独立驱动实例（各自的 lua_State / 各自的 fd），
//      物理上无法共用一条连接。
//
// 响应帧格式（先文本头一行，后紧跟二进制体，便于 socat 手测头部）：
//   OK jpeg <len> <w> <h>\n  之后是 <len> 字节 JPEG
//   ERR <reason>\n
class CaptureBridge : public UnixLineServer {
public:
    explicit CaptureBridge(const std::string& socketPath);

    // 遮蔽基类 poll：先走公共骨架（收行/断开管理），再报告本帧是否有待处理的
    // capture 请求——主循环据此决定要不要抓帧回传。
    bool poll();

    // 主循环渲染完、抓到帧后回传。jpeg 指向 len 字节。成功返回 true；对端已断
    // 或发送超时返回 false（并已 dropClient）。回传后清除请求标志。
    bool sendFrame(const unsigned char* jpeg, std::size_t len, int width, int height);

    // 抓帧/编码失败时回一条 ERR。同样清除请求标志。
    bool sendError(const char* reason);

private:
    void handleLine(const std::string& line) override;
    void onClientGone() override;

    bool requestPending_ = false;
};
