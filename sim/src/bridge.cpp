#include "bridge.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// 一帧内攒下的未分行字节上限。mybot 一行指令最长也就几十字节，超限说明对端
// 在灌垃圾，直接断开而不是无界吃内存。
constexpr std::size_t kMaxBufferBytes = 4096;

// sun_path 是定长数组，超长会被静默截断成另一个路径，必须显式拒绝
constexpr std::size_t kMaxPathLen = sizeof(sockaddr_un::sun_path) - 1;

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

float parseSpeed(const char* text, bool& ok) {
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    ok = (end != text) && (*end == '\0');
    if (!ok) {
        return 1.0f;
    }
    if (value < 0.0f) {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

}  // namespace

const char* commandName(SimCommand cmd) {
    switch (cmd) {
        case SimCommand::Forward:
            return "forward";
        case SimCommand::Backward:
            return "backward";
        case SimCommand::Left:
            return "left";
        case SimCommand::Right:
            return "right";
        case SimCommand::Stop:
            return "stop";
    }
    return "stop";
}

CommandBridge::CommandBridge(const std::string& socketPath) : path_(socketPath) {
    if (path_.size() > kMaxPathLen) {
        std::fprintf(stderr, "bridge: socket path too long (max %zu bytes): %s\n", kMaxPathLen,
                     path_.c_str());
        return;
    }

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::fprintf(stderr, "bridge: socket() failed: %s\n", std::strerror(errno));
        return;
    }

    // 上次异常退出会留下 inode，不先摘掉 bind 就 EADDRINUSE
    ::unlink(path_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path_.c_str(), path_.size());

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "bridge: bind(%s) failed: %s\n", path_.c_str(), std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }

    if (::listen(listenFd_, 4) != 0) {
        std::fprintf(stderr, "bridge: listen() failed: %s\n", std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        ::unlink(path_.c_str());
        return;
    }

    if (!setNonBlocking(listenFd_)) {
        std::fprintf(stderr, "bridge: cannot set listen socket non-blocking: %s\n",
                     std::strerror(errno));
    }
}

CommandBridge::~CommandBridge() {
    dropClient();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
    }
}

void CommandBridge::dropClient() {
    if (clientFd_ >= 0) {
        ::close(clientFd_);
        clientFd_ = -1;
    }
    buffer_.clear();
    // 控制器走了蜘蛛就该停：mybot 跑完动作脚本退出、或中途崩掉，都表现为连接
    // 断开。让一只没人管的蜘蛛继续走比让它站住危险得多。
    command_ = SimCommand::Stop;
    speed_ = 1.0f;
}

void CommandBridge::acceptPending() {
    if (listenFd_ < 0) {
        return;
    }
    for (;;) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::fprintf(stderr, "bridge: accept() failed: %s\n", std::strerror(errno));
            }
            return;
        }
        setNonBlocking(fd);
        // 已有连接就让位给新的：mybot 每次跑动作脚本都重连，旧的多半是上一次的残骸
        dropClient();
        clientFd_ = fd;
    }
}

void CommandBridge::readPending() {
    if (clientFd_ < 0) {
        return;
    }
    for (;;) {
        char chunk[256];
        const ssize_t n = ::recv(clientFd_, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            if (buffer_.size() > kMaxBufferBytes) {
                std::fprintf(stderr, "bridge: client exceeded %zu bytes without newline, dropping\n",
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
            std::fprintf(stderr, "bridge: recv() failed: %s\n", std::strerror(errno));
            dropClient();
        }
        return;
    }
}

void CommandBridge::handleLine(const std::string& rawLine) {
    const char* text = rawLine.c_str();
    while (*text == ' ' || *text == '\t' || *text == '\r') {
        ++text;
    }
    if (*text == '\0') {
        return;
    }

    char word[32] = {};
    char rest[32] = {};
    const int fields = std::sscanf(text, "%31s %31s", word, rest);
    if (fields < 1) {
        return;
    }

    SimCommand parsed = SimCommand::Stop;
    if (std::strcmp(word, "forward") == 0) {
        parsed = SimCommand::Forward;
    } else if (std::strcmp(word, "backward") == 0 || std::strcmp(word, "back") == 0) {
        parsed = SimCommand::Backward;
    } else if (std::strcmp(word, "left") == 0) {
        parsed = SimCommand::Left;
    } else if (std::strcmp(word, "right") == 0) {
        parsed = SimCommand::Right;
    } else if (std::strcmp(word, "stop") == 0) {
        parsed = SimCommand::Stop;
    } else {
        // 认不出的指令不改状态：动作是持续型的，被一行垃圾打成 stop 会让蜘蛛
        // 莫名其妙停在半路。只报出来给人看。
        std::fprintf(stderr, "bridge: unknown command '%s' (ignored)\n", word);
        lastLine_ = rawLine;
        return;
    }

    float speed = 1.0f;
    if (fields >= 2 && rest[0] != '\0') {
        bool ok = false;
        speed = parseSpeed(rest, ok);
        if (!ok) {
            std::fprintf(stderr, "bridge: bad speed '%s' in '%s' (using 1.0)\n", rest,
                         rawLine.c_str());
        }
    }

    command_ = parsed;
    speed_ = speed;
    lastLine_ = rawLine;
}

void CommandBridge::poll() {
    acceptPending();

    if (clientFd_ < 0) {
        return;
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
}
