cmake_minimum_required(VERSION 3.16)
project(mybot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 静态分析（fix-plan 2026-09-20 阶段 D-3）：无条件导出 compile_commands.json，
# 供 clang-tidy 本地/CI 复用（对构建无副作用）。跑法见 docs/development.md 代码规范节。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# ======================================================================
# agent 模式：--agent 让大模型工具循环成为主执行流程，机器人结构脚本加载后
# 硬件常驻内存。
#
# 模型调用**不引入任何链接期依赖**：它是一颗 Lua 协议驱动
# （modules/LlmOpenAI.lua、modules/LlmDeepSeek.lua），经 LuaJIT ffi 在运行期
# 加载 libcurl。因此本工程的「零第三方 C++ 依赖（仅 LuaJIT + pthread + dl）」
# 在这条路径上依然成立；缺 libcurl 时是运行期在驱动 build() 阶段给出人话报错，
# 而不是构建失败。
# ======================================================================

# 被测主体（不含入口 main.cpp）：mybot 与 mybot_tests 共用同一份，
# 保证单测编译的正是发布二进制里的那套实现，而不是第二份拷贝。
set(MYBOT_CORE_SOURCES
  src/json.cpp
  src/jsonparse.cpp
  src/llmconfig.cpp
  src/matrix.cpp
  src/expr.cpp
  src/sqlparse.cpp
  src/mtmerge_core.cpp
  src/mtinclude.cpp
  src/luabridge.cpp
  src/luasandbox.cpp
  src/hardware.cpp
  src/luavar.cpp
  src/daemonvar.cpp
  src/hw_registry.cpp
  src/meta_provider.cpp
  src/hw_factory.cpp
  src/action_runner.cpp
  src/robot.cpp
  src/actionsource.cpp
  src/cliargs.cpp
  src/agent_subprocess.cpp
  src/agentproto.cpp
  src/agenttools.cpp
  src/agentloop.cpp
  src/agentmode.cpp
  src/agentdriver.cpp
  src/promptpack.cpp
)

# 两个目标的公共配置：零告警基线 + 头文件路径，一处声明一致套用，
# 避免「测试构建干净、主构建不干净」的错位（原先两份拷贝靠人工同步）。
#
# MYBOT_WERROR（2026-09-20 重构阶段 5.3）：把「零告警」从约定升级为门禁。
# 为什么不无条件加 -Werror：本项目要在**未预跑的编译器版本**上也能构建成功
#（CI 基线 debian:bookworm 的 g++12、树莓派发行版编译器都可能报出别家前端
# 才有的告警），无条件 -Werror 会把「换台机器就构建不过」引进来。故做成开关：
# 本地缺省 OFF（保持原取向），CI 的 build:release 显式 -DMYBOT_WERROR=ON——
# 门禁落在「有基线、可复现」的那台机器上。
option(MYBOT_WERROR "Treat -Wall/-Wextra/-Wpedantic warnings as errors" OFF)

add_library(mybot_warnings INTERFACE)
target_compile_options(mybot_warnings INTERFACE -Wall -Wextra -Wpedantic)
if(MYBOT_WERROR)
  target_compile_options(mybot_warnings INTERFACE -Werror)
endif()
target_include_directories(mybot_warnings INTERFACE src ${LUAJIT_INCLUDE_DIRS})

# Lua 运行时链接（仅 mybot / mybot_tests；mtmerge 是纯离线矩阵工具）
add_library(mybot_common INTERFACE)
target_link_libraries(mybot_common INTERFACE
  mybot_warnings
  ${LUAJIT_LIBRARIES}
  pthread
  dl
  m
)

add_executable(mybot
  src/main.cpp
  ${MYBOT_CORE_SOURCES}
)

# mtmerge：.mt 动作脚本合成器（离线编译目标，不依赖 LuaJIT 运行时）。
# N-11：矩阵解析入口（mt::readMatrix）下沉 matrix 层后，它只需
# matrix/sqlparse 一层 + actionsource 的头文件常量，不再链接整份 core。
# F1/N-30：归并核心抽到 mtmerge_core（与 mybot_tests 共用，白盒单测可覆盖）。
add_executable(mtmerge
  src/mtmerge.cpp
  src/mtmerge_core.cpp
  src/matrix.cpp
  src/expr.cpp
  src/sqlparse.cpp
)
target_link_libraries(mtmerge PRIVATE mybot_warnings)

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
  test/cpp/test_jsonparse.cpp
  test/cpp/test_llmconfig.cpp
  test/cpp/test_matrix.cpp
  test/cpp/test_expr.cpp
  test/cpp/test_sqlparse.cpp
  test/cpp/test_util.cpp
  test/cpp/test_cliargs.cpp
  test/cpp/test_hardware.cpp
  test/cpp/test_robot.cpp
  test/cpp/test_luaend.cpp
  test/cpp/test_luavar.cpp
  test/cpp/test_actionsource.cpp
  test/cpp/test_daemon.cpp
  test/cpp/test_daemon_concurrency.cpp
  test/cpp/test_mtmerge.cpp
  test/cpp/test_mtinclude.cpp
  test/cpp/test_agentproto.cpp
  test/cpp/test_agentdriver.cpp
  test/cpp/test_agenttools.cpp
  test/cpp/test_agentloop.cpp
  test/cpp/test_promptpack.cpp
  src/mtmerge_core.cpp
  ${MYBOT_CORE_SOURCES}
)

target_include_directories(mybot_tests PRIVATE test/cpp)
# 源码根路径：让单测能锚定 docs/ 与仓库夹具（--help 黄金测试据此比对
# docs/reference.md 的选项表），不依赖运行期 CWD。
target_compile_definitions(mybot_tests PRIVATE MYBOT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
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
add_executable(test_v4l2_consts test/test_v4l2_consts.c)
add_test(NAME const_ioctl COMMAND test_ioctl_nums)
add_test(NAME const_uart COMMAND test_uart_consts)
add_test(NAME const_v4l2 COMMAND test_v4l2_consts)

# ======================================================================
# libcurl 常量自检（F-24 同族）：llm_http.lua 硬编码的 CURLOPT/CURLINFO 编号
# 与 curl.h 编译期比对。libcurl 开发头文件**不是**构建依赖（mybot 运行期才
# dlopen libcurl.so），故找到头文件才构建本目标；run_all.sh 对缺失产物按
# 跳过计（与缺 python3 同口径），CI 安装 libcurl4-openssl-dev 保证门禁覆盖。
# ======================================================================
find_path(CURL_INCLUDE_DIR curl/curl.h)
if(CURL_INCLUDE_DIR)
  add_executable(test_curl_consts test/test_curl_consts.c)
  target_include_directories(test_curl_consts PRIVATE ${CURL_INCLUDE_DIR})
  add_test(NAME const_curl COMMAND test_curl_consts)
else()
  message(STATUS "libcurl headers not found - test_curl_consts not built (CURLOPT const check skipped)")
endif()

# ======================================================================
# Lua 离线回归逐用例注册（run_all.sh 保持主编排入口不变）：
# 每个 test_*.lua 都是 `luajit test/xxx.lua` 可单跑的独立用例（HAL 依赖 ffi
# 必须 LuaJIT）。真机用例（文件头 "-- requires: i2c" / "video"）经
# test/lib/device_gate.sh 包装：本机无对应 /dev/<dev>* 时以 125 退出，
# 由 SKIP_RETURN_CODE 记为跳过——**不再按文件名特判**（阶段 5.2 前只特判
# test_jit_warmup.lua，导致同样带 video 标记的 test_camera_v4l2.lua 被裸注册，
# 无摄像头机器上 ctest 报 Failed 而不是 Skipped）。标记解析与 run_all.sh 同源。
# 收益：ctest -R lua_ 获得逐用例粒度与超时，不必整段跑完 run_all.sh 守门脚本。
# ======================================================================
file(GLOB MYBOT_LUA_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/test/test_*.lua)
find_program(LUAJIT_EXECUTABLE NAMES luajit)
if(LUAJIT_EXECUTABLE)
	foreach(lua_test IN LISTS MYBOT_LUA_TESTS)
		get_filename_component(lua_name "${lua_test}" NAME)
		add_test(NAME lua_${lua_name}
			COMMAND ${CMAKE_SOURCE_DIR}/test/lib/device_gate.sh
				"${lua_test}" ${LUAJIT_EXECUTABLE} "${lua_test}")
		set_tests_properties(lua_${lua_name} PROPERTIES
			WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
			TIMEOUT 60
			SKIP_RETURN_CODE 125
			ENVIRONMENT "LUACMD=${LUAJIT_EXECUTABLE}")
	endforeach()
endif()

# 安装产物：mybot 直跑 ./build/mybot（bot4cj 原 _run.sh 的等价物，见 test/run_all.sh）
install(TARGETS mybot RUNTIME DESTINATION bin)