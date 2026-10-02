#include "bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

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
        case SimCommand::FwdDist:
            return "fdist";
        case SimCommand::Turn:
            return "turn";
        case SimCommand::Pick:
            return "pick";
        case SimCommand::Place:
            return "place";
    }
    return "stop";
}

CommandBridge::CommandBridge(const std::string& socketPath)
    : UnixLineServer(socketPath, "bridge", 4096) {}

void CommandBridge::onClientGone() {
    command_ = SimCommand::Stop;
    speed_ = 1.0f;
    param_ = 0.0f;
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
    float param = 0.0f;
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
    } else if (std::strcmp(word, "fdist") == 0) {
        parsed = SimCommand::FwdDist;
    } else if (std::strcmp(word, "turn") == 0) {
        parsed = SimCommand::Turn;
    } else if (std::strcmp(word, "pick") == 0) {
        parsed = SimCommand::Pick;
    } else if (std::strcmp(word, "place") == 0) {
        parsed = SimCommand::Place;
    } else {
        // 认不出的指令不改状态：动作是持续型的，被一行垃圾打成 stop 会让蜘蛛
        // 莫名其妙停在半路。只报出来给人看。
        std::fprintf(stderr, "bridge: unknown command '%s' (ignored)\n", word);
        lastLine_ = rawLine;
        return;
    }

    float speed = 1.0f;
    if (fields >= 2 && rest[0] != '\0') {
        if (parsed == SimCommand::FwdDist || parsed == SimCommand::Turn) {
            char* end = nullptr;
            param = std::strtof(rest, &end);
            if (end == rest || *end != '\0') {
                std::fprintf(stderr, "bridge: bad param '%s' in '%s' (ignored)\n", rest,
                             rawLine.c_str());
                return;
            }
        } else {
            bool ok = false;
            speed = parseSpeed(rest, ok);
            if (!ok) {
                std::fprintf(stderr, "bridge: bad speed '%s' in '%s' (using 1.0)\n", rest,
                             rawLine.c_str());
            }
        }
    } else if (parsed == SimCommand::FwdDist || parsed == SimCommand::Turn) {
        std::fprintf(stderr, "bridge: %s requires a parameter\n", word);
        return;
    }

    command_ = parsed;
    speed_ = speed;
    param_ = param;
    ++seq_;
    lastLine_ = rawLine;
}
