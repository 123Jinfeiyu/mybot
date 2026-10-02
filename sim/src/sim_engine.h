#pragma once

#include <string>

#include "bridge.h"  // SimCommand, SimEvent
#include "robot.h"
#include "world.h"

// 把「机器人 + 世界 + 场景会话 + 每帧推进 + 指令派发」收进一个 **GL 无关** 的对象。
// 此前这些逻辑是 main.cpp（GLFW 壳）里的自由函数与 lambda，无法在无 GL 的
// sim_tests 里验证；抽成 SimEngine 后，fdist 到量、pick/place、save/reload 等
// 都能 headless 单测。
//
// 职责边界：引擎只管仿真语义与物理推进，**不碰** socket、GL、蜘蛛外观步态
// （Spider 是可视化，留在壳里按 robot().v() 自己生成）。壳每帧从命令桥取出
// 持续型状态与一次性事件喂给引擎，再调 step(dt)，然后读 robot()/world() 渲染。
class SimEngine {
public:
    // scenePath 空 = 用内置默认场景（无文件）。构造后需调 reloadScene() 装载。
    explicit SimEngine(std::string scenePath = std::string());

    // 装载/重载场景（含机器人位姿）。文件读失败回落内置默认并告警，不开黑屏世界。
    // 与旧 main.cpp 一致：复位机器人位姿/速度/相对运动，但不动持续型油门状态
    // （油门来自命令桥，重载后若桥仍是 forward，蜘蛛从新位姿继续走）。
    void reloadScene();
    // 把当前世界状态（障碍 + 位姿 + 搬运）原子写回场景文件；未配置路径则告警。
    void saveScene();

    // 持续型指令：每帧从命令桥喂入当前状态。改油门/方向；非 stop 的新持续型
    // 指令打断进行中的相对运动（stop 只松油门、不打断）。边沿触发日志。
    void applyContinuous(SimCommand cmd, float speed);
    // 一次性事件：fdist/turn 设相对运动剩余量、pick/place 交给 World、save 落盘。
    void applyEvent(const SimEvent& ev);

    // 每帧推进：tickRelative（相对运动接管则覆盖输入）→ update（按搬运负载折算
    // 最大速度）→ resolveCollisions → updateCarried。
    void step(float dt);

    // 感知：身体前端中心沿 heading 的射线测距（米）。
    float queryRange() const { return world_.queryRange(robot_); }

    // 壳渲染/HUD 用的只读访问
    const Robot& robot() const { return robot_; }
    const World& world() const { return world_; }
    const std::string& sceneName() const { return sceneName_; }

private:
    RobotInput commandToInput(SimCommand cmd, float speed) const;

    std::string scenePath_;
    std::string sceneName_;
    Robot robot_;
    World world_;

    // 当前持续型状态（每帧由 applyContinuous 从命令桥同步）
    SimCommand command_ = SimCommand::Stop;
    float speed_ = 1.0f;
    SimCommand lastContinuous_ = SimCommand::Stop;  // 边沿检测：只在变化时打断/记日志
};
