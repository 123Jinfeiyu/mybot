#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim_config.h"

namespace {

// 场景文件魔数
constexpr char kSceneMagic[] = "mybot-sim scene v1";

// 从 p 起连续解析至多 want 个浮点数，got 返回实际解析个数。
// 遇到非数字即停（返回 false）；解析不足 want 个不算错——由调用方按各自
// 的「必填/可选字段数」校验 got。
bool parseFloats(const char* p, float* vals, int want, int& got) {
    got = 0;
    while (got < want) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) {
            return false;
        }
        vals[got++] = static_cast<float>(v);
        p = end;
    }
    return true;
}

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
        ob.mass = volume * SimConfig::kDefaultDensity;
    }
    return obs;
}

World::World(std::vector<Obstacle> obstacles) : obstacles_(std::move(obstacles)) {}

void World::resolveCollisions(Robot& robot) {
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == carriedIdx_) continue;  // 被搬运物附着在机器人上，不参与碰撞
        float rx = robot.x(), rz = robot.z();
        if (resolveSphereAABB2D(rx, rz, SimConfig::kRobotRadius, obstacles_[i])) {
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

void World::applyScene(const SceneState& scene) {
    obstacles_ = scene.obstacles;
    carriedIdx_ = -1;
    carriedMass_ = 0.0f;
    if (scene.carriedIdx >= 0 && scene.carriedIdx < (int)obstacles_.size()) {
        carriedIdx_ = scene.carriedIdx;
        carriedMass_ = obstacles_[carriedIdx_].mass;
    }
}

bool readScene(const std::string& path, SceneState& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        std::fprintf(stderr, "scene: 无法打开 %s\n", path.c_str());
        return false;
    }

    SceneState st;
    char line[SimConfig::kMaxSceneLine];
    bool magicSeen = false;
    bool ok = true;
    while (std::fgets(line, sizeof(line), f)) {
        // 去注释与首尾空白
        if (char* hash = std::strchr(line, '#')) {
            *hash = '\0';
        }
        char* text = line;
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
            ++text;
        }
        if (*text == '\0') {
            continue;
        }

        if (!magicSeen) {
            if (std::strncmp(text, kSceneMagic, sizeof(kSceneMagic) - 1) == 0) {
                magicSeen = true;
            } else {
                std::fprintf(stderr, "scene: 首个有效行不是魔数 '%s'，拒绝整个文件\n",
                             kSceneMagic);
                ok = false;
                break;
            }
            continue;
        }

        char word[16] = {};
        if (std::sscanf(text, "%15s", word) != 1) {
            continue;
        }
        const char* rest = text + std::strlen(word);

        if (std::strcmp(word, "box") == 0) {
            float v[10] = {};
            int got = 0;
            if (!parseFloats(rest, v, 10, got) || (got != 9 && got != 10)) {
                std::fprintf(stderr, "scene: box 行需要 9 或 10 个数（cx cy cz hx hy hz r g b [mass]），跳过: %s", text);
                continue;
            }
            if (v[3] <= 0.0f || v[4] <= 0.0f || v[5] <= 0.0f) {
                std::fprintf(stderr, "scene: box 半尺寸必须为正，跳过: %s", text);
                continue;
            }
            if (st.obstacles.size() >= SimConfig::kMaxSceneObstacles) {
                std::fprintf(stderr, "scene: 障碍数超过上限 %zu，多余忽略\n", SimConfig::kMaxSceneObstacles);
                continue;
            }
            Obstacle ob;
            ob.center = glm::vec3(v[0], v[1], v[2]);
            ob.half = glm::vec3(v[3], v[4], v[5]);
            ob.color = glm::vec3(v[6], v[7], v[8]);
            ob.mass = (got == 10) ? v[9]
                                  : 8.0f * ob.half.x * ob.half.y * ob.half.z * SimConfig::kDefaultDensity;
            st.obstacles.push_back(ob);
        } else if (std::strcmp(word, "robot") == 0) {
            float v[4] = {};
            int got = 0;
            if (!parseFloats(rest, v, 4, got) || (got != 3 && got != 4)) {
                std::fprintf(stderr, "scene: robot 行需要 3 或 4 个数（x z theta_deg [carried_idx]），跳过: %s", text);
                continue;
            }
            st.robotX = v[0];
            st.robotZ = v[1];
            st.robotTheta = glm::radians(v[2]);
            st.carriedIdx = (got == 4) ? static_cast<int>(v[3]) : -1;
        } else {
            std::fprintf(stderr, "scene: 未知行类型 '%s'（忽略）\n", word);
        }
    }
    std::fclose(f);

    if (!ok || !magicSeen) {
        return false;
    }
    if (st.carriedIdx >= (int)st.obstacles.size()) {
        std::fprintf(stderr, "scene: carried_idx=%d 越界（共 %zu 个障碍），按无搬运处理\n",
                     st.carriedIdx, st.obstacles.size());
        st.carriedIdx = -1;
    }
    out = std::move(st);
    return true;
}

bool writeScene(const std::string& path, const SceneState& state) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "scene: 无法写 %s\n", tmp.c_str());
        return false;
    }

    std::fprintf(f, "%s\n", kSceneMagic);
    std::fprintf(f, "# box cx cy cz hx hy hz r g b [mass（缺省=体积x800kg/m3）]\n");
    std::fprintf(f, "# robot x z theta_deg [carried_idx]\n");
    std::fprintf(f, "robot %.4f %.4f %.4f", state.robotX, state.robotZ,
                 glm::degrees(state.robotTheta));
    if (state.carriedIdx >= 0) {
        std::fprintf(f, " %d", state.carriedIdx);
    }
    std::fputc('\n', f);
    for (const Obstacle& ob : state.obstacles) {
        std::fprintf(f, "box %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f %.2f\n",
                     ob.center.x, ob.center.y, ob.center.z,
                     ob.half.x, ob.half.y, ob.half.z,
                     ob.color.r, ob.color.g, ob.color.b, ob.mass);
    }

    const bool flushed = std::fflush(f) == 0;
    if (!flushed || std::fclose(f) != 0) {
        std::fprintf(stderr, "scene: 写 %s 失败（磁盘满？）\n", tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "scene: rename %s -> %s 失败\n", tmp.c_str(), path.c_str());
        return false;
    }
    return true;
}
