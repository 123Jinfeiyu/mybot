#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// 蜘蛛碰撞球半径（XZ 平面，约半车身长）。刻意贴着 Robot::kBodyLength 而不
// 直接引用：碰撞体是圆的，车身是方的，取半车长会卡进盒缝。
constexpr float kRobotRadius = 0.20f;

// 默认密度 kg/m³（近似木材）
constexpr float kDefaultDensity = 800.0f;

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

// 球（XZ 平面）vs AABB（XZ 截面）碰撞解算：把球心推出盒面。
// 返回 true 表示发生了碰撞（位置被修正）。
bool resolveSphereAABB2D(float& sx, float& sz, float radius,
                         const Obstacle& ob) {
    const float bx = ob.center.x, bz = ob.center.z;
    const float hx = ob.half.x, hz = ob.half.z;
    // 盒面上离球心最近的点
    const float cx = std::clamp(sx, bx - hx, bx + hx);
    const float cz = std::clamp(sz, bz - hz, bz + hz);
    const float dx = sx - cx, dz = sz - cz;
    const float distSq = dx * dx + dz * dz;
    if (distSq >= radius * radius) {
        return false;  // 无碰撞
    }
    const float dist = std::sqrt(distSq);
    if (dist < 1e-5f) {
        // 球心在盒内：沿最短轴推出
        const float penXp = (bx + hx) - sx, penXn = sx - (bx - hx);
        const float penZp = (bz + hz) - sz, penZn = sz - (bz - hz);
        float minPen = penXp;
        int axis = 0;  // 0=x, 1=z
        float sign = 1.0f;
        if (penXn < minPen) { minPen = penXn; axis = 0; sign = -1.0f; }
        if (penZp < minPen) { minPen = penZp; axis = 1; sign = 1.0f; }
        if (penZn < minPen) { minPen = penZn; axis = 1; sign = -1.0f; }
        const float push = radius + minPen;
        if (axis == 0) {
            sx += sign * push;
        } else {
            sz += sign * push;
        }
    } else {
        // 球心在盒外但距离 < radius：沿 dx,dz 方向推出
        const float push = radius - dist;
        sx += (dx / dist) * push;
        sz += (dz / dist) * push;
    }
    return true;
}

}  // namespace

std::vector<Obstacle> World::defaultScene() {
    std::vector<Obstacle> obs = {
        {{2.0f, 0.40f, 1.2f}, {0.40f, 0.40f, 0.40f}, {0.72f, 0.30f, 0.24f}},   // 前进左前方的红箱
        {{3.5f, 0.50f, -0.6f}, {0.20f, 0.50f, 1.20f}, {0.45f, 0.52f, 0.60f}},  // 横在右前的矮墙
        {{1.2f, 0.25f, -1.6f}, {0.50f, 0.25f, 0.50f}, {0.80f, 0.66f, 0.25f}},  // 右侧的黄块
        {{4.5f, 0.75f, 1.8f}, {0.25f, 0.75f, 0.25f}, {0.30f, 0.55f, 0.35f}},   // 远处的绿柱
        {{0.6f, 0.30f, 2.2f}, {0.30f, 0.30f, 0.30f}, {0.55f, 0.42f, 0.60f}},   // 左后的紫箱
        {{6.0f, 0.35f, 0.8f}, {0.35f, 0.35f, 0.35f}, {0.85f, 0.45f, 0.20f}},   // 第二排橙箱
        {{7.5f, 0.60f, -1.0f}, {0.20f, 0.60f, 0.80f}, {0.30f, 0.40f, 0.65f}},  // 深处蓝墙
        {{5.5f, 0.25f, -2.2f}, {0.45f, 0.25f, 0.45f}, {0.25f, 0.60f, 0.55f}},  // 右深的青块
        {{8.5f, 0.40f, 1.5f}, {0.40f, 0.40f, 0.40f}, {0.65f, 0.30f, 0.50f}},   // 远处品红箱
        {{6.8f, 0.30f, 2.8f}, {0.30f, 0.30f, 0.30f}, {0.55f, 0.65f, 0.25f}},   // 左深的绿黄箱
        {{9.5f, 0.50f, -0.2f}, {0.25f, 0.50f, 0.25f}, {0.75f, 0.75f, 0.72f}},  // 尽头白柱
    };
    for (auto& ob : obs) {
        const float volume = 8.0f * ob.half.x * ob.half.y * ob.half.z;  // 2*half 是全尺寸
        ob.mass = volume * kDefaultDensity;
    }
    return obs;
}

World::World(std::vector<Obstacle> obstacles) : obstacles_(std::move(obstacles)) {}

void World::resolveCollisions(Robot& robot) {
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == carriedIdx_) continue;  // 被搬运物附着在机器人上，不参与碰撞
        float rx = robot.x(), rz = robot.z();
        if (resolveSphereAABB2D(rx, rz, kRobotRadius, obstacles_[i])) {
            robot.setX(rx);
            robot.setZ(rz);
        }
    }
}

float World::queryRange(const Robot& robot, float maxRange) const {
    const float forwardX = std::cos(robot.theta());
    const float forwardZ = -std::sin(robot.theta());
    // 射线从身体前端中心出发
    const float ox = robot.x() + forwardX * (Robot::kBodyLength * 0.5f);
    const float oz = robot.z() + forwardZ * (Robot::kBodyLength * 0.5f);

    float nearest = maxRange;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == carriedIdx_) continue;  // 跳过被搬运的物体
        float t;
        if (rayAABB2D(ox, oz, forwardX, forwardZ, obstacles_[i], t) && t < nearest) {
            nearest = t;
        }
    }
    return nearest;
}

bool World::pick(const Robot& robot) {
    if (carriedIdx_ >= 0) {
        return false;  // 已搬运
    }

    const float forwardX = std::cos(robot.theta());
    const float forwardZ = -std::sin(robot.theta());
    const float ox = robot.x() + forwardX * (Robot::kBodyLength * 0.5f);
    const float oz = robot.z() + forwardZ * (Robot::kBodyLength * 0.5f);

    // 找前方最近的、在拾取半径内的障碍物
    int bestIdx = -1;
    float bestDist = kPickRange;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        const auto& ob = obstacles_[i];
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
        carriedMass_ = obstacles_[bestIdx].mass;
        return true;
    }
    return false;
}

bool World::place(const Robot& robot) {
    if (carriedIdx_ < 0 || carriedIdx_ >= (int)obstacles_.size()) {
        return false;
    }

    // 把搬运的物体放到前方 kPlaceDist 处
    const float forwardX = std::cos(robot.theta());
    const float forwardZ = -std::sin(robot.theta());
    auto& ob = obstacles_[carriedIdx_];
    ob.center.x = robot.x() + forwardX * (Robot::kBodyLength * 0.5f + kPlaceDist);
    ob.center.z = robot.z() + forwardZ * (Robot::kBodyLength * 0.5f + kPlaceDist);

    carriedIdx_ = -1;
    carriedMass_ = 0.0f;
    return true;
}

float World::effectiveMaxSpeed() const {
    // 搬运时的减速：质量越大，速度越慢。
    // 公式：maxSpeed / (1 + mass * k)，k=0.01 时 1kg 减速 1%，400kg 减速 80%。
    constexpr float k = 0.01f;
    return Robot::kMaxSpeed / (1.0f + carriedMass_ * k);
}

void World::updateCarried(const Robot& robot) {
    if (carriedIdx_ < 0 || carriedIdx_ >= (int)obstacles_.size()) {
        return;
    }
    auto& ob = obstacles_[carriedIdx_];
    // 放在机器人正上方，抬高避免挡住摄像机
    ob.center.x = robot.x();
    ob.center.z = robot.z();
    ob.center.y = Robot::kBodyY + Robot::kBodyHeight + ob.half.y + 0.10f;
}
