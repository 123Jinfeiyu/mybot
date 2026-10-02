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

// 场景里一只机器人的初始状态。theta 用弧度（文件里是度，读写边界换算）。
struct RobotSpawn {
    float x = 0.0f;
    float z = 0.0f;
    float theta = 0.0f;   // 弧度
    int carriedIdx = -1;  // 装载时该机器人已搬运的障碍下标（-1=无）
};

// 场景文件的内存形态：障碍集合 + N 只机器人。v1 文件读成 1 只、v2 可多只；
// readScene 保证 robots 至少 1 只（缺 robot 行补一只原点默认）。
struct SceneState {
    std::vector<Obstacle> obstacles;
    std::vector<RobotSpawn> robots;
};

// 仿真场景：障碍物集合 + 机器人与场景的相互作用（碰撞推出、射线测距、
// 拾取/放置/背负跟随）。Robot 管运动学与**自己背着哪个障碍**；「世界里有什么、
// 撞了怎么解、能不能搬」都在这里。所有交互都按传入的 Robot 参数化、World 自身
// 不持有任何单只机器人的状态——因此同一 World 可服务 N 只机器人，各搬各的。
// GL 无关，归 simcore。
class World {
public:
    // 装载给定障碍（不改动质量，测试用自定义场景）；缺省装载 defaultScene()
    explicit World(std::vector<Obstacle> obstacles = defaultScene());

    std::vector<Obstacle>& obstacles() { return obstacles_; }
    const std::vector<Obstacle>& obstacles() const { return obstacles_; }

    // 球（XZ 平面）vs 盒截面碰撞解算：把机器人推出盒面（逐障碍解算，不迭代）。
    // **高度感知**：顶面 ≤ 机器人脚下 y_+kGroundSnapTol 的障碍视为「地板」（可站/
    // 可跳过），不产生推出；只有比脚高的「墙」才推。故跳跃 apex 高过箱顶即可越过。
    // 该机器人正搬运的物体附着在它身上，跳过不参与碰撞。
    void resolveCollisions(Robot& robot);

    // 机器人脚下的支撑面高度（米）：脚下 XZ 圆（半径 kRobotRadius）重叠、且顶面
    // 不高于脚下（y_+kGroundSnapTol）的所有障碍里最高的那个顶面；无则地面 0。
    // SimEngine 每帧用它给 Robot.applySupport()——落地吸附、走下箱沿转坠落都靠它。
    // 该机器人搬运的物体不计入（背着的箱子不能踩）。
    float supportHeight(const Robot& robot) const;

    // 机器人前方射线与障碍求交，返回最近命中距离（米）；无命中返回 maxRange。
    // 射线从身体前端中心出发、沿 heading 方向；该机器人搬运的物体不计入。
    float queryRange(const Robot& robot, float maxRange = Robot::kRangeMax) const;

    // 搬运设备：拾取/放置前方物体。搬运状态写回**传入的这只 Robot**（各背各的）。
    // pick: 尝试拾取前方最近的可搬运物体（距离 < kPickRange 且在前方），成功
    // 返回 true；该机器人已持有物体时直接失败。
    // place: 把该机器人当前搬运的物体放到前方 kPlaceDist 处，成功返回 true。
    bool pick(Robot& robot);
    bool place(Robot& robot);

    // 该机器人搬运中的物体跟随它移动：钉在身体正上方（抬高避免挡住摄像机）
    void updateCarried(const Robot& robot);

    // 整体替换障碍集合。搬运状态属于每只 Robot、不由场景承载——重载后由
    // SimEngine 按 SceneState.carriedIdx 恢复到主机器人身上。carriedIdx 越界
    // 由 readScene 归一为 -1。
    void applyScene(const SceneState& scene);

    static constexpr float kPickRange = 0.6f;  // 拾取半径（米）
    static constexpr float kPlaceDist = 0.5f;  // 放置距离（米）

    // 默认障碍场景：蜘蛛从原点出发沿 +X 前进，障碍散布在前进路线两侧，
    // 质量按体积×密度（近似木材）填充。
    static std::vector<Obstacle> defaultScene();

private:
    std::vector<Obstacle> obstacles_;
};

// ---- 场景文件读写（行文本，与三桥协议同风格，可手编可 diff） ----
//
// 首行必须是魔数 "mybot-sim scene v1" 或 "mybot-sim scene v2"，之后每行一条——
//   box <cx> <cy> <cz> <hx> <hy> <hz> <r> <g> <b> [<mass>]
//   robot <x> <z> <theta_deg> [<carried_idx>]
// v1 与 v2 的差别只在 robot 行数：v1 恒 1 只，v2 允许**多条 robot 行**（多机器人）。
// readScene 两者都收，按出现顺序读进 SceneState.robots；writeScene 一律写 v2
// （v2 是 v1 的超集，单机器人也写 v2）。# 开头的注释与空行忽略；行内 '#' 起视为
// 注释。未知/坏行告警后跳过（与命令桥「垃圾不打断当前状态」同口径）；mass 缺省按
// 体积×800kg/m³；无 robot 行补一只原点默认。
//
// 读失败（缺文件/缺魔数/超障碍数上限）返回 false 并置 stderr。
bool readScene(const std::string& path, SceneState& out);

// 原子写：先落 <path>.tmp 再 rename，写一半崩溃不会毁掉已有场景。写 v2 魔数。
bool writeScene(const std::string& path, const SceneState& state);
