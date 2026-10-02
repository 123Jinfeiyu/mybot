#pragma once

#include <glm/glm.hpp>

// 语义动作折算出的身体速度目标，由 CommandBridge 的指令填充。
// 「停」就是 throttle=0，减速快慢由 update() 的滞后常数决定。
struct RobotInput {
    float throttle = 0.0f;  // -1 (后退) .. 1 (前进)
    float steer = 0.0f;     // -1 (右) .. 1 (左)
};

// 蜘蛛身体的平面运动学：一阶滞后逼近目标速度，再积分出位姿。
//
// 刻意不含物理、不含足地接触、不做 IK、也不知道场景里有什么——
// mybot 只发「前进/后退/转向/停」，这里只负责把这些动作变成身体在场景里
// 的位移结果；碰撞解算/射线测距/搬运等与场景的相互作用归 World。
//
// 坐标系：机体局部 +X 为前、+Y 为上、+Z 为**右**（右手系 X×Y=Z 的必然结果）。
// 因此 theta 增大 = 向左转，与 steer>0 = 左转 一致。
class Robot {
public:
    static constexpr float kMaxSpeed = 1.6f;     // m/s（空载上限）
    static constexpr float kMaxTurnRate = 2.2f;  // rad/s
    // 松开指令后的减速速率（1/s）。update() 的滞后常数与 tickRelative() 的
    // 刹车尾程估算共用这一个值——两处不一致刹车就会失准
    static constexpr float kStopRate = 6.0f;

    // 躯干尺寸，spider.cpp 按这些常量摆放身体与髋部；World 用车身前端作
    // 测距射线与拾取判定的原点
    static constexpr float kBodyLength = 0.40f;
    static constexpr float kBodyWidth = 0.28f;
    static constexpr float kBodyHeight = 0.14f;
    static constexpr float kBodyY = 0.26f;

    // 激光测距仪：最大探测距离（米）。超出此距离即使无障碍也返回该值。
    static constexpr float kRangeMax = 10.0f;

    // maxSpeed 由调用方给（World 按搬运负载折算后传入）；空载传 Robot::kMaxSpeed
    void update(float dt, const RobotInput& input, float maxSpeed);
    void reset();

    glm::mat4 baseTransform() const;

    float x() const { return x_; }
    float z() const { return z_; }
    float theta() const { return theta_; }
    float v() const { return v_; }
    float omega() const { return omega_; }

    void setX(float x) { x_ = x; }
    void setZ(float z) { z_ = z; }
    // 场景装载/重载用：整体设位姿（theta 弧度），不动速度
    void setPose(float x, float z, float theta) { x_ = x; z_ = z; theta_ = theta; }

    // 相对运动：fdist 与 turn 各记各的剩余量、**顺序执行**（先走完再转），
    // 同通道新目标覆盖旧目标。设定后每帧由主循环调用 tickRelative()，到量即
    // 自动停——到量前会提前松油门刹车，消掉一阶滞后的减速尾程。
    void setRelativeDist(float meters);
    void setRelativeTurn(float radians);
    // 每帧调用，返回本帧应覆盖的 input（有相对运动时忽略外部 input）。
    // 返回 true 表示相对运动仍在进行、false 表示已完成或没有。
    bool tickRelative(float dt, RobotInput& out);

private:
    float x_ = 0.0f, z_ = 0.0f, theta_ = 0.0f;
    float v_ = 0.0f, omega_ = 0.0f;

    // 相对运动剩余量；<=0 表示无待执行的相对运动。
    float remainingDist_ = 0.0f;   // 米（正=前进，负=后退）
    float remainingTurn_ = 0.0f;   // 弧度（正=左转，负=右转）
};
