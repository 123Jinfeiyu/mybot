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

add_executable(mybot
  src/main.cpp
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

# 安装产物：mybot 直跑 ./build/mybot（bot4cj 原 _run.sh 的等价物，见 test/run_all.sh）
install(TARGETS mybot RUNTIME DESTINATION bin)