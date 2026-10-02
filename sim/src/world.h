#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

#include "robot.h"

// 场景障碍物：盒体（center=中心, half=半尺寸, color=漫反射色, mass=质量kg）。
// mass 用于搬运时的减速计算，defaultScene() 按体积×密度填充。
struct Obstacle {
    glm::vec3 center;
    glm::vec3 half;
    glm::vec3 color;
    float mass = 1.0f;
};

// 场景文件的内存形态。theta 用弧度（文件里是度，读写边界换算）。
struct SceneState {
    std::vector<Obstacle> obstacles;
    float robotX = 0.0f;
    float robotZ = 0.0f;
    float robotTheta = 0.0f;  // 弧度
    int carriedIdx = -1;      // 搬运中的障碍下标（-1=无）
};

// 仿真场景：障碍物集合 + 机器人与场景的相互作用（碰撞推出、射线测距、
// 拾取/放置/背负）。Robot 只管运动学；「世界里有什么、撞了怎么解、搬得动
// 什么」都在这里。GL 无关，归 simcore。
class World {
public:
    // 装载给定障碍（不改动质量，测试用自定义场景）；缺省装载 defaultScene()
    explicit World(std::vector<Obstacle> obstacles = defaultScene());

    std::vector<Obstacle>& obstacles() { return obstacles_; }
    const std::vector<Obstacle>& obstacles() const { return obstacles_; }

    // 球（XZ 平面）vs 盒截面碰撞解算：把机器人推出盒面（逐障碍解算，不迭代）。
    // 被搬运的物体附着在机器人上，跳过不参与碰撞。
    void resolveCollisions(Robot& robot);

    // 机器人前方射线与障碍求交，返回最近命中距离（米）；无命中返回 maxRange。
    // 射线从身体前端中心出发、沿 heading 方向；被搬运的物体不计入。
    float queryRange(const Robot& robot, float maxRange = Robot::kRangeMax) const;

    // 搬运设备：拾取/放置前方物体。
    // pick: 尝试拾取前方最近的可搬运物体（距离 < kPickRange 且在前方），成功
    // 返回 true；已持有物体时直接失败。
    // place: 把当前搬运的物体放到前方 kPlaceDist 处，成功返回 true。
    bool pick(const Robot& robot);
    bool place(const Robot& robot);
    bool isCarrying() const { return carriedIdx_ >= 0; }
    int carriedIndex() const { return carriedIdx_; }
    float carriedMass() const { return carriedMass_; }

    // 考虑搬运负载后的最大速度（m/s）：质量越大越慢。
    float effectiveMaxSpeed() const;

    // 搬运中的物体跟随机器人移动：钉在身体正上方（抬高避免挡住摄像机）
    void updateCarried(const Robot& robot);

    // 整体替换场景（障碍 + 搬运状态）；carriedIdx 越界按无搬运处理
    void applyScene(const SceneState& scene);

    static constexpr float kPickRange = 0.6f;  // 拾取半径（米）
    static constexpr float kPlaceDist = 0.5f;  // 放置距离（米）

    // 默认障碍场景：蜘蛛从原点出发沿 +X 前进，障碍散布在前进路线两侧，
    // 质量按体积×密度（近似木材）填充。
    static std::vector<Obstacle> defaultScene();

private:
    std::vector<Obstacle> obstacles_;
    int carriedIdx_ = -1;          // 当前搬运的障碍物索引（-1=未搬运）
    float carriedMass_ = 0.0f;     // 搬运物质量缓存
};

// ---- 场景文件读写（行文本，与三桥协议同风格，可手编可 diff） ----
//
// 格式 v1：首行必须是魔数 "mybot-sim scene v1"，之后每行一条——
//   box <cx> <cy> <cz> <hx> <hy> <hz> <r> <g> <b> [<mass>]
//   robot <x> <z> <theta_deg> [<carried_idx>]
// # 开头的注释与空行忽略；行内 '#' 起视为注释。未知/坏行告警后跳过
// （与命令桥「垃圾不打断当前状态」同口径）；mass 缺省按体积×800kg/m³。
//
// 读失败（缺文件/缺魔数/超障碍数上限）返回 false 并置 stderr。
bool readScene(const std::string& path, SceneState& out);

// 原子写：先落 <path>.tmp 再 rename，写一半崩溃不会毁掉已有场景。
bool writeScene(const std::string& path, const SceneState& state);
