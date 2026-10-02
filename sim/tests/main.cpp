// simcore 单元测试：GL 无关的核心逻辑——运动学、相对运动、射线测距、搬运、
// 三座 AF_UNIX 桥的线上协议。GLFW/OpenGL 可视化壳不在覆盖范围，那部分由
// scripts/sim_agent_demo.sh 的三层端到端链路兜底。
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
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "bridge.h"
#include "capture.h"
#include "robot.h"
#include "sensor.h"

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
        r.update(1.0f / 60.0f, {1.0f, 0.0f});
    }
    check(r.v() > 0.9f * Robot::kMaxSpeed, "油门=1 时 v 逼近 kMaxSpeed");
    check(r.x() > 1.0f, "theta=0 前进 x 增加");
    check(near(r.z(), 0.0f, 1e-3f), "theta=0 前进 z 不动");

    // 松油门：v 快速衰减且不为负
    const float v0 = r.v();
    for (int i = 0; i < 60; ++i) {
        r.update(1.0f / 60.0f, {0.0f, 0.0f});
    }
    check(r.v() < v0 * 0.5f, "油门=0 时 v 快速衰减");
    check(r.v() >= 0.0f, "v 不为负");

    // steer>0 = 左转（theta 增加）；steer<0 = 右转
    Robot left;
    for (int i = 0; i < 120; ++i) {
        left.update(1.0f / 60.0f, {0.0f, 1.0f});
    }
    check(left.theta() > 0.1f, "steer>0 左转 theta 增加");
    Robot right;
    for (int i = 0; i < 120; ++i) {
        right.update(1.0f / 60.0f, {0.0f, -1.0f});
    }
    check(right.theta() < -0.1f, "steer<0 右转 theta 减小");
}

// ---- Robot：相对运动（fdist/turn） ----

void testRelativeMotion() {
    const float dt = 1.0f / 60.0f;

    // 记账口径：tickRelative 按上一帧速度减剩余量，位置按更新后速度积分，
    // 到量那帧车仍在满速——实走会比目标多出至多约 2*v*dt（60fps 下 ~5cm）。
    // 这是现有实现的固有过冲，断言按区间核对而不是掐死在目标值上。
    Robot r;
    r.setRelativeDist(1.0f);
    bool active = true;
    int guard = 0;
    while (active && guard++ < 600) {
        RobotInput in;
        active = r.tickRelative(dt, in);
        r.update(dt, in);
    }
    check(!active, "fdist 走满后 tickRelative 返回 false");
    check(guard < 600, "fdist 1m 在时限内完成");
    check(r.x() >= 0.95f && r.x() <= 1.10f, "fdist 1m 实走落在 [0.95, 1.10]");

    // 后退 0.5m
    Robot back;
    back.setRelativeDist(-0.5f);
    active = true;
    guard = 0;
    while (active && guard++ < 600) {
        RobotInput in;
        active = back.tickRelative(dt, in);
        back.update(dt, in);
    }
    check(!active, "负 fdist 亦能完成");
    check(back.x() <= -0.45f && back.x() >= -0.60f, "fdist -0.5m 实走落在 [-0.60, -0.45]");

    // 右转 90°（过冲同上，至多约 2*ωmax*dt ≈ 4°）
    Robot t;
    t.setRelativeTurn(glm::radians(-90.0f));
    active = true;
    guard = 0;
    while (active && guard++ < 600) {
        RobotInput in;
        active = t.tickRelative(dt, in);
        t.update(dt, in);
    }
    check(!active, "turn 完成后 tickRelative 返回 false");
    check(t.theta() <= glm::radians(-89.0f) && t.theta() >= glm::radians(-95.0f),
          "turn -90° 实转落在 [-95°, -89°]");

    // 无相对运动时 tickRelative 不接管
    Robot idle;
    RobotInput in;
    check(!idle.tickRelative(dt, in), "无相对运动返回 false");
}

// ---- Robot：射线测距 ----

std::vector<Obstacle> oneBox(float cx, float cz, float mass = 1.0f) {
    return {{{cx, 0.4f, cz}, {0.4f, 0.4f, 0.4f}, {1.0f, 1.0f, 1.0f}, mass}};
}

void testQueryRange() {
    Robot r;  // 原点 theta=0，射线起点 x=+0.2（半车长）
    // 盒中心 3.0 半宽 0.4 → 近面 2.6 → 命中距离 2.6-0.2 = 2.4
    check(near(r.queryRange(oneBox(3.0f, 0.0f)), 2.4f, 1e-3f), "正前方命中距离 = 2.4m");

    check(near(r.queryRange({}), Robot::kRangeMax, 1e-6f), "无障碍返回 maxRange");

    // 障碍在身后：射线反向无命中
    check(near(r.queryRange(oneBox(-3.0f, 0.0f)), Robot::kRangeMax, 1e-6f), "身后障碍不计入");

    // 转向后测侧方：theta=+90°（朝 -Z）时正前方变为 -Z 方向的盒
    Robot side;
    side.setRelativeTurn(glm::radians(90.0f));
    RobotInput in;
    int guard = 0;
    while (side.tickRelative(1.0f / 60.0f, in) && guard++ < 600) {
        side.update(1.0f / 60.0f, in);
    }
    check(near(side.queryRange(oneBox(side.x(), -3.0f)), 2.4f - 0.0f, 0.15f),
          "转 90° 后命中侧方障碍");
}

// ---- Robot：搬运 ----

void testPickPlace() {
    std::vector<Obstacle> obs{
        {{0.7f, 0.2f, 0.0f}, {0.2f, 0.2f, 0.2f}, {1.0f, 1.0f, 1.0f}, 5.0f},  // 前方近面 0.5
        {{2.0f, 0.2f, 3.0f}, {0.2f, 0.2f, 0.2f}, {1.0f, 1.0f, 1.0f}, 1.0f},  // 远处侧方
    };
    Robot r;
    check(!r.isCarrying(), "初始未搬运");

    // 前端在 0.2、近面在 0.5 → 距离 0.3 < 0.6，可拾取
    check(r.pick(obs), "前方 0.3m 的盒可拾取");
    check(r.isCarrying(), "拾取后 isCarrying");
    check(r.carriedObstacleIndex() == 0, "拾取的是最近的 idx=0");
    check(near(r.carriedMass(), 5.0f, 1e-6f), "carriedMass = 障碍质量");
    check(r.effectiveMaxSpeed() < Robot::kMaxSpeed, "搬运时最大速度被压低");

    check(!r.pick(obs), "已持有物体时再 pick 失败");
    check(near(r.queryRange(obs), Robot::kRangeMax, 1e-6f), "被搬运物体不参与测距");

    // 被搬运物体跟随机器人：updateCarriedPosition 把它钉在机体正上方
    for (int i = 0; i < 60; ++i) {
        r.update(1.0f / 60.0f, {0.5f, 0.0f});
    }
    r.updateCarriedPosition(obs);
    check(r.x() > 0.1f, "搬运中机器人仍可前进（减速但不清零）");
    check(near(obs[0].center.x, r.x(), 1e-6f), "搬运中物体 x 钉在机体正上方");
    check(obs[0].center.y > Robot::kBodyY, "搬运中物体抬到身体上方");

    // place：放到身体前端 + placeDist（0.2+0.5=0.7）
    const float px = r.x();
    check(r.place(obs), "place 成功");
    check(!r.isCarrying(), "place 后未搬运");
    check(near(obs[0].center.x, px + 0.2f + 0.5f, 1e-3f), "物体落在前方 placeDist 处");
    check(r.place(obs) == false, "未持有物体时 place 失败");

    // 拾不到的两种情形：太远 / 在身后
    Robot far;
    std::vector<Obstacle> farBox = oneBox(2.0f, 0.0f);
    std::vector<Obstacle> behindBox = oneBox(-1.0f, 0.0f);
    check(!far.pick(farBox), "前方 1.8m 的盒拾取失败（超出 0.6m）");
    check(!far.pick(behindBox), "身后的盒拾取失败");
}

// ---- CommandBridge：线上协议 ----

void testCommandBridge() {
    const std::string path = testSockPath("cmd");
    CommandBridge bridge(path);
    check(bridge.listening(), "命令桥监听成功");

    int fd = connectTo(path);
    check(fd >= 0, "客户端连上命令桥");

    say(fd, "fdist 2.50\n");
    bridge.poll();
    check(bridge.command() == SimCommand::FwdDist, "fdist 解析为 FwdDist");
    check(near(bridge.param(), 2.5f, 1e-4f), "fdist 参数 = 2.5");
    check(bridge.seq() == 1u, "第一条指令 seq=1");

    say(fd, "forward 0.5\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Forward && near(bridge.speed(), 0.5f, 1e-4f),
          "forward 0.5 解析成功");
    check(bridge.seq() == 2u, "seq 随接受行自增");

    // 垃圾行/坏参数/缺参数：整行忽略，状态与 seq 都不动
    say(fd, "xyzzy\nfdist abc\nturn\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Forward && bridge.seq() == 2u,
          "未知指令与坏参数整行忽略");

    // 同帧多条指令以最后一条为准，但每条都计入 seq
    say(fd, "turn -90\nleft 0.3\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Left && near(bridge.speed(), 0.3f, 1e-4f),
          "同帧多行以最后一条为准");
    check(bridge.seq() == 4u, "两条都被接受（seq=4）");

    // 发完即关写端（socat 手测模式）：行必须被处理，不能被 EOF 连带清掉
    say(fd, "fdist 3\n");
    ::shutdown(fd, SHUT_WR);
    bridge.poll();
    check(bridge.command() == SimCommand::FwdDist && near(bridge.param(), 3.0f, 1e-4f),
          "半关写端后请求行仍被处理（延迟断开）");
    bridge.poll();
    check(bridge.command() == SimCommand::Stop, "EOF 处理完后回落 Stop");

    // 新连接顶掉旧连接
    ::close(fd);
    const int fd2 = connectTo(path);
    check(fd2 >= 0, "重连成功");
    say(fd2, "stop\n");
    bridge.poll();
    check(bridge.command() == SimCommand::Stop && bridge.seq() == 6u,
          "新连接顶掉旧连接并接受指令");

    // 灌垃圾超限：断开并回落 Stop
    ::close(fd2);
    const int fd3 = connectTo(path);
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
    ::close(fd);
}

}  // namespace

int main() {
    testKinematics();
    testRelativeMotion();
    testQueryRange();
    testPickPlace();
    testCommandBridge();
    testSensorBridge();
    testCaptureBridge();

    std::printf("sim_tests: %d 通过, %d 失败\n", g_total - g_failed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
