#include "capture.h"

#include <cstdio>
#include <cstring>

CaptureBridge::CaptureBridge(const std::string& socketPath)
    : UnixLineServer(socketPath, "capture", 1024) {}

void CaptureBridge::onClientGone() {
    requestPending_ = false;
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
    UnixLineServer::poll();
    return requestPending_ && hasClient();
}

bool CaptureBridge::sendFrame(const unsigned char* jpeg, std::size_t len, int width, int height) {
    requestPending_ = false;
    if (!hasClient()) {
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
    if (!hasClient()) {
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
