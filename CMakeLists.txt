cmake_minimum_required(VERSION 3.16)
project(mybot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
  src/expr.cpp
  src/sqlparse.cpp
  src/luaend.cpp
  src/hardware.cpp
  src/luavar.cpp
  src/daemonvar.cpp
  src/robot.cpp
  src/actionsource.cpp
)

# 两个目标的公共配置：零告警基线 + LuaJIT 头/库 + 平台库，一处声明一致套用，
# 避免「测试构建干净、主构建不干净」的错位（原先两份拷贝靠人工同步）。
add_library(mybot_common INTERFACE)
target_compile_options(mybot_common INTERFACE -Wall -Wextra -Wpedantic)
target_include_directories(mybot_common INTERFACE src ${LUAJIT_INCLUDE_DIRS})
target_link_libraries(mybot_common INTERFACE
  ${LUAJIT_LIBRARIES}
  pthread
  dl
  m
)

add_executable(mybot
  src/main.cpp
  ${MYBOT_CORE_SOURCES}
)

# mtmerge：.mt 动作脚本合成器（离线编译目标，不依赖 LuaJIT 运行时；
# 复用 mybot_common 的头文件与告警基线，core 源码里只需 matrix/sqlparse/util
# 一层，但直接链整份 core 以与 mybot 单测同一策略，避免第二份拷贝）
add_executable(mtmerge
  src/mtmerge.cpp
  ${MYBOT_CORE_SOURCES}
)
target_link_libraries(mtmerge PRIVATE mybot_common)

# README 声称的零告警基线由此处强制执行（此前构建未启用任何告警标志，
# 基线只能靠人工手工传标志核查）。不加 -Werror：避免在未测试的编译器版本上直接构建失败。
target_link_libraries(mybot PRIVATE mybot_common)

# ======================================================================
# C++ 层单元测试（test/cpp/）：零第三方框架，harness.hpp 自带断言与注册。
# 与被测程序共用同一套源文件（除 main.cpp），因此不需要 mybot 变成库目标。
# ======================================================================
add_executable(mybot_tests
  test/cpp/main.cpp
  test/cpp/test_json.cpp
  test/cpp/test_matrix.cpp
  test/cpp/test_expr.cpp
  test/cpp/test_sqlparse.cpp
  test/cpp/test_hardware.cpp
  test/cpp/test_robot.cpp
  test/cpp/test_luaend.cpp
  test/cpp/test_luavar.cpp
  test/cpp/test_actionsource.cpp
  test/cpp/test_daemon.cpp
  ${MYBOT_CORE_SOURCES}
)

target_include_directories(mybot_tests PRIVATE test/cpp)
target_link_libraries(mybot_tests PRIVATE mybot_common)

enable_testing()
add_test(NAME cpp_unit COMMAND mybot_tests)
# 用例以相对路径（./modules/...、robots/test/...）读 fixtures，必须以仓库根为工作目录
set_tests_properties(cpp_unit PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ======================================================================
# 常量一致性自检（F-24）：HAL.lua 硬编码的 ioctl/termios 编号与系统头文件
# 编译期比对（#error/_Static_assert）——换内核/换架构导致常量漂移时构建即失败；
# 两个二进制再由 run_all.sh 在执行期跑一遍（构建期断言之外的双保险）。
# ======================================================================
add_executable(test_ioctl_nums test/test_ioctl_nums.c)
add_executable(test_uart_consts test/test_uart_consts.c)
add_test(NAME const_ioctl COMMAND test_ioctl_nums)
add_test(NAME const_uart COMMAND test_uart_consts)

# ======================================================================
# Lua 离线回归逐用例注册（run_all.sh 保持主编排入口不变）：
# 每个 test_*.lua 都是 `luajit test/xxx.lua` 可单跑的独立用例（HAL 依赖 ffi
# 必须 LuaJIT）；唯一例外 test_jit_warmup.lua 带 "-- requires: i2c" 真机标记，
# 与 run_all.sh 同语义按有无 /dev/i2c-* 决定执行或跳过。
# 收益：ctest -R lua_ 获得逐用例粒度与超时，不必整段跑完 1013 行守门脚本。
# ======================================================================
file(GLOB MYBOT_LUA_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/test/test_*.lua)
find_program(LUAJIT_EXECUTABLE NAMES luajit)
if(LUAJIT_EXECUTABLE)
	foreach(lua_test IN LISTS MYBOT_LUA_TESTS)
		get_filename_component(lua_name "${lua_test}" NAME)
		if(lua_name STREQUAL "test_jit_warmup.lua")
			# 真机 ABI 回归：无 /dev/i2c-* 时按 skip 计，有真机则照跑
			add_test(NAME lua_${lua_name}
				COMMAND ${CMAKE_SOURCE_DIR}/test/cpp/run_if_i2c.sh ${LUAJIT_EXECUTABLE} "${lua_test}")
		else()
			add_test(NAME lua_${lua_name} COMMAND ${LUAJIT_EXECUTABLE} "${lua_test}")
		endif()
		set_tests_properties(lua_${lua_name} PROPERTIES
			WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
			TIMEOUT 60
			ENVIRONMENT "LUACMD=${LUAJIT_EXECUTABLE}")
		# 真机用例包装（run_if_i2c.sh）在无 /dev/i2c-* 时以 125 退出表示"不适用"，
		# 须显式声明为 skip 返回码，否则 ctest 会把跳过记成 Failed
		set_tests_properties(lua_${lua_name} PROPERTIES SKIP_RETURN_CODE 125)
	endforeach()
endif()

# 安装产物：mybot 直跑 ./build/mybot（bot4cj 原 _run.sh 的等价物，见 test/run_all.sh）
install(TARGETS mybot RUNTIME DESTINATION bin)