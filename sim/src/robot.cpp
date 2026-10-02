#include "robot.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using glm::mat4;
using glm::vec3;

void Robot::update(float dt, const RobotInput& input, float maxSpeed) {
    const float targetV = input.throttle * maxSpeed;
    const float targetOmega = input.steer * kMaxTurnRate;
    // 松开指令（throttle=0）时减速明显快于加速，否则「停」之后还溜一大段
    const float vRate = std::min(1.0f, (input.throttle == 0.0f ? kStopRate : 3.0f) * dt);
    const float oRate = std::min(1.0f, kStopRate * dt);
    v_ += (targetV - v_) * vRate;
    omega_ += (targetOmega - omega_) * oRate;

    theta_ += omega_ * dt;
    x_ += v_ * std::cos(theta_) * dt;
    z_ += v_ * -std::sin(theta_) * dt;
}

void Robot::reset() {
    *this = Robot();
}

mat4 Robot::baseTransform() const {
    return glm::translate(mat4(1.0f), vec3(x_, 0.0f, z_)) *
           glm::rotate(mat4(1.0f), theta_, vec3(0.0f, 1.0f, 0.0f));
}

void Robot::setRelativeDist(float meters) {
    remainingDist_ = meters;
}

void Robot::setRelativeTurn(float radians) {
    remainingTurn_ = radians;
}

bool Robot::tickRelative(float dt, RobotInput& out) {
    if (std::abs(remainingDist_) > 1e-4f) {
        const float dir = remainingDist_ > 0.0f ? 1.0f : -1.0f;
        // 提前刹车：一阶滞后下松开油门后的滑行尾程 ≈ v/kStopRate。剩余量
        // 小于尾程就收油门，让滑行恰好落在目标上；每帧重估，自收敛——
        // 否则满速到量再松开，尾程会多滑 v/kStopRate（60fps 下 ~0.27m/21°）
        out.throttle = (std::abs(remainingDist_) > std::abs(v_) / kStopRate) ? dir : 0.0f;
        out.steer = 0.0f;
        const float traveled = std::abs(v_) * dt;
        remainingDist_ -= dir * traveled;
        if (std::abs(remainingDist_) <= 1e-4f ||
            (dir > 0 && remainingDist_ < 0) || (dir < 0 && remainingDist_ > 0)) {
            remainingDist_ = 0.0f;
        }
        return true;
    }
    if (std::abs(remainingTurn_) > 1e-4f) {
        const float dir = remainingTurn_ > 0.0f ? 1.0f : -1.0f;
        // 转向同理：剩余角量小于滑行尾程 ω/kStopRate 就回正方向舵
        out.throttle = 0.0f;
        out.steer = (std::abs(remainingTurn_) > std::abs(omega_) / kStopRate) ? dir : 0.0f;
        const float turned = std::abs(omega_) * dt;
        remainingTurn_ -= dir * turned;
        if (std::abs(remainingTurn_) <= 1e-4f ||
            (dir > 0 && remainingTurn_ < 0) || (dir < 0 && remainingTurn_ > 0)) {
            remainingTurn_ = 0.0f;
        }
        return true;
    }
    return false;
}
