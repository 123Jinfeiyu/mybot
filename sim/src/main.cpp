#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bridge.h"
#include "camera.h"
#include "capture.h"
#include "framegrab.h"
#include "mesh.h"
#include "robot.h"
#include "sensor.h"
#include "shader.h"
#include "shaders.h"
#include "sim_config.h"
#include "sim_engine.h"
#include "spider.h"
#include "world.h"

namespace {

const char* kDefaultSocketPath = "/tmp/mybot-sim.sock";
const char* kDefaultCaptureSocketPath = "/tmp/mybot-sim-capture.sock";
const char* kDefaultSensorSocketPath = "/tmp/mybot-sim-sensor.sock";

// 三条 socket 同一规矩：argv > 环境变量 > 缺省。路径的单一事实源是环境
// 变量名，mybot 侧 Sim.lua / Camera.lua 与这里同读。
std::string resolveSocketPath(int argc, char** argv, int argvIndex, const char* envName,
                              const char* fallback) {
    if (argc > argvIndex) {
        return argv[argvIndex];
    }
    if (const char* fromEnv = std::getenv(envName)) {
        if (fromEnv[0] != '\0') {
            return fromEnv;
        }
    }
    return fallback;
}

Camera g_camera;
bool g_resetRequested = false;
bool g_toggleCamera = false;
bool g_saveRequested = false;
bool g_dragging = false;
double g_lastX = 0.0, g_lastY = 0.0;

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
    if (key == GLFW_KEY_S && action == GLFW_PRESS)
        g_saveRequested = true;
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

// 窗口标题即仪表盘：动作/速度/位姿/测距/搬运/三桥状态一页看全
void updateTitle(GLFWwindow* window, const CommandBridge& bridge, const CaptureBridge& capture,
                 const SensorBridge& sensor, const Robot& robot, const World& world,
                 const char* sceneName) {
    char cmdBuf[48];
    if (bridge.command() == SimCommand::Stop) {
        std::snprintf(cmdBuf, sizeof(cmdBuf), "stop");
    } else {
        std::snprintf(cmdBuf, sizeof(cmdBuf), "%s %.0f%%", commandName(bridge.command()),
                      bridge.speed() * 100.0f);
    }

    char lastBuf[48];
    std::snprintf(lastBuf, sizeof(lastBuf), "%s", bridge.lastLine().c_str());

    const float rangeM = world.queryRange(robot);

    char carryBuf[32];
    if (world.isCarrying()) {
        std::snprintf(carryBuf, sizeof(carryBuf), "carrying %.1fkg", world.carriedMass());
    } else {
        std::snprintf(carryBuf, sizeof(carryBuf), "empty");
    }

    char title[512];
    std::snprintf(title, sizeof(title),
                  "MyBot Spider | cmd %-16s last %-14s v %+.2f m/s  w %+.2f rad/s | pose (%.1f, %.1f) th %.0f deg | range %.2fm | %s | scene %s | %s %s | cam %s sensor %s | C camera[%s]  R reset+reload  S save",
                  cmdBuf,
                  lastBuf,
                  robot.v(), robot.omega(),
                  robot.x(), robot.z(), glm::degrees(robot.theta()),
                  rangeM,
                  carryBuf,
                  sceneName,
                  bridge.listening() ? "listening" : "NOT LISTENING",
                  bridge.path().c_str(),
                  capture.listening() ? "on" : "OFF",
                  sensor.listening() ? "on" : "OFF",
                  g_camera.follow ? "follow" : "free");
    glfwSetWindowTitle(window, title);
}

}  // namespace

int main(int argc, char** argv) {
    // 三条 socket：命令（持续型即发即忘）、拍照（一问一答带二进制体）、测距
    // （同步查询）。mybot 侧 Sim.lua 与 Camera.lua 是独立驱动实例，无法共用连接。
    const std::string socketPath =
        resolveSocketPath(argc, argv, 1, "MYBOT_SIM_SOCK", kDefaultSocketPath);
    const std::string capturePath =
        resolveSocketPath(argc, argv, 2, "MYBOT_SIM_CAPTURE_SOCK", kDefaultCaptureSocketPath);
    const std::string sensorPath =
        resolveSocketPath(argc, argv, 3, "MYBOT_SIM_SENSOR_SOCK", kDefaultSensorSocketPath);

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
    Spider spider;

    // 场景：argv[4] > MYBOT_SIM_SCENE > 内置默认（空串=内置）。环境与代码解耦
    // 的落点：障碍布局来自文件。save 只写这个启动时定下的路径——socket 线上
    // 不收路径参数，杜绝「连任意文件写」原语（与 Camera.lua 只认 env 的口径同源）。
    const std::string scenePath = resolveSocketPath(argc, argv, 4, "MYBOT_SIM_SCENE", "");

    // 仿真逻辑（场景会话 + 每帧推进 + 指令派发）全在 GL 无关的 SimEngine 里，
    // 可 headless 单测；壳只负责窗口/输入/渲染/HUD，把语义动作喂引擎再 step()。
    SimEngine engine(scenePath);
    engine.reloadScene();

    const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.3f));
    const glm::vec3 lightColor(1.0f, 0.98f, 0.92f);

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
            // 复位 = 场景连同机器人回到装载时状态（文件版重读文件、内置版回默认）
            engine.reloadScene();
            spider.reset();
            g_resetRequested = false;
        }
        if (g_saveRequested) {
            engine.saveScene();
            g_saveRequested = false;
        }
        if (g_toggleCamera) {
            g_camera.follow = !g_camera.follow;
            g_toggleCamera = false;
        }

        // 语义动作喂引擎：持续型同步状态（新非-stop 指令打断相对运动在引擎内处理），
        // 一次性事件 drain 到空——每条必触发一次（连续两次 fdist 不再互相吞）。
        engine.applyContinuous(bridge.command(), bridge.speed());
        SimEvent ev;
        while (bridge.pollEvent(ev)) {
            engine.applyEvent(ev);
        }

        engine.step(dt);
        spider.update(dt, engine.robot().v());

        // 测距响应：碰撞解算完成后算距离，保证拿到的是本帧最终位姿
        if (wantRange) {
            const float dist = engine.queryRange();
            const int distMm = static_cast<int>(dist * 1000.0f + 0.5f);
            if (!sensor.sendRange(distMm)) {
                sensor.sendError("range send failed");
            }
        }

        if (g_camera.follow) {
            glm::vec3 robotPos(engine.robot().x(), 0.2f, engine.robot().z());
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
        for (const Obstacle& ob : engine.world().obstacles()) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), ob.center) *
                          glm::scale(glm::mat4(1.0f), ob.half * 2.0f);
            phong.setMat4("model", m);
            phong.setVec3("objectColor", ob.color);
            meshes.cube.draw();
        }

        drawSpider(engine.robot(), spider, phong, meshes);

        lines.use();
        lines.setMat4("mvp", projection * view);
        lines.setVec3("color", glm::vec3(0.18f, 0.22f, 0.18f));
        meshes.grid.draw();

        updateTitle(window, bridge, capture, sensor, engine.robot(), engine.world(),
                    engine.sceneName().c_str());

        // 抓帧回传放在 SwapBuffers 之前：此刻 GL_BACK 是刚画好的这一帧。
        if (wantCapture) {
            int capW = 0, capH = 0;
            std::vector<unsigned char> jpeg = grabFrameJpeg(SimConfig::kJpegQuality, capW, capH);
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
