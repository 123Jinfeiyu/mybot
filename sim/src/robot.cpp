#include "robot.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using glm::mat4;
using glm::vec3;

void Robot::update(float dt, const RobotInput& input) {
    const float targetV = input.throttle * effectiveMaxSpeed();
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

namespace {

// 2D 射线 vs AABB（XZ 平面）求交：slab 法。
// 射线从 (ox,oz) 沿 (dx,dz) 方向，返回命中距离；无命中返回 false。
bool rayAABB2D(float ox, float oz, float dx, float dz,
               const Obstacle& ob, float& tHit) {
    const float minX = ob.center.x - ob.half.x;
    const float maxX = ob.center.x + ob.half.x;
    const float minZ = ob.center.z - ob.half.z;
    const float maxZ = ob.center.z + ob.half.z;

    float tmin = -1e30f, tmax = 1e30f;

    if (std::abs(dx) < 1e-8f) {
        if (ox < minX || ox > maxX) return false;
    } else {
        float t1 = (minX - ox) / dx;
        float t2 = (maxX - ox) / dx;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }

    if (std::abs(dz) < 1e-8f) {
        if (oz < minZ || oz > maxZ) return false;
    } else {
        float t1 = (minZ - oz) / dz;
        float t2 = (maxZ - oz) / dz;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }

    if (tmin < 0.0f) return false;
    tHit = tmin;
    return true;
}

}  // namespace

float Robot::queryRange(const std::vector<Obstacle>& obstacles, float maxRange) const {
    const float forwardX = std::cos(theta_);
    const float forwardZ = -std::sin(theta_);
    // 射线从身体前端中心出发
    const float ox = x_ + forwardX * (kBodyLength * 0.5f);
    const float oz = z_ + forwardZ * (kBodyLength * 0.5f);

    float nearest = maxRange;
    for (int i = 0; i < (int)obstacles.size(); ++i) {
        if (i == carriedIdx_) continue;  // 跳过被搬运的物体
        float t;
        if (rayAABB2D(ox, oz, forwardX, forwardZ, obstacles[i], t) && t < nearest) {
            nearest = t;
        }
    }
    return nearest;
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

float Robot::carriedMass() const {
    return carriedMassCache_;
}

float Robot::effectiveMaxSpeed() const {
    // 搬运时的减速：质量越大，速度越慢。
    // 公式：maxSpeed / (1 + mass * k)，k=0.02 时 1kg 减速 2%，400kg 减速 89%。
    constexpr float k = 0.01f;
    return kMaxSpeed / (1.0f + carriedMassCache_ * k);
}

bool Robot::pick(std::vector<Obstacle>& obstacles, float pickRange) {
    if (carriedIdx_ >= 0) { return false; }  // 已搬运
    
    const float forwardX = std::cos(theta_);
    const float forwardZ = -std::sin(theta_);
    const float ox = x_ + forwardX * (kBodyLength * 0.5f);
    const float oz = z_ + forwardZ * (kBodyLength * 0.5f);
    
    // 找前方最近的、在 pickRange 内的障碍物
    int bestIdx = -1;
    float bestDist = pickRange;
    for (int i = 0; i < (int)obstacles.size(); ++i) {
        const auto& ob = obstacles[i];
        // 计算 AABB 表面最近点到机器人前端的距离
        const float closestX = std::clamp(ox, ob.center.x - ob.half.x, ob.center.x + ob.half.x);
        const float closestZ = std::clamp(oz, ob.center.z - ob.half.z, ob.center.z + ob.half.z);
        const float dx = closestX - ox;
        const float dz = closestZ - oz;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < bestDist) {
            // 还要判断在前方（点积 > 0）
            const float toCenterX = ob.center.x - ox;
            const float toCenterZ = ob.center.z - oz;
            const float dot = toCenterX * forwardX + toCenterZ * forwardZ;
            if (dot > 0.0f) {
                bestDist = dist;
                bestIdx = i;
            }
        }
    }
    
    if (bestIdx >= 0) {
        carriedIdx_ = bestIdx;
        carriedMassCache_ = obstacles[bestIdx].mass;
        return true;
    }
    return false;
}

bool Robot::place(std::vector<Obstacle>& obstacles, float placeDist) {
    if (carriedIdx_ < 0 || carriedIdx_ >= (int)obstacles.size()) {
        return false;
    }
    
    // 把搬运的物体放到前方 placeDist 处
    const float forwardX = std::cos(theta_);
    const float forwardZ = -std::sin(theta_);
    auto& ob = obstacles[carriedIdx_];
    ob.center.x = x_ + forwardX * (kBodyLength * 0.5f + placeDist);
    ob.center.z = z_ + forwardZ * (kBodyLength * 0.5f + placeDist);
    
    carriedIdx_ = -1;
    carriedMassCache_ = 0.0f;
    return true;
}

void Robot::updateCarriedPosition(std::vector<Obstacle>& obstacles) const {
    if (carriedIdx_ < 0 || carriedIdx_ >= (int)obstacles.size()) {
        return;
    }
    auto& ob = obstacles[carriedIdx_];
    // 放在机器人正上方，抬高避免挡住摄像机
    ob.center.x = x_;
    ob.center.z = z_;
    ob.center.y = kBodyY + kBodyHeight + ob.half.y + 0.10f;
}
