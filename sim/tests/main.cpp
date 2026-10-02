// simcore 单元测试：GL 无关的核心逻辑——运动学、相对运动、射线测距、碰撞、
// 搬运、三座 AF_UNIX 桥的线上协议。GLFW/OpenGL 可视化壳不在覆盖范围，那部分
// 由 scripts/sim_agent_demo.sh 的三层端到端链路兜底。
//
// 运行：./sim/build/sim_tests（或 ctest --test-dir sim/build）
// 风格与 test/ 的 Lua 守门一致：不起测试框架，宏断言 + 汇总退出码。
// socket 用例的监听路径带 pid，与开发会话里可能开着的真仿真器互不干扰。

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "action_table.h"
#include "bridge.h"
#include "capture.h"
#include "request_bridge.h"
#include "robot.h"
#include "sensor.h"
#include "sim_config.h"
#include "sim_engine.h"
#include "world.h"

namespace {

int g_total = 0;
int g_failed = 0;

void check(bool cond, const char* what) {
    ++g_total;
    if (!cond) {
        ++g_failed;
        std::fprintf(stderr, "[FAIL] %s\n", what);
    }
}

bool near(float a, float b, float eps) {
    return std::fabs(a - b) <= eps;
}

// 连到指定路径的 AF_UNIX 测试客户端；失败返回 -1。fd 保持阻塞语义。
int connectTo(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void say(int fd, const char* line) {
    (void)::send(fd, line, std::strlen(line), MSG_NOSIGNAL);
}

// 阻塞收一行（到 \n，不含）
std::string recvLine(int fd) {
    std::string line;
    char ch;
    while (line.size() < 255) {
        const ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) {
            break;
        }
        if (ch == '\n') {
            break;
        }
        line += ch;
    }
    return line;
}

bool recvExact(int fd, void* buf, std::size_t len) {
    auto* p = static_cast<char*>(buf);
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::recv(fd, p + off, len - off, 0);
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string testSockPath(const char* tag) {
    return "/tmp/mybot-sim-tests-" + std::to_string(::getpid()) + "-" + tag + ".sock";
}

// ---- Robot：运动学 ----

void testKinematics() {
    Robot r;
    check(r.x() == 0.0f && r.z() == 0.0f && r.theta() == 0.0f, "初始位姿在原点");

    // 满油门 4 秒：v 向 kMaxSpeed 一阶逼近，theta=0 时沿 +X 积分
    for (int i = 0; i < 240; ++i) {
        r.update(1.0f / 60.0f, {1.0f, 0.0f}, Robot::kMaxSpeed);
    }
    check(r.v() > 0.9f * Robot::kMaxSpeed, "油门=1 时 v 逼近 kMaxSpeed");
    check(r.x() > 1.0f, "theta=0 前进 x 增加");
    check(near(r.z(), 0.0f, 1e-3f), "theta=0 前进 z 不动");

    // 松油门：v 快速衰减且不为负
    const float v0 = r.v();
    for (int i = 0; i < 60; ++i) {
        r.update(1.0f / 60.0f, {0.0f, 0.0f}, Robot::kMaxSpeed);
    }
    check(r.v() < v0 * 0.5f, "油门=0 时 v 快速衰减");
    check(r.v() >= 0.0f, "v 不为负");

    // steer>0 = 左转（theta 增加）；steer<0 = 右转
    Robot left;
    for (int i = 0; i < 120; ++i) {
        left.update(1.0f / 60.0f, {0.0f, 1.0f}, Robot::kMaxSpeed);
    }
    check(left.theta() > 0.1f, "steer>0 左转 theta 增加");
    Robot right;
    for (int i = 0; i < 120; ++i) {
        right.update(1.0f / 60.0f, {0.0f, -1.0f}, Robot::kMaxSpeed);
    }
    check(right.theta() < -0.1f, "steer<0 右转 theta 减小");
}

// ---- Robot：相对运动（fdist/turn） ----

void testRelativeMotion() {
    const float dt = 1.0f / 60.0f;

    // tickRelative 到量前提前松油门刹车（尾程= v/kStopRate），实走应贴着目标。
    Robot r;
    r.setRelativeDist(1.0f);
    bool active = true;
    int guard = 0;
    while (active && guard++ < 600) {
        RobotInput in;
        active = r.tickRelative(dt, in);
        r.update(dt, in, Robot::kMaxSpeed);
    }
    check(!active, "fdist 走满后 tickRelative 返回 false");
    check(guard < 600, "fdist 1m 在时限内完成");
    check(near(r.x(), 1.0f, 0.05f), "fdist 1m 实走落在 1.0±0.05");

    // 到量后主循环仍在继续 update（现实情形）：残余动量不再拖走位姿
    // （回归：旧实现满速到量后会多滑 v/6 ≈ 0.27m）
    for (int i = 0; i < 90; ++i) {
        r.update(dt, {0.0f, 0.0f}, Robot::kMaxSpeed);
    }
    check(near(r.x(), 1.0f, 0.05f), "到量后减速尾程不再多滑");

    // 后退 0.5m
    Robot back;
    back.setRelativeDist(-0.5f);
    active = true;
    guard = 0;
    while (active && guard++ < 600) {
        RobotInput in;
        active = back.tickRelative(dt, in);
        back.update(dt, in, Robot::kMaxSpeed);
    }
    check(!active, "负 fdist 亦能完成");
    check(near(back.x(), -0.5f, 0.05f), "fdist -0.5m 实走落在 -0.5±0.05");

    // 右转 90°（回归：旧实现满速到量后角动量尾程会多转 ω/6 ≈ 21°）
    Robot t;
    t.setRelativeTurn(glm::radians(-90.0f));
    active = true;
    guard = 0;
    while (active && guard++ < 600) {
        RobotInput in;
        active = t.tickRelative(dt, in);
        t.update(dt, in, Robot::kMaxSpeed);
    }
    check(!active, "turn 完成后 tickRelative 返回 false");
    for (int i = 0; i < 90; ++i) {
        t.update(dt, {0.0f, 0.0f}, Robot::kMaxSpeed);
    }
    check(near(t.theta(), glm::radians(-90.0f), glm::radians(1.5f)),
          "turn -90° 实转落在 -90°±1.5°（含尾程）");

    // fdist 与 turn 顺序执行：先走完再转，同帧设定也不互相吞
    // （回归：旧实现 setRelativeTurn 会清掉 remainingDist，.mt 里
    //   「fdist 2 后 turn 90」两行连续下发时走路段被静默丢弃）
    Robot seq;
    seq.setRelativeDist(0.5f);
    seq.setRelativeTurn(glm::radians(90.0f));
    active = true;
    guard = 0;
    while (active && guard++ < 1200) {
        RobotInput in;
        active = seq.tickRelative(dt, in);
        seq.update(dt, in, Robot::kMaxSpeed);
    }
    check(!active, "fdist+turn 队列执行完成");
    check(near(seq.x(), 0.5f, 0.05f), "先走完 0.5m");
    check(near(seq.theta(), glm::radians(90.0f), glm::radians(1.5f)), "再转 90°");

    // 无相对运动时 tickRelative 不接管
    Robot idle;
    RobotInput in;
    check(!idle.tickRelative(dt, in), "无相对运动返回 false");
}

// ---- World：射线测距 ----

std::vector<Obstacle> oneBox(float cx, float cz, float mass = 1.0f) {
    return {{{cx, 0.4f, cz}, {0.4f, 0.4f, 0.4f}, {1.0f, 1.0f, 1.0f}, mass}};
}

void testQueryRange() {
    Robot r;  // 原点 theta=0，射线起点 x=+0.2（半车长）
    // 盒中心 3.0 半宽 0.4 → 近面 2.6 → 命中距离 2.6-0.2 = 2.4
    World w(oneBox(3.0f, 0.0f));
    check(near(w.queryRange(r), 2.4f, 1e-3f), "正前方命中距离 = 2.4m");

    World empty(std::vector<Obstacle>{});
    check(near(empty.queryRange(r), Robot::kRangeMax, 1e-6f), "无障碍返回 maxRange");

    // 障碍在身后：射线反向无命中
    World behind(oneBox(-3.0f, 0.0f));
    check(near(behind.queryRange(r), Robot::kRangeMax, 1e-6f), "身后障碍不计入");

    // 转向后测侧方：theta=+90°（朝 -Z）时命中 -Z 方向的盒
    Robot side;
    side.setRelativeTurn(glm::radians(90.0f));
    RobotInput in;
    int guard = 0;
    while (side.tickRelative(1.0f / 60.0f, in) && guard++ < 600) {
        side.update(1.0f / 60.0f, in, Robot::kMaxSpeed);
    }
    World sideBox(oneBox(side.x(), -3.0f));
    check(near(sideBox.queryRange(side), 2.4f, 0.15f), "转 90° 后命中侧方障碍");
}

// ---- World：碰撞 ----

void testCollisions() {
    // 朝盒推进：解算把车推出盒面，车心进不到「近面 - 碰撞半径」以内
    World w(oneBox(2.0f, 0.0f));  // 盒近面 x=1.6
    Robot r;
    for (int i = 0; i < 300; ++i) {
        r.update(1.0f / 60.0f, {1.0f, 0.0f}, Robot::kMaxSpeed);
        w.resolveCollisions(r);
    }
    check(r.x() <= 1.401f, "碰撞把车挡在盒面外（1.6-0.2）");
    check(r.x() > 1.2f, "车确实贴到了盒面");
}

// ---- World：搬运 ----

void testPickPlace() {
    World world(std::vector<Obstacle>{
        {{0.7f, 0.2f, 0.0f}, {0.2f, 0.2f, 0.2f}, {1.0f, 1.0f, 1.0f}, 5.0f},  // 前方近面 0.5
        {{2.0f, 0.2f, 3.0f}, {0.2f, 0.2f, 0.2f}, {1.0f, 1.0f, 1.0f}, 1.0f},  // 远处侧方
    });
    Robot r;
    check(!world.isCarrying(), "初始未搬运");

    // 前端在 0.2、近面在 0.5 → 距离 0.3 < 0.6，可拾取
    check(world.pick(r), "前方 0.3m 的盒可拾取");
    check(world.isCarrying(), "拾取后 isCarrying");
    check(world.carriedIndex() == 0, "拾取的是最近的 idx=0");
    check(near(world.carriedMass(), 5.0f, 1e-6f), "carriedMass = 障碍质量");
    check(world.effectiveMaxSpeed() < Robot::kMaxSpeed, "搬运时最大速度被压低");

    check(!world.pick(r), "已持有物体时再 pick 失败");
    check(near(world.queryRange(r), Robot::kRangeMax, 1e-6f), "被搬运物体不参与测距");

    // 被搬运物体跟随机器人：updateCarried 把它钉在机体正上方
    for (int i = 0; i < 60; ++i) {
        r.update(1.0f / 60.0f, {0.5f, 0.0f}, world.effectiveMaxSpeed());
    }
    world.updateCarried(r);
    check(r.x() > 0.1f, "搬运中机器人仍可前进（减速但不清零）");
    check(near(world.obstacles()[0].center.x, r.x(), 1e-6f), "搬运中物体 x 钉在机体正上方");
    check(world.obstacles()[0].center.y > Robot::kBodyY, "搬运中物体抬到身体上方");

    // place：放到身体前端 + kPlaceDist（0.2+0.5=0.7）
    const float px = r.x();
    check(world.place(r), "place 成功");
    check(!world.isCarrying(), "place 后未搬运");
    check(near(world.obstacles()[0].center.x, px + 0.2f + 0.5f, 1e-3f),
          "物体落在前方 placeDist 处");
    check(!world.place(r), "未持有物体时 place 失败");

    // 拾不到的两种情形：太远 / 在身后
    Robot far;
    World farWorld(oneBox(2.0f, 0.0f));
    World behindWorld(oneBox(-1.0f, 0.0f));
    check(!farWorld.pick(far), "前方 1.8m 的盒拾取失败（超出 0.6m）");
    check(!behindWorld.pick(far), "身后的盒拾取失败");
}

// ---- 场景文件：round-trip 与解析容错 ----

void writeTextFile(const std::string& path, const char* content) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) {
        std::fputs(content, f);
        std::fclose(f);
    }
}

void testSceneIo() {
    const std::string path = "/tmp/mybot-sim-tests-" + std::to_string(::getpid()) + "-scene.scene";

    // round-trip：障碍/位姿/搬运状态
    SceneState st;
    st.obstacles = {
        {{1.0f, 0.2f, 0.0f}, {0.2f, 0.2f, 0.2f}, {1.0f, 0.0f, 0.0f}, 7.5f},
        {{3.0f, 0.2f, 1.0f}, {0.3f, 0.3f, 0.3f}, {0.0f, 1.0f, 0.0f}, 2.0f},
    };
    st.robotX = 1.5f;
    st.robotZ = -0.25f;
    st.robotTheta = 0.7f;
    st.carriedIdx = 1;
    check(writeScene(path, st), "writeScene 成功");
    SceneState back;
    check(readScene(path, back), "readScene 成功");
    check(back.obstacles.size() == 2, "障碍数 round-trip");
    check(near(back.obstacles[1].mass, 2.0f, 1e-4f), "质量 round-trip");
    check(near(back.obstacles[0].center.x, 1.0f, 1e-4f) &&
              near(back.obstacles[1].half.z, 0.3f, 1e-4f) &&
              near(back.obstacles[0].color.g, 0.0f, 1e-4f),
          "盒字段 round-trip");
    check(near(back.robotX, 1.5f, 1e-4f) && near(back.robotZ, -0.25f, 1e-4f) &&
              near(back.robotTheta, 0.7f, 1e-4f),
          "机器人位姿 round-trip");
    check(back.carriedIdx == 1, "搬运索引 round-trip");

    // applyScene：障碍与搬运状态整体替换
    World w(std::vector<Obstacle>{});
    w.applyScene(back);
    check(w.obstacles().size() == 2 && w.isCarrying() && near(w.carriedMass(), 2.0f, 1e-4f),
          "applyScene 恢复障碍与搬运状态");
    check(near(w.effectiveMaxSpeed(), Robot::kMaxSpeed / (1.0f + 2.0f * 0.01f), 1e-4f),
          "applyScene 后搬运减速生效");

    // 注释/空行/未知行/坏行：告警跳过，不毁整个文件
    writeTextFile(path,
                  "mybot-sim scene v1\n"
                  "# 注释行\n"
                  "\n"
                  "box abc def ghi\n"
                  "bogus 1 2 3\n"
                  "box 1 0.2 0 0.2 0.2 0.2 1 0 0\n"
                  "robot 1 2 90\n");
    SceneState st2;
    check(readScene(path, st2), "容错解析成功");
    check(st2.obstacles.size() == 1, "坏行/未知行被跳过");
    check(near(st2.obstacles[0].mass, 51.2f, 0.1f), "缺省质量按体积×800");
    check(near(st2.robotTheta, glm::radians(90.0f), 1e-4f), "theta 度→弧度换算");
    check(st2.carriedIdx == -1, "robot 行无 carried 即无搬运");

    // carried 越界按无搬运处理
    writeTextFile(path, "mybot-sim scene v1\nrobot 0 0 0 5\n");
    check(readScene(path, st2) && st2.carriedIdx == -1, "carried 越界按无搬运处理");

    // 缺魔数：整文件拒绝
    writeTextFile(path, "# 只有注释\nbox 1 2 3 4 5 6 7 8 9\n");
    check(!readScene(path, st2), "缺魔数整文件拒绝");

    // 缺文件
    check(!readScene("/tmp/mybot-sim-tests-not-exist.scene", st2), "缺文件返回 false");

    // 原子写不残留 .tmp
    check(writeScene(path, st), "再次 writeScene 成功");
    const std::string tmp = path + ".tmp";
    FILE* probe = std::fopen(tmp.c_str(), "r");
    check(probe == nullptr, "原子写不残留 .tmp");
    if (probe) {
        std::fclose(probe);
    }

    ::unlink(path.c_str());
    ::unlink(tmp.c_str());
}

// ---- CommandBridge：线上协议（持续型状态 + 一次性事件） ----

void testCommandBridge() {
    const std::string path = testSockPath("cmd");
    CommandBridge bridge(path);
    check(bridge.listening(), "命令桥监听成功");

    int fd = connectTo(path);
    check(fd >= 0, "客户端连上命令桥");

    // 一次性事件：不改变持续型状态
    say(fd, "fdist 2.50\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Stop, "fdist 不改持续型状态");
    SimEvent ev;
    check(bridge.pollEvent(ev) && ev.type == SimEvent::Type::FwdDist && near(ev.param, 2.5f, 1e-4f),
          "fdist 作为事件出队，参数 2.5");
    check(!bridge.pollEvent(ev), "队列为空时 pollEvent 返回 false");

    // 持续型指令
    say(fd, "forward 0.5\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Forward && near(bridge.speed(), 0.5f, 1e-4f),
          "forward 0.5 解析成功");
    check(!bridge.pollEvent(ev), "持续型指令不产生事件");

    // 连续两次 fdist：两条都要触发（回归守门：按枚举边沿触发的旧实现会吞掉第二条）
    say(fd, "fdist 2.0\nfdist 3.5\n");
    bridge.poll();
    check(bridge.pollEvent(ev) && near(ev.param, 2.0f, 1e-4f), "第一条 fdist 入队");
    check(bridge.pollEvent(ev) && near(ev.param, 3.5f, 1e-4f), "第二条 fdist 也入队（不被吞）");
    check(!bridge.pollEvent(ev), "两条 drain 后为空");

    // 垃圾行/坏参数/缺参数：不改状态、不产生事件
    say(fd, "xyzzy\nfdist abc\nturn\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Forward && !bridge.pollEvent(ev),
          "未知指令与坏参数整行忽略");

    // 同帧持续型多行以最后一条为准；一次性按 FIFO 入队
    say(fd, "turn -90\nleft 0.3\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Left && near(bridge.speed(), 0.3f, 1e-4f),
          "同帧多行以最后一条持续型为准");
    check(bridge.pollEvent(ev) && ev.type == SimEvent::Type::Turn && near(ev.param, -90.0f, 1e-4f),
          "turn -90 按序入队");
    check(!bridge.pollEvent(ev), "队列 drain 完");

    // pick/place 也是事件
    say(fd, "pick\nplace\n");
    bridge.poll();
    check(bridge.pollEvent(ev) && ev.type == SimEvent::Type::Pick, "pick 入队");
    check(bridge.pollEvent(ev) && ev.type == SimEvent::Type::Place, "place 入队");

    // save 同为事件（线上不带路径——路径只来自本进程的 argv/env）
    say(fd, "save\n");
    bridge.poll();
    check(bridge.pollEvent(ev) && ev.type == SimEvent::Type::Save, "save 入队");

    // 半关写端（socat 手测模式）：事件不能被 EOF 连带清掉
    say(fd, "fdist 3\n");
    ::shutdown(fd, SHUT_WR);
    bridge.poll();
    check(bridge.pollEvent(ev) && ev.type == SimEvent::Type::FwdDist && near(ev.param, 3.0f, 1e-4f),
          "半关写端后事件仍被处理（延迟断开）");
    bridge.poll();
    check(bridge.command() == SimCommand::Stop, "EOF 处理完后回落 Stop");

    // 新连接顶掉旧连接
    ::close(fd);
    const int fd2 = connectTo(path);
    check(fd2 >= 0, "重连成功");
    say(fd2, "stop\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Stop, "新连接顶掉旧连接并接受指令");

    // 事件队列有界：灌超过上限的事件，多的被丢并告警
    const int fd3 = connectTo(path);
    for (int i = 0; i < 40; ++i) {
        say(fd3, "pick\n");
    }
    bridge.poll();
    int drained = 0;
    while (bridge.pollEvent(ev)) {
        ++drained;
    }
    check(drained == 32, "事件队列上限 32，超出的被丢");

    // 灌垃圾超限：断开并回落 Stop
    const std::string junk(5000, 'x');
    (void)::send(fd3, junk.data(), junk.size(), MSG_NOSIGNAL);
    bridge.poll();
    check(bridge.command() == SimCommand::Stop, "超限垃圾被断开并回落 Stop");
    ::close(fd3);
}

// ---- SensorBridge：请求-响应 ----

void testSensorBridge() {
    const std::string path = testSockPath("sensor");
    SensorBridge sensor(path);
    check(sensor.listening(), "测距桥监听成功");

    const int fd = connectTo(path);
    check(fd >= 0, "客户端连上测距桥");

    say(fd, "range\n");
    check(sensor.poll(), "range 请求被检出");
    check(sensor.sendRange(237), "sendRange 成功");
    check(recvLine(fd) == "OK 237", "响应行 = 'OK 237'");

    say(fd, "bogus\n");
    check(!sensor.poll(), "未知请求不置位");
    say(fd, "range\n");
    check(sensor.poll(), "再次 range 仍可检出");
    check(sensor.sendError("boom"), "sendError 成功");
    check(recvLine(fd) == "ERR boom", "错误响应行 = 'ERR boom'");

    // 半关写端（socat 手测模式）：请求不能再被 EOF 连带清掉，答复要发得出去
    say(fd, "range\n");
    ::shutdown(fd, SHUT_WR);
    check(sensor.poll(), "半关写端后 range 请求仍被检出（延迟断开）");
    check(sensor.sendRange(4321), "半关连接上答复仍可发送");
    check(recvLine(fd) == "OK 4321", "半关连接答复 = 'OK 4321'");

    ::close(fd);
}

// ---- CaptureBridge：请求-响应 + 二进制体 ----

void testCaptureBridge() {
    const std::string path = testSockPath("cap");
    CaptureBridge capture(path);
    check(capture.listening(), "拍照桥监听成功");

    const int fd = connectTo(path);
    check(fd >= 0, "客户端连上拍照桥");

    say(fd, "capture\n");
    check(capture.poll(), "capture 请求被检出");

    const unsigned char fake[4] = {0xFF, 0xD8, 0xFF, 0xE0};
    check(capture.sendFrame(fake, sizeof(fake), 2, 2), "sendFrame 成功");
    check(recvLine(fd) == "OK jpeg 4 2 2", "帧头 = 'OK jpeg 4 2 2'");
    unsigned char body[4] = {};
    check(recvExact(fd, body, sizeof(body)), "帧体收满");
    check(std::memcmp(body, fake, sizeof(fake)) == 0, "帧体逐字节一致");

    say(fd, "bogus\n");
    check(!capture.poll(), "未知请求不置位");

    // 半关写端（socat 手测模式）：请求不能再被 EOF 连带清掉，答复要发得出去
    say(fd, "capture\n");
    ::shutdown(fd, SHUT_WR);
    check(capture.poll(), "半关写端后 capture 请求仍被检出（延迟断开）");
    check(capture.sendFrame(fake, sizeof(fake), 2, 2), "半关连接上答复仍可发送");
    check(recvLine(fd) == "OK jpeg 4 2 2", "半关连接帧头一致");
    check(recvExact(fd, body, sizeof(body)), "半关连接帧体收满");

    ::close(fd);
}

// ---- 动作表：内部不变量 + 与 modules/Sim.lua 的一致性守门 ----

#ifndef MYBOT_MODULES_DIR
#define MYBOT_MODULES_DIR ""
#endif

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 取 marker 之后第一个平衡的 { ... } 块内部内容；找不到返回空串。
std::string braceBlock(const std::string& src, const std::string& marker) {
    const std::size_t m = src.find(marker);
    if (m == std::string::npos) {
        return {};
    }
    const std::size_t open = src.find('{', m);
    if (open == std::string::npos) {
        return {};
    }
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
        } else if (src[i] == '}' && --depth == 0) {
            return src.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

// 解析形如 [0] = "stop" 的条目 → {code: name}
std::map<int, std::string> parseCodeNames(const std::string& block) {
    std::map<int, std::string> out;
    for (std::size_t i = 0; (i = block.find('[', i)) != std::string::npos;) {
        int code = 0;
        char name[64] = {};
        int consumed = 0;
        if (std::sscanf(block.c_str() + i, "[%d] = \"%63[^\"]\"%n", &code, name, &consumed) == 2) {
            out[code] = name;
            i += static_cast<std::size_t>(consumed);
        } else {
            ++i;
        }
    }
    return out;
}

// 解析形如 [5] = true 的条目 → {code}
std::set<int> parseCodeSet(const std::string& block) {
    std::set<int> out;
    for (std::size_t i = 0; (i = block.find('[', i)) != std::string::npos;) {
        int code = 0;
        int consumed = 0;
        if (std::sscanf(block.c_str() + i, "[%d] = true%n", &code, &consumed) == 1) {
            out.insert(code);
            i += static_cast<std::size_t>(consumed);
        } else {
            ++i;
        }
    }
    return out;
}

// 取 marker 之后第一个双引号字符串字面量的内容；找不到返回空串。
std::string quotedAfter(const std::string& src, const std::string& marker) {
    const std::size_t m = src.find(marker);
    if (m == std::string::npos) {
        return {};
    }
    const std::size_t q1 = src.find('"', m);
    if (q1 == std::string::npos) {
        return {};
    }
    const std::size_t q2 = src.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return {};
    }
    return src.substr(q1 + 1, q2 - q1 - 1);
}

void testActionTable() {
    // 内部不变量：码连续覆盖 0..kMaxActionCode、每码唯一、规范名可反查
    check(kActionCount == kMaxActionCode + 1, "动作表条目数 = 最大码 + 1");
    for (int code = 0; code <= kMaxActionCode; ++code) {
        const ActionDef* a = findActionByCode(code);
        check(a != nullptr, "每个数字码都有定义");
        if (a != nullptr) {
            check(a->code == code, "码与下标一致");
            check(findActionByWireName(a->wireName) == a, "规范名可反查到同一行");
        }
    }
    check(findActionByCode(kMaxActionCode + 1) == nullptr, "越界码返回 nullptr");
    check(findActionByWireName("nosuchaction") == nullptr, "未知动作名返回 nullptr");
    // backward 的别名 back 命中同一行
    check(findActionByWireName("back") == findActionByWireName("backward"), "别名 back 命中 backward");

    // 与 modules/Sim.lua 的一致性：CODES / PARAM_CODES / CODE_MAX 必须与表相符。
    // 加动作时若只改一侧，这里会红——把「漏改」从运行期提前到测试期。
    const std::string lua = slurp(std::string(MYBOT_MODULES_DIR) + "/Sim.lua");
    if (lua.empty()) {
        check(false, "读到 modules/Sim.lua（MYBOT_MODULES_DIR 是否正确）");
        return;
    }
    const std::map<int, std::string> codes = parseCodeNames(braceBlock(lua, "local CODES ="));
    const std::set<int> params = parseCodeSet(braceBlock(lua, "local PARAM_CODES ="));

    int codeMax = -1;
    const std::size_t cmPos = lua.find("CODE_MAX");
    if (cmPos != std::string::npos) {
        std::sscanf(lua.c_str() + cmPos, "CODE_MAX = %d", &codeMax);
    }
    check(codeMax == kMaxActionCode, "Lua CODE_MAX 与表最大码一致");
    check(static_cast<int>(codes.size()) == kActionCount, "Lua CODES 条目数与表一致（无多余/缺失）");

    for (int i = 0; i < kActionCount; ++i) {
        const ActionDef& a = kActions[i];
        const auto it = codes.find(a.code);
        check(it != codes.end() && it->second == a.wireName, "Lua CODES 动作名与表一致");
        const bool luaNeedsParam = params.count(a.code) != 0;
        check(luaNeedsParam == a.needsParam, "Lua PARAM_CODES 与表 needsParam 一致");
    }
}

// 缺省 socket 路径是 C++/Lua 两侧的对外契约。两套构建系统之间不做代码生成
// （跨 build 生成脆弱，同阶段1 动作码守门的取舍），改由这里运行期读
// modules/Sim.lua 交叉校验字面量一致——任一侧改了缺省路径而漏改另一侧即红。
// 拍照路径 Camera.lua 只取自 env、无硬编码缺省，故不参与校验。
void testSharedConstants() {
    const std::string lua = slurp(std::string(MYBOT_MODULES_DIR) + "/Sim.lua");
    if (lua.empty()) {
        check(false, "读到 modules/Sim.lua（MYBOT_MODULES_DIR 是否正确）");
        return;
    }
    check(quotedAfter(lua, "local kDefaultSock =") == SimConfig::kDefaultSocketPath,
          "Lua 命令 socket 缺省路径与 SimConfig 一致");
    check(quotedAfter(lua, "local kSensorDefaultSock =") == SimConfig::kDefaultSensorSocketPath,
          "Lua sensor socket 缺省路径与 SimConfig 一致");

    // 三条缺省路径互不相同（防复制粘贴串台），且都是 /tmp 下绝对路径
    const std::string cmd = SimConfig::kDefaultSocketPath;
    const std::string cap = SimConfig::kDefaultCaptureSocketPath;
    const std::string sen = SimConfig::kDefaultSensorSocketPath;
    check(cmd != cap && cmd != sen && cap != sen, "三条缺省 socket 路径互不相同");
    check(cmd.rfind("/tmp/", 0) == 0 && cap.rfind("/tmp/", 0) == 0 && sen.rfind("/tmp/", 0) == 0,
          "缺省路径均为 /tmp 下绝对路径");
}

// 遍历动作表逐条过命令桥：证明 handleLine 对每个动作都按表分派（持续型改状态、
// 一次性入队），新增动作只要表里加一行就被这条守门覆盖。
void testActionTableDispatch() {
    const std::string path = testSockPath("dispatch");
    CommandBridge bridge(path);
    check(bridge.listening(), "命令桥监听成功（表驱动派发）");
    const int fd = connectTo(path);
    check(fd >= 0, "客户端连上命令桥");

    for (int i = 0; i < kActionCount; ++i) {
        const ActionDef& a = kActions[i];
        SimEvent ev;
        while (bridge.pollEvent(ev)) {
        }  // 清空上一条残留

        char line[64];
        if (a.kind == ActionKind::OneShot) {
            if (a.needsParam) {
                std::snprintf(line, sizeof(line), "%s 2.5\n", a.wireName);
            } else {
                std::snprintf(line, sizeof(line), "%s\n", a.wireName);
            }
            say(fd, line);
            bridge.poll();
            check(bridge.pollEvent(ev) && ev.type == a.oneShot, "一次性动作按表入队");
            if (a.needsParam) {
                check(near(ev.param, 2.5f, 1e-4f), "带参动作参数被解析");
            }
        } else {
            std::snprintf(line, sizeof(line), "%s\n", a.wireName);
            say(fd, line);
            bridge.poll();
            check(bridge.command() == a.continuous, "持续型动作按表改状态");
            check(!bridge.pollEvent(ev), "持续型动作不产生事件");
        }
    }
    ::close(fd);
}

// ---- 请求-应答基类：证明「新增一个同步传感器」的扩展成本 ----
// 派生 RequestResponseBridge、给个关键字、写一个 sendXxx 即可，无需碰 capture/sensor。
class PingBridge : public RequestResponseBridge {
public:
    explicit PingBridge(const std::string& socketPath)
        : RequestResponseBridge(socketPath, "ping", "ping") {}

    bool sendPong(int n) {
        if (!beginResponse()) {
            return false;
        }
        char msg[32];
        const int k = std::snprintf(msg, sizeof(msg), "OK pong %d\n", n);
        return sendAll(msg, static_cast<std::size_t>(k));
    }
};

void testRequestResponseBase() {
    const std::string path = testSockPath("ping");
    PingBridge ping(path);
    check(ping.listening(), "派生桥监听成功");
    const int fd = connectTo(path);
    check(fd >= 0, "客户端连上派生桥");

    say(fd, "bogus\n");
    check(!ping.poll(), "未知关键字不置位");
    say(fd, "ping\n");
    check(ping.poll(), "配置的关键字命中置位");
    check(ping.sendPong(7), "子类 sendXxx 成功");
    check(recvLine(fd) == "OK pong 7", "子类 OK 帧正确");
    check(ping.sendError("nope"), "基类 sendError 可用");
    check(recvLine(fd) == "ERR nope", "基类 ERR 帧正确");

    // 半关写端（socat 手测模式）：请求不被 EOF 连带清掉，答复仍发得出去
    say(fd, "ping\n");
    ::shutdown(fd, SHUT_WR);
    check(ping.poll(), "半关写端后请求仍被检出（延迟断开）");
    check(ping.sendPong(9), "半关连接上答复仍可发送");
    check(recvLine(fd) == "OK pong 9", "半关连接答复正确");

    ::close(fd);
}

// ---- SimEngine：headless 验证仿真逻辑（原本困在 GL 壳里，无法单测） ----

std::string engineScenePath(const char* tag) {
    return "/tmp/mybot-sim-tests-" + std::to_string(::getpid()) + "-engine-" + tag + ".scene";
}

SimEvent mkEvent(SimEvent::Type type, float param = 0.0f) {
    SimEvent ev;
    ev.type = type;
    ev.param = param;
    return ev;
}

void testSimEngine() {
    const float dt = 1.0f / 60.0f;

    // 无障碍场景：持续型推进与相对运动都不受碰撞干扰
    const std::string emptyPath = engineScenePath("empty");
    writeTextFile(emptyPath, "mybot-sim scene v1\nrobot 0 0 0\n");
    // 拾取场景：正前方 0.3m 处一个可搬盒（近面在拾取半径内）
    const std::string pickPath = engineScenePath("pick");
    writeTextFile(pickPath,
                  "mybot-sim scene v1\n"
                  "robot 0 0 0\n"
                  "box 0.5 0.2 0 0.2 0.2 0.2 1 1 1 5\n");

    // ① 持续型前进：油门=1 沿 +X 积分；stop 后减速
    {
        SimEngine eng(emptyPath);
        eng.reloadScene();
        check(!eng.sceneName().empty() && eng.sceneName().find('/') == std::string::npos,
              "sceneName 取自路径 basename（无斜杠、非空）");
        eng.applyContinuous(SimCommand::Forward, 1.0f);
        for (int i = 0; i < 120; ++i) {
            eng.step(dt);
        }
        check(eng.robot().x() > 1.0f, "持续型 forward 推进 +X");
        check(near(eng.robot().z(), 0.0f, 1e-3f), "theta=0 前进不偏 z");
        eng.applyContinuous(SimCommand::Stop, 1.0f);
        const float v0 = eng.robot().v();
        for (int i = 0; i < 60; ++i) {
            eng.step(dt);
        }
        check(eng.robot().v() < v0 * 0.5f, "stop 后速度衰减");
    }

    // ② 一次性 fdist/turn：引擎 step 内 tickRelative 到量自动停
    {
        SimEngine eng(emptyPath);
        eng.reloadScene();
        eng.applyEvent(mkEvent(SimEvent::Type::FwdDist, 1.0f));
        for (int i = 0; i < 300; ++i) {
            eng.step(dt);
        }
        check(near(eng.robot().x(), 1.0f, 0.05f), "fdist 1m 实走落在 1.0±0.05");

        eng.applyEvent(mkEvent(SimEvent::Type::Turn, 90.0f));
        for (int i = 0; i < 300; ++i) {
            eng.step(dt);
        }
        check(near(eng.robot().theta(), glm::radians(90.0f), glm::radians(1.5f)),
              "turn 90° 实转落在 90°±1.5°");
    }

    // ③ 新持续型指令打断进行中的相对运动（stop 不打断）
    {
        SimEngine eng(emptyPath);
        eng.reloadScene();
        eng.applyEvent(mkEvent(SimEvent::Type::FwdDist, 1.0f));
        for (int i = 0; i < 20; ++i) {
            eng.step(dt);  // 走一段，未到量
        }
        eng.applyContinuous(SimCommand::Forward, 1.0f);  // 打断 fdist
        for (int i = 0; i < 300; ++i) {
            eng.step(dt);
        }
        check(eng.robot().x() > 2.0f, "持续型 forward 打断 fdist 后继续前进（不止于 1m）");
    }

    // ④ pick/place 经引擎事件派发
    {
        SimEngine eng(pickPath);
        eng.reloadScene();
        check(!eng.world().isCarrying(), "初始未搬运");
        eng.applyEvent(mkEvent(SimEvent::Type::Pick));
        check(eng.world().isCarrying(), "pick 事件后 isCarrying");
        check(eng.world().effectiveMaxSpeed() < Robot::kMaxSpeed, "搬运后最大速度被压低");
        // 搬运中前进，物体跟随
        eng.applyContinuous(SimCommand::Forward, 1.0f);
        for (int i = 0; i < 60; ++i) {
            eng.step(dt);
        }
        check(near(eng.world().obstacles()[0].center.x, eng.robot().x(), 1e-4f),
              "搬运中物体跟随机体");
        eng.applyContinuous(SimCommand::Stop, 1.0f);
        const float px = eng.robot().x();
        eng.applyEvent(mkEvent(SimEvent::Type::Place));
        check(!eng.world().isCarrying(), "place 事件后未搬运");
        check(eng.world().obstacles()[0].center.x > px, "物体放到机体前方");
    }

    // ⑤ save/reload round-trip：save 落盘当前位姿，reload 从文件恢复
    {
        const std::string savePath = engineScenePath("save");
        writeTextFile(savePath, "mybot-sim scene v1\nrobot 0 0 0\n");
        SimEngine eng(savePath);
        eng.reloadScene();
        eng.applyEvent(mkEvent(SimEvent::Type::FwdDist, 1.0f));
        for (int i = 0; i < 300; ++i) {
            eng.step(dt);
        }
        const float savedX = eng.robot().x();
        check(near(savedX, 1.0f, 0.05f), "save 前走到 ~1m");
        eng.applyEvent(mkEvent(SimEvent::Type::Save));  // 写回文件
        // 再走一段，位姿前移
        eng.applyEvent(mkEvent(SimEvent::Type::FwdDist, 1.0f));
        for (int i = 0; i < 300; ++i) {
            eng.step(dt);
        }
        check(eng.robot().x() > savedX + 0.5f, "reload 前位姿已前移");
        eng.reloadScene();  // 从刚保存的文件重载
        check(near(eng.robot().x(), savedX, 1e-3f), "reload 恢复到 save 时的位姿");
        ::unlink(savePath.c_str());
    }

    ::unlink(emptyPath.c_str());
    ::unlink(pickPath.c_str());
}

}  // namespace

int main() {
    testKinematics();
    testRelativeMotion();
    testQueryRange();
    testCollisions();
    testPickPlace();
    testSceneIo();
    testActionTable();
    testSharedConstants();
    testCommandBridge();
    testActionTableDispatch();
    testSensorBridge();
    testCaptureBridge();
    testRequestResponseBase();
    testSimEngine();

    std::printf("sim_tests: %d 通过, %d 失败\n", g_total - g_failed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
