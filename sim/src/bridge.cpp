#include "bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim_config.h"

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
    }
    return "stop";
}

CommandBridge::CommandBridge(const std::string& socketPath)
    : UnixLineServer(socketPath, "bridge", 4096) {}

void CommandBridge::onClientGone() {
    command_ = SimCommand::Stop;
    speed_ = 1.0f;
    events_.clear();
}

bool CommandBridge::pollEvent(SimEvent& out) {
    if (events_.empty()) {
        return false;
    }
    out = events_.front();
    events_.erase(events_.begin());
    return true;
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

    // 分类：持续型改状态、一次性入队、认不出的什么都不改——动作是持续型的，
    // 被一行垃圾打成 stop 会让蜘蛛莫名其妙停在半路，只报出来给人看。
    SimCommand cont = SimCommand::Stop;
    SimEvent ev{};
    bool oneShot = false;

    if (std::strcmp(word, "forward") == 0) {
        cont = SimCommand::Forward;
    } else if (std::strcmp(word, "backward") == 0 || std::strcmp(word, "back") == 0) {
        cont = SimCommand::Backward;
    } else if (std::strcmp(word, "left") == 0) {
        cont = SimCommand::Left;
    } else if (std::strcmp(word, "right") == 0) {
        cont = SimCommand::Right;
    } else if (std::strcmp(word, "stop") == 0) {
        cont = SimCommand::Stop;
    } else if (std::strcmp(word, "fdist") == 0) {
        oneShot = true;
        ev.type = SimEvent::Type::FwdDist;
    } else if (std::strcmp(word, "turn") == 0) {
        oneShot = true;
        ev.type = SimEvent::Type::Turn;
    } else if (std::strcmp(word, "pick") == 0) {
        oneShot = true;
        ev.type = SimEvent::Type::Pick;
    } else if (std::strcmp(word, "place") == 0) {
        oneShot = true;
        ev.type = SimEvent::Type::Place;
    } else if (std::strcmp(word, "save") == 0) {
        oneShot = true;
        ev.type = SimEvent::Type::Save;
    } else {
        std::fprintf(stderr, "bridge: unknown command '%s' (ignored)\n", word);
        lastLine_ = rawLine;
        return;
    }

    if (oneShot) {
        // fdist/turn 需要数值参数；pick/place 不带参数，多余字段忽略
        if (ev.type == SimEvent::Type::FwdDist || ev.type == SimEvent::Type::Turn) {
            if (fields < 2 || rest[0] == '\0') {
                std::fprintf(stderr, "bridge: %s requires a parameter\n", word);
                return;
            }
            char* end = nullptr;
            ev.param = std::strtof(rest, &end);
            if (end == rest || *end != '\0') {
                std::fprintf(stderr, "bridge: bad param '%s' in '%s' (ignored)\n", rest,
                             rawLine.c_str());
                return;
            }
        }
        if (events_.size() >= SimConfig::kMaxQueuedEvents) {
            std::fprintf(stderr, "bridge: event queue full (%zu), dropping '%s'\n",
                         SimConfig::kMaxQueuedEvents, word);
            lastLine_ = rawLine;
            return;
        }
        events_.push_back(ev);
    } else {
        float speed = 1.0f;
        if (fields >= 2 && rest[0] != '\0') {
            bool ok = false;
            speed = parseSpeed(rest, ok);
            if (!ok) {
                std::fprintf(stderr, "bridge: bad speed '%s' in '%s' (using 1.0)\n", rest,
                             rawLine.c_str());
            }
        }
        command_ = cont;
        speed_ = speed;
    }

    lastLine_ = rawLine;
}
