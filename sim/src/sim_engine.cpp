#include "sim_engine.h"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstring>

SimEngine::SimEngine(std::string scenePath) : scenePath_(std::move(scenePath)) {
    if (!scenePath_.empty()) {
        const char* slash = std::strrchr(scenePath_.c_str(), '/');
        sceneName_ = slash ? slash + 1 : scenePath_;
    } else {
        sceneName_ = "builtin";
    }
}

void SimEngine::reloadScene() {
    if (!scenePath_.empty()) {
        SceneState st;
        if (readScene(scenePath_, st)) {
            world_.applyScene(st);
            robot_ = Robot();
            robot_.setPose(st.robotX, st.robotZ, st.robotTheta);
            std::fprintf(stderr, "scene: 已装载 %s（%zu 个障碍）\n", scenePath_.c_str(),
                         st.obstacles.size());
            return;
        }
        std::fprintf(stderr, "scene: 回落内置默认场景\n");
    }
    world_ = World();
    robot_ = Robot();
}

void SimEngine::saveScene() {
    if (scenePath_.empty()) {
        std::fprintf(stderr, "save: 未配置场景路径（argv[4] 或 MYBOT_SIM_SCENE），无处置写\n");
        return;
    }
    SceneState st;
    st.obstacles = world_.obstacles();
    st.robotX = robot_.x();
    st.robotZ = robot_.z();
    st.robotTheta = robot_.theta();
    st.carriedIdx = world_.isCarrying() ? world_.carriedIndex() : -1;
    if (writeScene(scenePath_, st)) {
        std::fprintf(stderr, "save: 场景已写入 %s（%zu 个障碍）\n", scenePath_.c_str(),
                     st.obstacles.size());
    } else {
        std::fprintf(stderr, "save: 写入 %s 失败\n", scenePath_.c_str());
    }
}

RobotInput SimEngine::commandToInput(SimCommand cmd, float speed) const {
    RobotInput input;
    switch (cmd) {
        case SimCommand::Forward:
            input.throttle = speed;
            break;
        case SimCommand::Backward:
            input.throttle = -speed;
            break;
        case SimCommand::Left:
            input.steer = speed;
            break;
        case SimCommand::Right:
            input.steer = -speed;
            break;
        case SimCommand::Stop:
            break;
    }
    return input;
}

void SimEngine::applyContinuous(SimCommand cmd, float speed) {
    command_ = cmd;
    speed_ = speed;
    if (cmd != lastContinuous_) {
        std::fprintf(stderr, "cmd: %s\n", commandName(cmd));
        if (cmd != SimCommand::Stop) {
            // 新持续型指令覆盖进行中的相对运动（stop 只松油门，不打断 fdist/turn）
            robot_.setRelativeDist(0.0f);
            robot_.setRelativeTurn(0.0f);
        }
        lastContinuous_ = cmd;
    }
}

void SimEngine::applyEvent(const SimEvent& ev) {
    switch (ev.type) {
        case SimEvent::Type::FwdDist:
            std::fprintf(stderr, "cmd: fdist %.2f\n", ev.param);
            robot_.setRelativeDist(ev.param);
            break;
        case SimEvent::Type::Turn:
            std::fprintf(stderr, "cmd: turn %.2f\n", ev.param);
            robot_.setRelativeTurn(glm::radians(ev.param));
            break;
        case SimEvent::Type::Pick:
            if (world_.isCarrying()) {
                std::fprintf(stderr, "pick: 已持有物体 (idx=%d, %.2fkg)，忽略\n",
                             world_.carriedIndex(), world_.carriedMass());
            } else if (world_.pick(robot_)) {
                const auto& ob = world_.obstacles()[world_.carriedIndex()];
                std::fprintf(stderr, "pick: 成功拾取 idx=%d mass=%.2fkg pos=(%.2f,%.2f)\n",
                             world_.carriedIndex(), world_.carriedMass(), ob.center.x, ob.center.z);
            } else {
                std::fprintf(stderr, "pick: 前方 %.2fm 内无障碍物，拾取失败\n", World::kPickRange);
            }
            break;
        case SimEvent::Type::Place:
            if (world_.place(robot_)) {
                std::fprintf(stderr, "place: 放置成功\n");
            } else {
                std::fprintf(stderr, "place: 未持有物体，放置失败\n");
            }
            break;
        case SimEvent::Type::Save:
            saveScene();
            break;
    }
}

void SimEngine::step(float dt) {
    RobotInput input = commandToInput(command_, speed_);
    RobotInput relInput;
    if (robot_.tickRelative(dt, relInput)) {
        input = relInput;
    }
    robot_.update(dt, input, world_.effectiveMaxSpeed());
    world_.resolveCollisions(robot_);
    world_.updateCarried(robot_);
}
