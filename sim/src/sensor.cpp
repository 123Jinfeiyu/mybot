#include "sensor.h"

#include <cstdio>
#include <cstring>

SensorBridge::SensorBridge(const std::string& socketPath)
    : UnixLineServer(socketPath, "sensor", 1024) {}

void SensorBridge::onClientGone() {
    requestPending_ = false;
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
    UnixLineServer::poll();
    return requestPending_ && hasClient();
}

bool SensorBridge::sendRange(int distanceMm) {
    requestPending_ = false;
    if (!hasClient()) {
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
