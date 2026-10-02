#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bridge.h"
#include "camera.h"
#include "capture.h"
#include "framegrab.h"
#include "mesh.h"
#include "robot.h"
#include "sensor.h"
#include "shader.h"
#include "spider.h"

namespace {

const char* kDefaultSocketPath = "/tmp/mybot-sim.sock";
const char* kDefaultCaptureSocketPath = "/tmp/mybot-sim-capture.sock";
const char* kDefaultSensorSocketPath = "/tmp/mybot-sim-sensor.sock";
constexpr int kJpegQuality = 90;

const char* kPhongVs = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
out vec3 fragPos;
out vec3 normal;
void main() {
    fragPos = vec3(model * vec4(aPos, 1.0));
    normal = mat3(transpose(inverse(model))) * aNormal;
    gl_Position = projection * view * vec4(fragPos, 1.0);
}
)";

const char* kPhongFs = R"(
#version 330 core
in vec3 fragPos;
in vec3 normal;
uniform vec3 objectColor;
uniform vec3 lightColor;
uniform vec3 lightDir;
uniform vec3 viewPos;
out vec4 fragColor;
void main() {
    vec3 n = normalize(normal);
    vec3 l = normalize(-lightDir);
    float diff = max(dot(n, l), 0.0);
    vec3 viewDir = normalize(viewPos - fragPos);
    vec3 h = normalize(viewDir + l);
    float spec = pow(max(dot(n, h), 0.0), 48.0) * 0.35;
    vec3 color = (0.28 + diff) * objectColor * lightColor + spec * lightColor;
    fragColor = vec4(color, 1.0);
}
)";

const char* kLineVs = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 mvp;
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
}
)";

const char* kLineFs = R"(
#version 330 core
uniform vec3 color;
out vec4 fragColor;
void main() {
    fragColor = vec4(color, 1.0);
}
)";

Camera g_camera;
bool g_resetRequested = false;
bool g_toggleCamera = false;
bool g_dragging = false;
double g_lastX = 0.0, g_lastY = 0.0;

// 场景障碍物：蜘蛛从原点出发沿 +X 前进，障碍散布在前进路线两侧。
// Obstacle 定义在 robot.h（queryRange 需要访问）。
// 非 const 是因为搬运设备会修改被搬运物体的位置。
constexpr float kRobotRadius = 0.20f;  // 蜘蛛碰撞球半径（XZ 平面，约半车身长）
constexpr float kDefaultDensity = 800.0f;  // 默认密度 kg/m³（近似木材）
std::vector<Obstacle> g_obstacles = {
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

// 初始化障碍物质量：体积 × 密度。在 main() 开头调用一次。
void initObstacleMass() {
    for (auto& ob : g_obstacles) {
        const float volume = 8.0f * ob.half.x * ob.half.y * ob.half.z;  // 2*half 是全尺寸
        ob.mass = volume * kDefaultDensity;
    }
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

void framebufferSizeCallback(GLFWwindow*, int width, int height) {
    glViewport(0, 0, width, height);
}

void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    if (key == GLFW_KEY_R && action == GLFW_PRESS)
        g_resetRequested = true;
    if (key == GLFW_KEY_C && action == GLFW_PRESS)
        g_toggleCamera = true;
}

void mouseButtonCallback(GLFWwindow*, int button, int action, int) {
    if (button == GLFW_MOUSE_BUTTON_LEFT)
        g_dragging = (action == GLFW_PRESS);
}

void cursorPosCallback(GLFWwindow*, double x, double y) {
    if (g_dragging)
        g_camera.onMouseDrag((float)(x - g_lastX), (float)(y - g_lastY));
    g_lastX = x;
    g_lastY = y;
}

void scrollCallback(GLFWwindow*, double, double yOffset) {
    g_camera.onScroll((float)yOffset);
}

RobotInput commandToInput(SimCommand cmd, float speed) {
    RobotInput input;
    switch (cmd) {
        case SimCommand::Forward:
            input.throttle = speed;
            break;
        case SimCommand::Backward:
            input.throttle = -speed;
            break;
        case SimCommand::Left:
            input.steer = speed;
            break;
        case SimCommand::Right:
            input.steer = -speed;
            break;
        case SimCommand::Stop:
            break;
    }
    return input;
}

}  // namespace

int main(int argc, char** argv) {
    const char* socketPath = kDefaultSocketPath;
    if (argc > 1) {
        socketPath = argv[1];
    } else if (const char* fromEnv = std::getenv("MYBOT_SIM_SOCK")) {
        if (fromEnv[0] != '\0') {
            socketPath = fromEnv;
        }
    }

    // 拍照 socket 与命令 socket 分开两条：命令是持续型即发即忘、拍照是一问一答
    // 带二进制体，且 mybot 侧 Sim.lua 与 Camera.lua 是各自独立的驱动实例，无法
    // 共用一条连接。路径来源同命令 socket 的规矩：argv[2] > 环境变量 > 缺省。
    const char* capturePath = kDefaultCaptureSocketPath;
    if (argc > 2) {
        capturePath = argv[2];
    } else if (const char* fromEnv = std::getenv("MYBOT_SIM_CAPTURE_SOCK")) {
        if (fromEnv[0] != '\0') {
            capturePath = fromEnv;
        }
    }

    // 测距 sensor socket：argv[3] > 环境变量 > 缺省。
    const char* sensorPath = kDefaultSensorSocketPath;
    if (argc > 3) {
        sensorPath = argv[3];
    } else if (const char* fromEnv = std::getenv("MYBOT_SIM_SENSOR_SOCK")) {
        if (fromEnv[0] != '\0') {
            sensorPath = fromEnv;
        }
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "failed to initialize GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "MyBot Spider Simulator", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::fprintf(stderr, "failed to load OpenGL\n");
        glfwTerminate();
        return 1;
    }

    glEnable(GL_DEPTH_TEST);

    // 监听失败不退出：窗口照常开着，标题上标出 NOT LISTENING 让人去查路径，
    // 比直接崩掉好定位（多半是路径太长或目录不可写）
    CommandBridge bridge(socketPath);
    if (bridge.listening()) {
        std::fprintf(stderr, "listening for mybot commands on %s\n", bridge.path().c_str());
        std::fprintf(stderr, "  try: socat - UNIX-CONNECT:%s   then type: forward\n",
                     bridge.path().c_str());
    }

    CaptureBridge capture(capturePath);
    if (capture.listening()) {
        std::fprintf(stderr, "listening for capture requests on %s\n", capture.path().c_str());
        std::fprintf(stderr, "  try: printf 'capture\\n' | socat - UNIX-CONNECT:%s | head -c 64\n",
                     capture.path().c_str());
    }

    SensorBridge sensor(sensorPath);
    if (sensor.listening()) {
        std::fprintf(stderr, "listening for sensor requests on %s\n", sensor.path().c_str());
        std::fprintf(stderr, "  try: printf 'range\\n' | socat - UNIX-CONNECT:%s\n",
                     sensor.path().c_str());
    }

    Shader phong(kPhongVs, kPhongFs);
    Shader lines(kLineVs, kLineFs);
    SceneMeshes meshes = buildSceneMeshes();
    Robot robot;
    Spider spider;

    initObstacleMass();  // 按体积×密度填充质量

    const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.3f));
    const glm::vec3 lightColor(1.0f, 0.98f, 0.92f);

    // 跟踪上一条命令，只在命令变化时设置相对运动参数，避免每帧重置
    SimCommand lastCommand = SimCommand::Stop;

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = std::min(0.05f, (float)(now - last));
        last = now;

        bridge.poll();
        // 记下本帧是否有拍照请求；真正的抓帧要等这一帧画完、SwapBuffers 之前
        // 再做（那时 GL_BACK 才是刚渲染好的画面），故此处只取标志不抓帧。
        const bool wantCapture = capture.poll();
        const bool wantRange = sensor.poll();

        if (g_resetRequested) {
            robot.reset();
            spider.reset();
            g_resetRequested = false;
        }
        if (g_toggleCamera) {
            g_camera.follow = !g_camera.follow;
            g_toggleCamera = false;
        }

        // 相对运动拦截：fdist/turn 到达时设到 robot 的剩余量里，后续帧由
        // tickRelative() 接管 input 直到量耗尽。新的持续型指令（forward 等）
        // 或 stop 会清零剩余量，交回手动控制。
        // 只在命令变化时设置相对运动参数，避免每帧重置
        const SimCommand cmd = bridge.command();
        if (cmd != lastCommand) {
            std::fprintf(stderr, "cmd: %d -> %d\n", static_cast<int>(lastCommand), static_cast<int>(cmd));
            if (cmd == SimCommand::FwdDist) {
                robot.setRelativeDist(bridge.param());
            } else if (cmd == SimCommand::Turn) {
                robot.setRelativeTurn(glm::radians(bridge.param()));
            } else if (cmd == SimCommand::Pick) {
                if (robot.isCarrying()) {
                    std::fprintf(stderr, "pick: 已持有物体 (idx=%d, %.2fkg)，忽略\n",
                                 robot.carriedObstacleIndex(), robot.carriedMass());
                } else if (robot.pick(g_obstacles)) {
                    const auto& ob = g_obstacles[robot.carriedObstacleIndex()];
                    std::fprintf(stderr, "pick: 成功拾取 idx=%d mass=%.2fkg pos=(%.2f,%.2f)\n",
                                 robot.carriedObstacleIndex(), robot.carriedMass(),
                                 ob.center.x, ob.center.z);
                } else {
                    std::fprintf(stderr, "pick: 前方 %.2fm 内无障碍物，拾取失败\n", 0.6f);
                }
            } else if (cmd == SimCommand::Place) {
                if (robot.place(g_obstacles)) {
                    std::fprintf(stderr, "place: 放置成功\n");
                } else {
                    std::fprintf(stderr, "place: 未持有物体，放置失败\n");
                }
            } else if (cmd != SimCommand::Stop) {
                // 持续型指令覆盖相对运动
                robot.setRelativeDist(0.0f);
                robot.setRelativeTurn(0.0f);
            }
            lastCommand = cmd;
        }

        static int frameCount = 0;
        ++frameCount;
        
        RobotInput input = commandToInput(cmd, bridge.speed());
        RobotInput relInput;
        if (robot.tickRelative(dt, relInput)) {
            input = relInput;
        }
        if (robot.isCarrying() && frameCount % 60 == 0) {
            std::fprintf(stderr, "input: throttle=%.2f steer=%.2f cmd=%d\n",
                         input.throttle, input.steer, static_cast<int>(cmd));
        }
        robot.update(dt, input);
        // 碰撞解算：逐个障碍检查，撞了就推出（多障碍时逐次解算，不迭代）
        // 跳过被搬运的物体——它附着在机器人上，不应该参与碰撞
        const int carriedIdx = robot.carriedObstacleIndex();
        for (int i = 0; i < (int)g_obstacles.size(); ++i) {
            if (i == carriedIdx) continue;
            const Obstacle& ob = g_obstacles[i];
            float rx = robot.x(), rz = robot.z();
            if (resolveSphereAABB2D(rx, rz, kRobotRadius, ob)) {
                robot.setX(rx);
                robot.setZ(rz);
            }
        }
        // 搬运中的物体跟随机器人移动：放在身体前端上方
        robot.updateCarriedPosition(g_obstacles);
        if (robot.isCarrying()) {
            if (frameCount % 60 == 0) {  // 每秒输出一次
                const auto& ob = g_obstacles[robot.carriedObstacleIndex()];
                std::fprintf(stderr, "carry: robot=(%.2f,%.2f) v=%.3f carried=(%.2f,%.2f) theta=%.2f\n",
                             robot.x(), robot.z(), robot.v(), ob.center.x, ob.center.z, robot.theta());
            }
        }
        spider.update(dt, robot.v());

        // 测距响应：碰撞解算完成后算距离，保证拿到的是本帧最终位姿
        if (wantRange) {
            const float dist = robot.queryRange(g_obstacles);
            const int distMm = static_cast<int>(dist * 1000.0f + 0.5f);
            if (!sensor.sendRange(distMm)) {
                sensor.sendError("frame grab or send failed");
            }
        }

        if (g_camera.follow) {
            glm::vec3 robotPos(robot.x(), 0.2f, robot.z());
            float k = std::min(1.0f, 5.0f * dt);
            g_camera.target += (robotPos - g_camera.target) * k;
        }

        int width = 1, height = 1;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.62f, 0.73f, 0.86f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f), (float)width / (float)height, 0.1f, 300.0f);
        glm::mat4 view = g_camera.view();

        phong.use();
        phong.setMat4("view", view);
        phong.setMat4("projection", projection);
        phong.setVec3("lightDir", lightDir);
        phong.setVec3("lightColor", lightColor);
        phong.setVec3("viewPos", g_camera.position());

        glm::mat4 ground = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.05f, 0.0f)) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(62.0f, 0.1f, 62.0f));
        phong.setMat4("model", ground);
        phong.setVec3("objectColor", glm::vec3(0.38f, 0.46f, 0.36f));
        meshes.cube.draw();

        // 障碍盒：unit cube 经 translate(center)*scale(2*half) 摆到位
        for (const Obstacle& ob : g_obstacles) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), ob.center) *
                          glm::scale(glm::mat4(1.0f), ob.half * 2.0f);
            phong.setMat4("model", m);
            phong.setVec3("objectColor", ob.color);
            meshes.cube.draw();
        }

        drawSpider(robot, spider, phong, meshes);

        lines.use();
        lines.setMat4("mvp", projection * view);
        lines.setVec3("color", glm::vec3(0.18f, 0.22f, 0.18f));
        meshes.grid.draw();

        char cmdBuf[48];
        if (bridge.command() == SimCommand::Stop) {
            std::snprintf(cmdBuf, sizeof(cmdBuf), "stop");
        } else if (bridge.command() == SimCommand::FwdDist) {
            std::snprintf(cmdBuf, sizeof(cmdBuf), "fdist %.2fm", bridge.param());
        } else if (bridge.command() == SimCommand::Turn) {
            std::snprintf(cmdBuf, sizeof(cmdBuf), "turn %.0f deg", bridge.param());
        } else {
            std::snprintf(cmdBuf, sizeof(cmdBuf), "%s %.0f%%", commandName(bridge.command()),
                          bridge.speed() * 100.0f);
        }

        const float rangeM = robot.queryRange(g_obstacles);

        char carryBuf[32];
        if (robot.isCarrying()) {
            std::snprintf(carryBuf, sizeof(carryBuf), "carrying %.1fkg", robot.carriedMass());
        } else {
            std::snprintf(carryBuf, sizeof(carryBuf), "empty");
        }

        char title[420];
        std::snprintf(title, sizeof(title),
                      "MyBot Spider | cmd %-16s v %+.2f m/s  w %+.2f rad/s | pose (%.1f, %.1f) th %.0f deg | range %.2fm | %s | %s %s | cam %s sensor %s | C camera[%s]  R reset",
                      cmdBuf,
                      robot.v(), robot.omega(),
                      robot.x(), robot.z(), glm::degrees(robot.theta()),
                      rangeM,
                      carryBuf,
                      bridge.listening() ? "listening" : "NOT LISTENING",
                      bridge.path().c_str(),
                      capture.listening() ? "on" : "OFF",
                      sensor.listening() ? "on" : "OFF",
                      g_camera.follow ? "follow" : "free");
        glfwSetWindowTitle(window, title);

        // 抓帧回传放在 SwapBuffers 之前：此刻 GL_BACK 是刚画好的这一帧。
        if (wantCapture) {
            int capW = 0, capH = 0;
            std::vector<unsigned char> jpeg = grabFrameJpeg(kJpegQuality, capW, capH);
            if (jpeg.empty()) {
                capture.sendError("frame grab or jpeg encode failed");
            } else {
                capture.sendFrame(jpeg.data(), jpeg.size(), capW, capH);
            }
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}
