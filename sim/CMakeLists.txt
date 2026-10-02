# mybot 3D 仿真器：独立 CMake 工程，刻意不被仓库根 CMakeLists.txt 引用。
#
# 原因：根工程的五条 CI job（release/sanitizer/tsan/clang/musl）都是
# `cmake --build` 全 target 构建，而 CI 基础镜像没有任何 GL/X11 库、musl
# 静态构建也编不过 GLFW。挂进根工程会直接把这五条打红。这里比照 agent/
# （Rust）的模式：自带工程 + scripts/build_sim.sh 独立构建。
#
# 构建：bash scripts/build_sim.sh，或
#   cmake -S sim -B sim/build && cmake --build sim/build
cmake_minimum_required(VERSION 3.20)
project(mybot-sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
)
FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
)
FetchContent_MakeAvailable(glfw glm)

add_library(glad STATIC external/glad/src/glad.c)
target_include_directories(glad PUBLIC external/glad/include)

add_executable(mybot-sim
    src/main.cpp
    src/shader.cpp
    src/camera.cpp
    src/mesh.cpp
    src/robot.cpp
    src/spider.cpp
    src/bridge.cpp
    src/capture.cpp
    src/sensor.cpp
    src/framegrab.cpp
)
# external/stb：public-domain stb_image_write.h（JPEG 编码），仅 framegrab.cpp
# 定义 STB_IMAGE_WRITE_IMPLEMENTATION 一处引入实现。
target_include_directories(mybot-sim PRIVATE src external/stb)
target_link_libraries(mybot-sim PRIVATE glfw glad glm::glm)
