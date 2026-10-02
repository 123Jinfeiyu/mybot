#pragma once

#include <glm/glm.hpp>

#include <vector>

// 场景障碍物：盒体（center=中心, half=半尺寸, color=漫反射色, mass=质量kg）。
// 放在 robot.h 是因为 Robot::queryRange 需要对障碍做射线求交。
// mass 由体积×密度算出，用于搬运时的减速计算。
struct Obstacle {
    glm::vec3 center;
    glm::vec3 half;
    glm::vec3 color;
    float mass = 1.0f;  // 质量（kg），缺省 1.0；由 main.cpp 按体积×密度填充
};

// 语义动作折算出的身体速度目标，由 CommandBridge 的指令填充。
// 迁入前这里是键盘油门/转向/刹车；现在上游是 mybot，没有刹车一说——
// 「停」就是 throttle=0，减速快慢由 update() 的滞后常数决定。
struct RobotInput {
    float throttle = 0.0f;  // -1 (后退) .. 1 (前进)
    float steer = 0.0f;     // -1 (右) .. 1 (左)
};

// 蜘蛛身体的平面运动学：一阶滞后逼近目标速度，再积分出位姿。
//
// 沿用迁入前那台差速小车的模型。刻意不含物理、不含足地接触、不做 IK——
// mybot 只发「前进/后退/转向/停」，这里只负责把这些动作变成身体在场景里
// 的位移结果。
//
// 坐标系：机体局部 +X 为前、+Y 为上、+Z 为**右**（右手系 X×Y=Z 的必然结果）。
// 因此 theta 增大 = 向左转，与 steer>0 = 左转 一致。
class Robot {
public:
    static constexpr float kMaxSpeed = 1.6f;     // m/s
    static constexpr float kMaxTurnRate = 2.2f;  // rad/s

    // 躯干尺寸，spider.cpp 按这些常量摆放身体与髋部
    static constexpr float kBodyLength = 0.40f;
    static constexpr float kBodyWidth = 0.28f;
    static constexpr float kBodyHeight = 0.14f;
    static constexpr float kBodyY = 0.26f;

    // 激光测距仪：最大探测距离（米）。超出此距离即使无障碍也返回该值。
    static constexpr float kRangeMax = 10.0f;

    void update(float dt, const RobotInput& input);
    void reset();

    glm::mat4 baseTransform() const;

    float x() const { return x_; }
    float z() const { return z_; }
    float theta() const { return theta_; }
    float v() const { return v_; }
    float omega() const { return omega_; }

    void setX(float x) { x_ = x; }
    void setZ(float z) { z_ = z; }

    // 前方射线与场景障碍求交，返回最近命中距离（米）。
    // 射线从身体前端中心出发、沿 heading 方向；无命中返回 maxRange。
    float queryRange(const std::vector<Obstacle>& obstacles,
                     float maxRange = kRangeMax) const;

    // 相对运动：设定后每帧由主循环调用 tickRelative()，到量即自动停。
    void setRelativeDist(float meters);
    void setRelativeTurn(float radians);
    // 每帧调用，返回本帧应覆盖的 input（有相对运动时忽略外部 input）。
    // 返回 true 表示相对运动仍在进行、false 表示已完成或没有。
    bool tickRelative(float dt, RobotInput& out);

    // 搬运设备：拾取/放置前方物体。
    // pick: 尝试拾取前方最近的可搬运物体（距离 < pickRange），成功返回 true。
    // place: 把当前搬运的物体放到前方 placeDist 处，成功返回 true。
    // isCarrying: 是否正在搬运物体。
    // carriedMass: 当前搬运物体的质量（kg），未搬运时返回 0。
    // effectiveMaxSpeed: 考虑搬运负载后的最大速度（m/s）。
    // updateCarriedPosition: 更新被搬运物体的位置（跟随机器人移动）。
    bool pick(std::vector<Obstacle>& obstacles, float pickRange = 0.6f);
    bool place(std::vector<Obstacle>& obstacles, float placeDist = 0.5f);
    bool isCarrying() const { return carriedIdx_ >= 0; }
    int carriedObstacleIndex() const { return carriedIdx_; }
    float carriedMass() const;
    float effectiveMaxSpeed() const;
    void updateCarriedPosition(std::vector<Obstacle>& obstacles) const;

private:
    float x_ = 0.0f, z_ = 0.0f, theta_ = 0.0f;
    float v_ = 0.0f, omega_ = 0.0f;

    // 相对运动剩余量；<=0 表示无待执行的相对运动。
    float remainingDist_ = 0.0f;   // 米（正=前进，负=后退）
    float remainingTurn_ = 0.0f;   // 弧度（正=左转，负=右转）

    // 搬运设备：当前搬运的障碍物索引（-1=未搬运）及其质量缓存。
    int carriedIdx_ = -1;
    float carriedMassCache_ = 0.0f;
};
