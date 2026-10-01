#pragma once

#include <cstddef>
#include <string>

// AF_UNIX 请求-响应服务：mybot 侧 modules/Camera.lua 连上来发一行 "capture\n"，
// 仿真器抓当前帧、编码成 JPEG 回传。刻意与 CommandBridge 分成两条 socket：
//   1) 语义不同——命令是持续型即发即忘（无回执），拍照是一问一答带二进制体；
//   2) Sim.lua 与 Camera.lua 是两个独立驱动实例（各自的 lua_State / 各自的 fd），
//      物理上无法共用一条连接。
//
// 响应帧格式（先文本头一行，后紧跟二进制体，便于 socat 手测头部）：
//   OK jpeg <len> <w> <h>\n  之后是 <len> 字节 JPEG
//   ERR <reason>\n
//
// 非阻塞收、带 deadline 的阻塞式发：主渲染循环每帧 poll() 一次，recv 绝不能阻塞
// （否则没请求时整个画面卡死）；而一张 JPEG 可能几百 KB，超过 socket 发送缓冲，
// 非阻塞 send 会 EAGAIN 截断——故发送侧走「MSG_DONTWAIT + 短拍休眠 + 绝对
// deadline」，与 mybot C++ 侧 sendFrame 同一套（AF_UNIX 在对端缓冲满时仍报可写，
// poll(POLLOUT) 恒就绪，靠不住）。
class CaptureBridge {
public:
    explicit CaptureBridge(const std::string& socketPath);
    ~CaptureBridge();

    CaptureBridge(const CaptureBridge&) = delete;
    CaptureBridge& operator=(const CaptureBridge&) = delete;

    bool listening() const { return listenFd_ >= 0; }
    const std::string& path() const { return path_; }

    // 每帧调一次：接受连接、收请求行。返回本帧是否有待处理的 capture 请求。
    // 主循环据此决定要不要抓帧回传。
    bool poll();

    // 主循环渲染完、抓到帧后回传。jpeg 指向 len 字节。成功返回 true；对端已断
    // 或发送超时返回 false（并已 dropClient）。回传后清除请求标志。
    bool sendFrame(const unsigned char* jpeg, std::size_t len, int width, int height);

    // 抓帧/编码失败时回一条 ERR。同样清除请求标志。
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
