#include "capture.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

// 一行请求最长也就 "capture\n" 几字节；攒够这么多还没换行说明对端在灌垃圾。
constexpr std::size_t kMaxBufferBytes = 1024;

// sun_path 是定长数组，超长会被静默截断成另一个路径，必须显式拒绝。
constexpr std::size_t kMaxPathLen = sizeof(sockaddr_un::sun_path) - 1;

// 发送侧绝对 deadline：一张 JPEG 撑死几百 KB，正常几毫秒就发完；给 3 秒纯粹是
// 兜住「对端连上却不读」的卡死，超时就断开，绝不无限阻塞渲染循环。
constexpr int kSendDeadlineMs = 3000;

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

CaptureBridge::CaptureBridge(const std::string& socketPath) : path_(socketPath) {
    if (path_.size() > kMaxPathLen) {
        std::fprintf(stderr, "capture: socket path too long (max %zu bytes): %s\n", kMaxPathLen,
                     path_.c_str());
        return;
    }

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::fprintf(stderr, "capture: socket() failed: %s\n", std::strerror(errno));
        return;
    }

    ::unlink(path_.c_str());  // 上次异常退出留下的 inode 会导致 EADDRINUSE

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path_.c_str(), path_.size());

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "capture: bind(%s) failed: %s\n", path_.c_str(), std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }

    if (::listen(listenFd_, 4) != 0) {
        std::fprintf(stderr, "capture: listen() failed: %s\n", std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        ::unlink(path_.c_str());
        return;
    }

    if (!setNonBlocking(listenFd_)) {
        std::fprintf(stderr, "capture: cannot set listen socket non-blocking: %s\n",
                     std::strerror(errno));
    }
}

CaptureBridge::~CaptureBridge() {
    dropClient();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
    }
}

void CaptureBridge::dropClient() {
    if (clientFd_ >= 0) {
        ::close(clientFd_);
        clientFd_ = -1;
    }
    buffer_.clear();
    requestPending_ = false;
}

void CaptureBridge::acceptPending() {
    if (listenFd_ < 0) {
        return;
    }
    for (;;) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::fprintf(stderr, "capture: accept() failed: %s\n", std::strerror(errno));
            }
            return;
        }
        setNonBlocking(fd);  // recv 侧必须非阻塞，否则无请求时卡死渲染循环
        // 新连接顶掉旧的：Camera.lua 每次拍照可能重连，旧 fd 多半是上次残骸
        dropClient();
        clientFd_ = fd;
    }
}

void CaptureBridge::readPending() {
    if (clientFd_ < 0) {
        return;
    }
    for (;;) {
        char chunk[256];
        const ssize_t n = ::recv(clientFd_, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            if (buffer_.size() > kMaxBufferBytes) {
                std::fprintf(stderr, "capture: client exceeded %zu bytes without newline, dropping\n",
                             kMaxBufferBytes);
                dropClient();
                return;
            }
            continue;
        }
        if (n == 0) {
            dropClient();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::fprintf(stderr, "capture: recv() failed: %s\n", std::strerror(errno));
            dropClient();
        }
        return;
    }
}

void CaptureBridge::handleLine(const std::string& rawLine) {
    const char* text = rawLine.c_str();
    while (*text == ' ' || *text == '\t' || *text == '\r') {
        ++text;
    }
    if (*text == '\0') {
        return;
    }
    if (std::strcmp(text, "capture") == 0) {
        requestPending_ = true;
        return;
    }
    std::fprintf(stderr, "capture: unknown request '%s' (ignored)\n", text);
}

bool CaptureBridge::poll() {
    acceptPending();
    if (clientFd_ < 0) {
        return false;
    }
    readPending();

    std::size_t start = 0;
    for (std::size_t i = 0; i < buffer_.size(); ++i) {
        if (buffer_[i] != '\n') {
            continue;
        }
        handleLine(buffer_.substr(start, i - start));
        start = i + 1;
    }
    buffer_.erase(0, start);

    return requestPending_ && clientFd_ >= 0;
}

bool CaptureBridge::sendAll(const void* data, std::size_t len) {
    if (clientFd_ < 0) {
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kSendDeadlineMs);
    const unsigned char* ptr = static_cast<const unsigned char*>(data);
    std::size_t off = 0;
    while (off < len) {
        // MSG_NOSIGNAL：对端已关时 send 触发 SIGPIPE 会直接杀掉仿真器进程
        const ssize_t n = ::send(clientFd_, ptr + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "capture: send timed out after %d ms, dropping client\n",
                             kSendDeadlineMs);
                dropClient();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        std::fprintf(stderr, "capture: send() failed: %s\n", std::strerror(errno));
        dropClient();
        return false;
    }
    return true;
}

bool CaptureBridge::sendFrame(const unsigned char* jpeg, std::size_t len, int width, int height) {
    requestPending_ = false;
    if (clientFd_ < 0) {
        return false;
    }
    char header[96];
    const int hlen = std::snprintf(header, sizeof(header), "OK jpeg %zu %d %d\n", len, width, height);
    if (hlen <= 0 || static_cast<std::size_t>(hlen) >= sizeof(header)) {
        std::fprintf(stderr, "capture: header formatting failed\n");
        dropClient();
        return false;
    }
    if (!sendAll(header, static_cast<std::size_t>(hlen))) {
        return false;
    }
    return sendAll(jpeg, len);
}

bool CaptureBridge::sendError(const char* reason) {
    requestPending_ = false;
    if (clientFd_ < 0) {
        return false;
    }
    char msg[256];
    const int n = std::snprintf(msg, sizeof(msg), "ERR %s\n", reason ? reason : "unknown");
    if (n <= 0) {
        dropClient();
        return false;
    }
    return sendAll(msg, static_cast<std::size_t>(n));
}
