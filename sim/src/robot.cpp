#include "robot.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using glm::mat4;
using glm::vec3;

void Robot::update(float dt, const RobotInput& input) {
    const float targetV = input.throttle * kMaxSpeed;
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
