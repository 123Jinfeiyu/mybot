#include "action_table.h"

#include <cstring>

// 动作表：持续型行的 oneShot、一次性行的 continuous 字段不会被读取，填中性占位。
const ActionDef kActions[] = {
    // code  wireName      alias      kind                        needsParam  continuous              oneShot
    {0, "stop", nullptr, ActionKind::Continuous, false, SimCommand::Stop, SimEvent::Type::Pick},
    {1, "forward", nullptr, ActionKind::Continuous, false, SimCommand::Forward, SimEvent::Type::Pick},
    {2, "backward", "back", ActionKind::Continuous, false, SimCommand::Backward, SimEvent::Type::Pick},
    {3, "left", nullptr, ActionKind::Continuous, false, SimCommand::Left, SimEvent::Type::Pick},
    {4, "right", nullptr, ActionKind::Continuous, false, SimCommand::Right, SimEvent::Type::Pick},
    {5, "fdist", nullptr, ActionKind::OneShot, true, SimCommand::Stop, SimEvent::Type::FwdDist},
    {6, "turn", nullptr, ActionKind::OneShot, true, SimCommand::Stop, SimEvent::Type::Turn},
    {7, "pick", nullptr, ActionKind::OneShot, false, SimCommand::Stop, SimEvent::Type::Pick},
    {8, "place", nullptr, ActionKind::OneShot, false, SimCommand::Stop, SimEvent::Type::Place},
    {9, "save", nullptr, ActionKind::OneShot, false, SimCommand::Stop, SimEvent::Type::Save},
};

const int kActionCount = static_cast<int>(sizeof(kActions) / sizeof(kActions[0]));
const int kMaxActionCode = 9;

const ActionDef* findActionByWireName(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < kActionCount; ++i) {
        if (std::strcmp(kActions[i].wireName, name) == 0) {
            return &kActions[i];
        }
        if (kActions[i].alias != nullptr && std::strcmp(kActions[i].alias, name) == 0) {
            return &kActions[i];
        }
    }
    return nullptr;
}

const ActionDef* findActionByCode(int code) {
    for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].code == code) {
            return &kActions[i];
        }
    }
    return nullptr;
}
