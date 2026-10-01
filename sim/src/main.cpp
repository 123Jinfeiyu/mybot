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
#include "shader.h"
#include "spider.h"

namespace {

const char* kDefaultSocketPath = "/tmp/mybot-sim.sock";
const char* kDefaultCaptureSocketPath = "/tmp/mybot-sim-capture.sock";
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

    Shader phong(kPhongVs, kPhongFs);
    Shader lines(kLineVs, kLineFs);
    SceneMeshes meshes = buildSceneMeshes();
    Robot robot;
    Spider spider;

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

        if (g_resetRequested) {
            robot.reset();
            spider.reset();
            g_resetRequested = false;
        }
        if (g_toggleCamera) {
            g_camera.follow = !g_camera.follow;
            g_toggleCamera = false;
        }

        const RobotInput input = commandToInput(bridge.command(), bridge.speed());
        robot.update(dt, input);
        spider.update(dt, robot.v());

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

        drawSpider(robot, spider, phong, meshes);

        lines.use();
        lines.setMat4("mvp", projection * view);
        lines.setVec3("color", glm::vec3(0.18f, 0.22f, 0.18f));
        meshes.grid.draw();

        char cmdBuf[48];
        if (bridge.command() == SimCommand::Stop) {
            std::snprintf(cmdBuf, sizeof(cmdBuf), "stop");
        } else {
            std::snprintf(cmdBuf, sizeof(cmdBuf), "%s %.0f%%", commandName(bridge.command()),
                          bridge.speed() * 100.0f);
        }

        char title[320];
        std::snprintf(title, sizeof(title),
                      "MyBot Spider | cmd %-12s v %+.2f m/s  w %+.2f rad/s | pose (%.1f, %.1f) th %.0f deg | %s %s | cam %s | C camera[%s]  R reset",
                      cmdBuf,
                      robot.v(), robot.omega(),
                      robot.x(), robot.z(), glm::degrees(robot.theta()),
                      bridge.listening() ? "listening" : "NOT LISTENING",
                      bridge.path().c_str(),
                      capture.listening() ? "on" : "OFF",
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
