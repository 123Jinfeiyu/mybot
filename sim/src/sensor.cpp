#include "sensor.h"

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

constexpr std::size_t kMaxBufferBytes = 1024;
constexpr std::size_t kMaxPathLen = sizeof(sockaddr_un::sun_path) - 1;
constexpr int kSendDeadlineMs = 2000;

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

SensorBridge::SensorBridge(const std::string& socketPath) : path_(socketPath) {
    if (path_.size() > kMaxPathLen) {
        std::fprintf(stderr, "sensor: socket path too long (max %zu bytes): %s\n", kMaxPathLen,
                     path_.c_str());
        return;
    }

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::fprintf(stderr, "sensor: socket() failed: %s\n", std::strerror(errno));
        return;
    }

    ::unlink(path_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path_.c_str(), path_.size());

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "sensor: bind(%s) failed: %s\n", path_.c_str(), std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }

    if (::listen(listenFd_, 4) != 0) {
        std::fprintf(stderr, "sensor: listen() failed: %s\n", std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        ::unlink(path_.c_str());
        return;
    }

    if (!setNonBlocking(listenFd_)) {
        std::fprintf(stderr, "sensor: cannot set listen socket non-blocking: %s\n",
                     std::strerror(errno));
    }
}

SensorBridge::~SensorBridge() {
    dropClient();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
    }
}

void SensorBridge::dropClient() {
    if (clientFd_ >= 0) {
        ::close(clientFd_);
        clientFd_ = -1;
    }
    buffer_.clear();
    requestPending_ = false;
}

void SensorBridge::acceptPending() {
    if (listenFd_ < 0) {
        return;
    }
    for (;;) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::fprintf(stderr, "sensor: accept() failed: %s\n", std::strerror(errno));
            }
            return;
        }
        setNonBlocking(fd);
        dropClient();
        clientFd_ = fd;
    }
}

void SensorBridge::readPending() {
    if (clientFd_ < 0) {
        return;
    }
    for (;;) {
        char chunk[256];
        const ssize_t n = ::recv(clientFd_, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            if (buffer_.size() > kMaxBufferBytes) {
                std::fprintf(stderr, "sensor: client exceeded %zu bytes without newline, dropping\n",
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
            std::fprintf(stderr, "sensor: recv() failed: %s\n", std::strerror(errno));
            dropClient();
        }
        return;
    }
}

void SensorBridge::handleLine(const std::string& rawLine) {
    const char* text = rawLine.c_str();
    while (*text == ' ' || *text == '\t' || *text == '\r') {
        ++text;
    }
    if (*text == '\0') {
        return;
    }
    if (std::strcmp(text, "range") == 0) {
        requestPending_ = true;
        return;
    }
    std::fprintf(stderr, "sensor: unknown request '%s' (ignored)\n", text);
}

bool SensorBridge::poll() {
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

bool SensorBridge::sendAll(const void* data, std::size_t len) {
    if (clientFd_ < 0) {
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kSendDeadlineMs);
    const unsigned char* ptr = static_cast<const unsigned char*>(data);
    std::size_t off = 0;
    while (off < len) {
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
                std::fprintf(stderr, "sensor: send timed out after %d ms, dropping client\n",
                             kSendDeadlineMs);
                dropClient();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        std::fprintf(stderr, "sensor: send() failed: %s\n", std::strerror(errno));
        dropClient();
        return false;
    }
    return true;
}

bool SensorBridge::sendRange(int distanceMm) {
    requestPending_ = false;
    if (clientFd_ < 0) {
        return false;
    }
    char msg[64];
    const int n = std::snprintf(msg, sizeof(msg), "OK %d\n", distanceMm);
    if (n <= 0) {
        dropClient();
        return false;
    }
    return sendAll(msg, static_cast<std::size_t>(n));
}

bool SensorBridge::sendError(const char* reason) {
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
