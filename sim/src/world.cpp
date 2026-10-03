#include "world.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim_config.h"

namespace {

// 场景文件魔数：v1（单机器人）/v2（多机器人）/v3（box 行带朝向）都收，写一律 v3。
constexpr char kSceneMagicV1[] = "mybot-sim scene v1";
constexpr char kSceneMagicV2[] = "mybot-sim scene v2";
constexpr char kSceneMagicV3[] = "mybot-sim scene v3";

// 多机器人场景的机器人数上限（防滥用；每只一套 socket，太多也没意义）。
constexpr std::size_t kMaxSceneRobots = 64;

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

// 朝向盒（OBB）的局部系约定：盒局部 +X 轴在世界系的方向是 (cos yaw, -sin yaw)，
// 与 Robot::theta 同号。解算 OBB 的思路——绕 Y 旋转是等距变换（保长度、保点积），
// 所以把点/方向**旋进盒局部系**后就能复用轴对齐数学，再把结果旋回世界系。
// 下面两个辅助互逆。仅对「点相对盒心」的位移增量做旋回，避免把盒心也旋了。
inline void worldToLocal2D(float dx, float dz, float yaw, float& lx, float& lz) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    lx = dx * c - dz * s;
    lz = dx * s + dz * c;
}
inline void localToWorld2D(float lx, float lz, float yaw, float& dx, float& dz) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    dx = lx * c + lz * s;
    dz = -lx * s + lz * c;
}

// 2D 射线 vs OBB（XZ 平面）求交：把射线旋进盒局部系转为轴对齐 slab 法。
// 射线从世界系 (ox,oz) 沿世界系 (dx,dz) 方向；旋转保距，故局部系里算出的 tHit
// 直接可用。无命中返回 false。
bool rayOBB2D(float ox, float oz, float dx, float dz,
              const Obstacle& ob, float& tHit) {
    const float yaw = ob.yaw;
    float lox, loz, ldx, ldz;
    worldToLocal2D(ox - ob.center.x, oz - ob.center.z, yaw, lox, loz);
    worldToLocal2D(dx, dz, yaw, ldx, ldz);

    const float hx = ob.half.x, hz = ob.half.z;
    float tmin = -1e30f, tmax = 1e30f;

    if (std::abs(ldx) < 1e-8f) {
        if (lox < -hx || lox > hx) return false;
    } else {
        float t1 = (-hx - lox) / ldx;
        float t2 = (hx - lox) / ldx;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }

    if (std::abs(ldz) < 1e-8f) {
        if (loz < -hz || loz > hz) return false;
    } else {
        float t1 = (-hz - loz) / ldz;
        float t2 = (hz - loz) / ldz;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }

    if (tmin < 0.0f) return false;
    tHit = tmin;
    return true;
}

// 球（XZ 平面）vs OBB（XZ 截面）碰撞解算：旋进盒局部系按原点盒解算，再把位移
// 增量旋回世界系叠加。返回 true 表示发生了碰撞（位置被修正）。
bool resolveSphereOBB2D(float& sx, float& sz, float radius,
                        const Obstacle& ob) {
    const float yaw = ob.yaw;
    float lx, lz;
    worldToLocal2D(sx - ob.center.x, sz - ob.center.z, yaw, lx, lz);
    const float hx = ob.half.x, hz = ob.half.z;

    // 盒（局部系里以原点为中心，半尺寸 ±half）面上离球心最近的点
    const float cx = std::clamp(lx, -hx, hx);
    const float cz = std::clamp(lz, -hz, hz);
    const float dx = lx - cx, dz = lz - cz;
    const float distSq = dx * dx + dz * dz;
    if (distSq >= radius * radius) {
        return false;  // 无碰撞
    }

    float nlx = lx, nlz = lz;
    const float dist = std::sqrt(distSq);
    if (dist < 1e-5f) {
        // 球心在盒内：沿最短轴推出
        const float penXp = hx - lx, penXn = lx + hx;
        const float penZp = hz - lz, penZn = lz + hz;
        float minPen = penXp;
        int axis = 0;  // 0=x, 1=z
        float sign = 1.0f;
        if (penXn < minPen) { minPen = penXn; axis = 0; sign = -1.0f; }
        if (penZp < minPen) { minPen = penZp; axis = 1; sign = 1.0f; }
        if (penZn < minPen) { minPen = penZn; axis = 1; sign = -1.0f; }
        const float push = radius + minPen;
        if (axis == 0) {
            nlx += sign * push;
        } else {
            nlz += sign * push;
        }
    } else {
        // 球心在盒外但距离 < radius：沿 dx,dz 方向推出
        const float push = radius - dist;
        nlx += (dx / dist) * push;
        nlz += (dz / dist) * push;
    }

    // 只把位移增量旋回世界系叠加
    float wdx, wdz;
    localToWorld2D(nlx - lx, nlz - lz, yaw, wdx, wdz);
    sx += wdx;
    sz += wdz;
    return true;
}

// 放置解算用常量：贴合判定留的缝隙、迭代放宽重推的最大轮数、单次放置允许累计
// 推出的总距离上限（防「深嵌 / 多堵墙连环顶」把箱子推到离机器人很远甚至身后——
// 超限视为几何不可行，place() 直接失败而不是硬塞一个离谱的结果）。
constexpr float kPlaceEps = 1e-3f;
constexpr int kMaxPlaceIter = 8;
constexpr float kMaxPlacePush = 3.0f;

// OBB 在给定世界法向上的投影半径（SAT 用）：half.x*|uX·n| + half.z*|uZ·n|，
// 其中局部 +X 世界方向 uX=(cos,-sin)、+Z 为 uZ=(sin,cos)（与 worldToLocal2D 一致）。
float obbProjRadius(const Obstacle& ob, float nx, float nz) {
    const float c = std::cos(ob.yaw), s = std::sin(ob.yaw);
    return ob.half.x * std::abs(c * nx - s * nz) + ob.half.z * std::abs(s * nx + c * nz);
}

// 两个 OBB 在 XZ 平面是否重叠：2D SAT，候选轴 = 两盒各自局部 X/Z（4 条）。
// 任一轴上投影距离 >= 半径和即分离；用 >=（非 >）使「贴合」不算重叠——放置刚推到
// 相切时不应再触发推出。
bool obbOverlapXZ(const Obstacle& a, const Obstacle& b) {
    const float ca = std::cos(a.yaw), sa = std::sin(a.yaw);
    const float cb = std::cos(b.yaw), sb = std::sin(b.yaw);
    const float axes[4][2] = {
        {ca, -sa}, {sa, ca},  // a 的局部 +X / +Z 世界方向
        {cb, -sb}, {sb, cb},  // b 的局部 +X / +Z 世界方向
    };
    const float dx = b.center.x - a.center.x;
    const float dz = b.center.z - a.center.z;
    for (const auto& ax : axes) {
        if (std::abs(dx * ax[0] + dz * ax[1]) >=
            obbProjRadius(a, ax[0], ax[1]) + obbProjRadius(b, ax[0], ax[1])) {
            return false;
        }
    }
    return true;
}

// 「墙」判定：待放盒从释放高度 releaseBottom 下落到脚下高度 supportTop 附近，途中若
// oth 伸进这条竖直带（bottom < releaseBottom）又不是可踩的「地板」（top > supportTop），
// 放下去就会嵌进去 → 是墙，需要水平推开。顶面不高于脚下的箱是支撑、不推。
bool wallVerticalConflict(const Obstacle& oth, float releaseBottom, float supportTop) {
    return (oth.center.y + oth.half.y) > supportTop && (oth.center.y - oth.half.y) < releaseBottom;
}

// 沿 -f 把待放盒推到与墙 oth 沿 f 严格分离所需的近侧推出量。f 是待放盒的局部
// +X（place 里 ob.yaw=robot.theta()），故是 SAT 候选轴：沿 f 分开即证明两盒不相交、
// 且推的方向天然落在近侧。s = 待放盒心相对墙心在 f 上的投影（s<0 表示在近侧），
// d = s + R + kPlaceEps。注意不是 R-|s|——释放点已越过墙心（s>0）时那样会欠推。
// d<=0 表示无需推。
float nearSidePush(const Obstacle& ob, const Obstacle& oth, float fx, float fz) {
    const float s = (ob.center.x - oth.center.x) * fx + (ob.center.z - oth.center.z) * fz;
    return s + obbProjRadius(ob, fx, fz) + obbProjRadius(oth, fx, fz) + kPlaceEps;
}

// 两盒是否真正 3D 重叠：竖直区间相交 + XZ 投影重叠。place() 写回前用它做最终
// 校验——不信任「墙/支撑」的预分类（那是基于释放高度的启发式），直接按放置后
// 的最终位姿判一次事实，兜底仍不收敛时能被这里拦下而不是悄悄放行。
bool overlaps3D(const Obstacle& a, const Obstacle& b) {
    const bool vOverlap = (a.center.y - a.half.y) < (b.center.y + b.half.y) &&
                          (b.center.y - b.half.y) < (a.center.y + a.half.y);
    return vOverlap && obbOverlapXZ(a, b);
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
    const float footY = robot.y() + Robot::kGroundSnapTol;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == robot.carriedIndex()) continue;  // 该机器人搬运物附着在身上，不参与碰撞
        // 高度感知：顶面不高于脚下的障碍是「地板」（能站上去/跳过去），不推；
        // 只有比脚高的「墙」才把机器人推出 XZ。跳够高越过矮箱即源于此。
        if (obstacles_[i].center.y + obstacles_[i].half.y <= footY) continue;
        float rx = robot.x(), rz = robot.z();
        if (resolveSphereOBB2D(rx, rz, SimConfig::kRobotRadius, obstacles_[i])) {
            robot.setX(rx);
            robot.setZ(rz);
        }
    }
}

float World::supportHeight(const Robot& robot) const {
    float support = 0.0f;  // 缺省地面
    const float footY = robot.y() + Robot::kGroundSnapTol;
    const int carried = robot.carriedIndex();
    const float r2 = SimConfig::kRobotRadius * SimConfig::kRobotRadius;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == carried) continue;  // 背着的箱子不能踩
        const Obstacle& ob = obstacles_[i];
        const float top = ob.center.y + ob.half.y;
        if (top > footY) continue;  // 比脚高的是墙，不是可站的地板
        // 脚下 XZ 圆与盒截面是否重叠（与碰撞同用最近点判定，先旋进盒局部系）
        float lx, lz;
        worldToLocal2D(robot.x() - ob.center.x, robot.z() - ob.center.z, ob.yaw, lx, lz);
        const float cx = std::clamp(lx, -ob.half.x, ob.half.x);
        const float cz = std::clamp(lz, -ob.half.z, ob.half.z);
        const float dx = lx - cx, dz = lz - cz;
        if (dx * dx + dz * dz <= r2 && top > support) {
            support = top;
        }
    }
    return support;
}


float World::queryRange(const Robot& robot, float maxRange) const {
    const float forwardX = std::cos(robot.theta());
    const float forwardZ = -std::sin(robot.theta());
    // 射线从身体前端中心出发
    const float ox = robot.x() + forwardX * (Robot::kBodyLength * 0.5f);
    const float oz = robot.z() + forwardZ * (Robot::kBodyLength * 0.5f);

    float nearest = maxRange;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == robot.carriedIndex()) continue;  // 跳过该机器人搬运的物体
        float t;
        if (rayOBB2D(ox, oz, forwardX, forwardZ, obstacles_[i], t) && t < nearest) {
            nearest = t;
        }
    }
    return nearest;
}

bool World::isObstacleCovered(int idx) const {
    const Obstacle& base = obstacles_[idx];
    const float top = base.center.y + base.half.y;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == idx) continue;
        const Obstacle& other = obstacles_[i];
        const float bottom = other.center.y - other.half.y;
        // 贴合判定：other 的底面落在 base 顶面的 kGroundSnapTol 容差带内，
        // 且两者 XZ 有重叠——即 other 搭在 base 上面（哪怕只搭一个边角，
        // 偏保守：宁可多拒绝拾取，也不悄悄抽掉下层留个悬空的上层）。
        if (std::abs(bottom - top) <= Robot::kGroundSnapTol && obbOverlapXZ(base, other)) {
            return true;
        }
    }
    return false;
}

bool World::pick(Robot& robot) {
    if (robot.isCarrying()) {
        return false;  // 该机器人已搬运
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
        // 计算 OBB 表面最近点到机器人前端的距离：旋进盒局部系按轴对齐解算，
        // 旋转保距故局部系里算出的距离即世界系距离。
        float lox, loz;
        worldToLocal2D(ox - ob.center.x, oz - ob.center.z, ob.yaw, lox, loz);
        const float closestX = std::clamp(lox, -ob.half.x, ob.half.x);
        const float closestZ = std::clamp(loz, -ob.half.z, ob.half.z);
        const float dx = closestX - lox;
        const float dz = closestZ - loz;
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
        if (isObstacleCovered(bestIdx)) {
            return false;  // 上面贴着别的箱子，抽不出来
        }
        robot.setCarried(bestIdx, obstacles_[bestIdx].mass);
        return true;
    }
    return false;
}

bool World::place(Robot& robot) {
    const int idx = robot.carriedIndex();
    if (idx < 0 || idx >= (int)obstacles_.size()) {
        return false;
    }

    // 在局部副本上做几何解算，只有最终校验通过才写回 obstacles_[idx]：
    // 几何不可行（兜底仍重叠、或推出距离超限）时宁可响亮失败、物体留在搬运态，
    // 也不要硬塞一个可能与别的箱子重叠的结果（与本仓「宁可报错不给假数据」同口径）。
    Obstacle candidate = obstacles_[idx];

    // f = 机器人前方，也恰是待放盒的局部 +X（下面 candidate.yaw=robot.theta()），
    // 故是 SAT 候选轴。
    const float fx = std::cos(robot.theta());
    const float fz = -std::sin(robot.theta());
    // 松手瞬间背着的物体底面所在高度。用机器人姿态推算，而不是读 candidate.center.y——
    // pick 与 place 可能落在同一帧的事件批里，那时 updateCarried 还没跑过。
    const float releaseBottom = robot.y() + Robot::kBodyY + Robot::kBodyHeight + kCarryLift;
    // 「地板高度」＝机器人脚下（与 resolveCollisions/supportHeight 同口径）：顶面不高于
    // 此高度的箱是它踩得上的支撑面、可叠放；高于此高度的箱相对它是「墙」、要推开。
    // 「一律落地 + 推开、只在机器人自己抬高（站上箱顶）时叠放」就落在这条线上。
    const float supportTop = robot.y() + Robot::kGroundSnapTol;

    // 先定 XZ 与朝向（y 留到 Phase B 按最终脚印的支撑面算）
    candidate.center.x = robot.x() + fx * (Robot::kBodyLength * 0.5f + kPlaceDist);
    candidate.center.z = robot.z() + fz * (Robot::kBodyLength * 0.5f + kPlaceDist);
    candidate.yaw = robot.theta();

    // Phase A：把待放盒从「墙」（竖直冲突且 XZ 重叠的箱）里沿 -f 水平推开，只往机器人
    // 这一侧推、绝不穿到阻挡箱对面。每轮取最严格的一堵墙推，最多 kMaxPlaceIter 轮。
    // 墙集合只依赖 releaseBottom/supportTop（常量），不依赖落地高度，故无「推↔落地」震荡。
    // 累计推出量超过 kMaxPlacePush 视为不可行（如被巨大/多堵墙连环顶），提前放弃。
    bool resolved = false;
    bool pushExceeded = false;
    float totalPush = 0.0f;
    for (int iter = 0; iter < kMaxPlaceIter; ++iter) {
        float push = 0.0f;
        for (int i = 0; i < (int)obstacles_.size(); ++i) {
            if (i == idx) continue;
            const Obstacle& oth = obstacles_[i];
            if (!wallVerticalConflict(oth, releaseBottom, supportTop)) continue;
            if (!obbOverlapXZ(candidate, oth)) continue;
            push = std::max(push, nearSidePush(candidate, oth, fx, fz));
        }
        if (push <= 0.0f) {
            resolved = true;
            break;
        }
        totalPush += push;
        if (totalPush > kMaxPlacePush) {
            pushExceeded = true;
            break;
        }
        candidate.center.x -= fx * push;
        candidate.center.z -= fz * push;
    }
    if (!resolved && !pushExceeded) {
        // 兜底：迭代未收敛（如两侧都堆着墙）时去掉 overlap 过滤，对全部竖直冲突的墙取
        // 最严格者再推一次，尽量保证不嵌墙；同样受累计推出上限约束。
        float push = 0.0f;
        for (int i = 0; i < (int)obstacles_.size(); ++i) {
            if (i == idx) continue;
            const Obstacle& oth = obstacles_[i];
            if (!wallVerticalConflict(oth, releaseBottom, supportTop)) continue;
            push = std::max(push, nearSidePush(candidate, oth, fx, fz));
        }
        if (push > 0.0f) {
            totalPush += push;
            if (totalPush > kMaxPlacePush) {
                pushExceeded = true;
            } else {
                candidate.center.x -= fx * push;
                candidate.center.z -= fz * push;
            }
        }
    }
    if (pushExceeded) {
        return false;  // 推出距离超限：几何不可行，放弃本次放置（物体仍背着）
    }

    // Phase B：按最终脚印（XZ+yaw）定落地高度——取所有「顶面不高于脚下且脚印重叠」的箱
    // 里最高的顶面作支撑面，没有则落地 0。机器人自己站上箱顶再松手就落在这层箱顶，即叠放。
    float bottom = 0.0f;
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == idx) continue;
        const Obstacle& oth = obstacles_[i];
        const float top = oth.center.y + oth.half.y;
        if (top > supportTop) continue;  // 比脚下高的是墙（Phase A 已处理），不是支撑
        if (!obbOverlapXZ(candidate, oth)) continue;
        if (top > bottom) bottom = top;
    }
    candidate.center.y = bottom + candidate.half.y;

    // 最终校验：不信任上面基于释放高度的「墙/支撑」启发式分类，直接按放置后的
    // 最终位姿对全部障碍做一次事实性 3D 重叠检查。兜底仍未收敛的罕见情形会在
    // 这里被拦下，而不是悄悄放行一个重叠结果。
    for (int i = 0; i < (int)obstacles_.size(); ++i) {
        if (i == idx) continue;
        if (overlaps3D(candidate, obstacles_[i])) {
            return false;  // 兜底仍无法消除重叠：几何不可行，放弃本次放置
        }
    }

    obstacles_[idx] = candidate;
    robot.clearCarried();
    return true;
}

void World::updateCarried(const Robot& robot) {
    const int idx = robot.carriedIndex();
    if (idx < 0 || idx >= (int)obstacles_.size()) {
        return;
    }
    auto& ob = obstacles_[idx];
    // 放在机器人正上方，抬高避免挡住摄像机；y 跟随机器人离地高度（跳起/站箱上时
    // 背着的箱子一起升，否则会相对机体下沉）。
    ob.center.x = robot.x();
    ob.center.z = robot.z();
    ob.center.y = robot.y() + Robot::kBodyY + Robot::kBodyHeight + ob.half.y + kCarryLift;
    // 朝向跟随机体：机器人转向时背着的箱子一起转（可见 + 放置后保留）。
    ob.yaw = robot.theta();
}

bool World::boxesOverlapXZ(const Obstacle& a, const Obstacle& b) { return obbOverlapXZ(a, b); }

void World::applyScene(const SceneState& scene) {
    obstacles_ = scene.obstacles;
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
            if (std::strncmp(text, kSceneMagicV1, sizeof(kSceneMagicV1) - 1) == 0 ||
                std::strncmp(text, kSceneMagicV2, sizeof(kSceneMagicV2) - 1) == 0 ||
                std::strncmp(text, kSceneMagicV3, sizeof(kSceneMagicV3) - 1) == 0) {
                magicSeen = true;
            } else {
                std::fprintf(stderr, "scene: 首个有效行不是魔数 '%s'/'%s'/'%s'，拒绝整个文件\n",
                             kSceneMagicV1, kSceneMagicV2, kSceneMagicV3);
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
            float v[11] = {};
            int got = 0;
            if (!parseFloats(rest, v, 11, got) || (got != 9 && got != 10 && got != 11)) {
                std::fprintf(stderr, "scene: box 行需要 9/10/11 个数（cx cy cz hx hy hz r g b [mass [yaw_deg]]），跳过: %s", text);
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
            ob.mass = (got >= 10) ? v[9]
                                  : 8.0f * ob.half.x * ob.half.y * ob.half.z * SimConfig::kDefaultDensity;
            ob.yaw = (got == 11) ? glm::radians(v[10]) : 0.0f;
            st.obstacles.push_back(ob);
        } else if (std::strcmp(word, "robot") == 0) {
            float v[4] = {};
            int got = 0;
            if (!parseFloats(rest, v, 4, got) || (got != 3 && got != 4)) {
                std::fprintf(stderr, "scene: robot 行需要 3 或 4 个数（x z theta_deg [carried_idx]），跳过: %s", text);
                continue;
            }
            if (st.robots.size() >= kMaxSceneRobots) {
                std::fprintf(stderr, "scene: 机器人数超过上限 %zu，多余忽略\n", kMaxSceneRobots);
                continue;
            }
            RobotSpawn rs;
            rs.x = v[0];
            rs.z = v[1];
            rs.theta = glm::radians(v[2]);
            rs.carriedIdx = (got == 4) ? static_cast<int>(v[3]) : -1;
            st.robots.push_back(rs);
        } else {
            std::fprintf(stderr, "scene: 未知行类型 '%s'（忽略）\n", word);
        }
    }
    std::fclose(f);

    if (!ok || !magicSeen) {
        return false;
    }
    if (st.robots.empty()) {
        st.robots.push_back(RobotSpawn{});  // 缺 robot 行：补一只原点默认
    }
    for (RobotSpawn& rs : st.robots) {
        if (rs.carriedIdx >= (int)st.obstacles.size()) {
            std::fprintf(stderr, "scene: carried_idx=%d 越界（共 %zu 个障碍），按无搬运处理\n",
                         rs.carriedIdx, st.obstacles.size());
            rs.carriedIdx = -1;
        }
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

    std::fprintf(f, "%s\n", kSceneMagicV3);
    std::fprintf(f, "# box cx cy cz hx hy hz r g b [mass（缺省=体积x800kg/m3）[yaw_deg（绕Y朝向，0=轴对齐）]]\n");
    std::fprintf(f, "# robot x z theta_deg [carried_idx]（多机器人 = 多条 robot 行）\n");
    for (const RobotSpawn& rs : state.robots) {
        std::fprintf(f, "robot %.4f %.4f %.4f", rs.x, rs.z, glm::degrees(rs.theta));
        if (rs.carriedIdx >= 0) {
            std::fprintf(f, " %d", rs.carriedIdx);
        }
        std::fputc('\n', f);
    }
    for (const Obstacle& ob : state.obstacles) {
        std::fprintf(f, "box %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f %.2f %.4f\n",
                     ob.center.x, ob.center.y, ob.center.z,
                     ob.half.x, ob.half.y, ob.half.z,
                     ob.color.r, ob.color.g, ob.color.b, ob.mass,
                     glm::degrees(ob.yaw));
    }

    const bool flushed = std::fflush(f) == 0;
    if (!flushed || std::fclose(f) != 0) {
        std::fprintf(stderr, "scene: 写 %s 失败（磁盘满？）\n", tmp.c_str());
        return false;
    }
    // 收窄权限到 0600：地图（与播种出来的那份同源）是用户数据，不该因 fopen 的
    // 缺省 0666&~umask 变成组/他人可读。chmod 失败只影响权限，不阻断保存。
    ::chmod(tmp.c_str(), 0600);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "scene: rename %s -> %s 失败\n", tmp.c_str(), path.c_str());
        return false;
    }
    return true;
}
