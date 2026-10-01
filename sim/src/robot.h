#pragma once

#include <glm/glm.hpp>

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

    void update(float dt, const RobotInput& input);
    void reset();

    glm::mat4 baseTransform() const;

    float x() const { return x_; }
    float z() const { return z_; }
    float theta() const { return theta_; }
    float v() const { return v_; }
    float omega() const { return omega_; }

private:
    float x_ = 0.0f, z_ = 0.0f, theta_ = 0.0f;
    float v_ = 0.0f, omega_ = 0.0f;
};
