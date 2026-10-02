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
    const float vRate = std::min(1.0f, (input.throttle == 0.0f ? 6.0f : 3.0f) * dt);
    const float oRate = std::min(1.0f, 6.0f * dt);
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
    remainingTurn_ = 0.0f;
}

void Robot::setRelativeTurn(float radians) {
    remainingTurn_ = radians;
    remainingDist_ = 0.0f;
}

bool Robot::tickRelative(float dt, RobotInput& out) {
    if (std::abs(remainingDist_) > 1e-4f) {
        const float dir = remainingDist_ > 0.0f ? 1.0f : -1.0f;
        out.throttle = dir;
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
        out.throttle = 0.0f;
        out.steer = dir;
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
