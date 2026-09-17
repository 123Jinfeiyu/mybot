cmake_minimum_required(VERSION 3.16)
project(mybot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# LuaJIT：优先 pkg-config（linux 发行版已装 libluajit-5.1-dev 即有）
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LUAJIT QUIET luajit)
endif()

if(LUAJIT_FOUND)
  message(STATUS "Found LuaJIT via pkg-config: ${LUAJIT_INCLUDE_DIRS}")
else()
  find_path(LUAJIT_INCLUDE_DIR
    NAMES lua.h lauxlib.h
    PATH_SUFFIXES luajit-2.1)
  find_library(LUAJIT_LIBRARY NAMES luajit-5.1 luajit)
  if(NOT LUAJIT_INCLUDE_DIR OR NOT LUAJIT_LIBRARY)
    message(FATAL_ERROR "LuaJIT not found. Install libluajit-5.1-dev (e.g. sudo apt install libluajit-5.1-dev)")
  endif()
  set(LUAJIT_INCLUDE_DIRS ${LUAJIT_INCLUDE_DIR})
  set(LUAJIT_LIBRARIES ${LUAJIT_LIBRARY})
endif()

# Sanitizer 开关（F-23）：随仓库分发、两个目标一致套用，避免各人手工拼
# CXX_FLAGS 造成「测试构建干净、主构建不干净」的错位。用法：
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
#     -DMYBOT_SANITIZE=address,undefined
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMYBOT_SANITIZE=thread
set(MYBOT_SANITIZE "" CACHE STRING
  "Sanitizer list applied to all targets (e.g. address,undefined or thread)")
if(MYBOT_SANITIZE)
  add_compile_options(-fsanitize=${MYBOT_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${MYBOT_SANITIZE})
endif()

# 被测主体（不含入口 main.cpp）：mybot 与 mybot_tests 共用同一份，
# 保证单测编译的正是发布二进制里的那套实现，而不是第二份拷贝。
set(MYBOT_CORE_SOURCES
  src/json.cpp
  src/matrix.cpp
  src/sqlparse.cpp
  src/luaend.cpp
  src/hardware.cpp
  src/luavar.cpp
  src/daemonvar.cpp
  src/robot.cpp
  src/actionsource.cpp
)

add_executable(mybot
  src/main.cpp
  ${MYBOT_CORE_SOURCES}
)

target_include_directories(mybot PRIVATE src ${LUAJIT_INCLUDE_DIRS})

# README 声称的零告警基线由此处强制执行（此前构建未启用任何告警标志，
# 基线只能靠人工手工传标志核查）。不加 -Werror：避免在未测试的编译器版本上直接构建失败。
target_compile_options(mybot PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(mybot PRIVATE
  ${LUAJIT_LIBRARIES}
  pthread
  dl
  m
)

# ======================================================================
# C++ 层单元测试（test/cpp/）：零第三方框架，harness.hpp 自带断言与注册。
# 与被测程序共用同一套源文件（除 main.cpp），因此不需要 mybot 变成库目标；
# 告警标志与链接库也保持完全一致，避免「测试构建干净、主构建不干净」的错位。
# ======================================================================
add_executable(mybot_tests
  test/cpp/main.cpp
  test/cpp/test_json.cpp
  test/cpp/test_matrix.cpp
  test/cpp/test_sqlparse.cpp
  test/cpp/test_hardware.cpp
  test/cpp/test_robot.cpp
  test/cpp/test_luaend.cpp
  test/cpp/test_luavar.cpp
  test/cpp/test_actionsource.cpp
  ${MYBOT_CORE_SOURCES}
)

target_include_directories(mybot_tests PRIVATE src test/cpp ${LUAJIT_INCLUDE_DIRS})
target_compile_options(mybot_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mybot_tests PRIVATE
  ${LUAJIT_LIBRARIES}
  pthread
  dl
  m
)

enable_testing()
add_test(NAME cpp_unit COMMAND mybot_tests)

# 安装产物：mybot 直跑 ./build/mybot（bot4cj 原 _run.sh 的等价物，见 test/run_all.sh）
install(TARGETS mybot RUNTIME DESTINATION bin)