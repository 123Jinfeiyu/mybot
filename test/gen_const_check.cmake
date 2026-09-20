# gen_const_check.cmake —— 从 Lua 源**抽取**硬件常量，生成 C 断言用的宏头（A2 收口）
#
# 背景（2026-09-20 审计 A2，严重）：`test_ioctl_nums.c` / `test_uart_consts.c` /
# `test_v4l2_consts.c` / `test_curl_consts.c` 声称把「HAL.lua / Camera.lua /
# llm_http.lua 硬编码的编号」与系统头比对，实际却把那些数字**在 C 里又手抄了
# 一遍**，从不打开 Lua 源（四个文件里没有任何 fopen/read）。于是 Lua 侧漂移
# 完全不被发现——实测把 HAL.SPI_IOC_WR_MODE 改成 0xDEADBEEF，四个自检全部照绿。
# 本脚本把链路接上：**Lua 源是唯一事实源**，构建期抽取为宏，再由 C 断言同时钉住
# 「Lua 值 == 手抄的预期值」（人写错/抽取失效会红）与「预期值 == 系统头」
# （内核/架构漂移会红）。两侧都钉死，任一侧漂移构建即失败。
#
# 用法：cmake -DOUT=<生成的头文件> -DSRC=<仓库根> -P test/gen_const_check.cmake
#
# 抽取是**纯文本模式匹配**，不执行 Lua（不依赖 luajit、不受 CWD 影响）：每个常量
# 的行格式在本仓库内是稳定的（`HAL.NAME<spaces>=<spaces>值`）。抽不到即
# FATAL_ERROR —— 「抽取失效」必须响亮失败，而不是静默生成空头让断言退化成自证。

if(NOT DEFINED OUT OR NOT DEFINED SRC)
  message(FATAL_ERROR "gen_const_check.cmake: 需要 -DOUT=<头文件> -DSRC=<仓库根>")
endif()

# read_const(<绝对路径> <前缀> <输出变量>)：逐行匹配 `^<前缀><NAME> = <值>`
# （前缀里的 '.' 不转义，因为用了 MATCHES 的 ERE —— 那里 '.' 本就匹配任意字符，
# 对本文件这类受控输入足够且更简单）。捕获 NAME 与值，值取到行尾（去掉 `--` 注释）。
# 返回分号分隔的 `NAME=值` 列表。
function(read_const path prefix out_var)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "gen_const_check.cmake: 源文件不存在：${path}")
  endif()
  file(READ "${path}" content)
  # 去掉 `--` 行注释（保留换行），避免注释里的数字被吃进值；随后按行扫描，
  # 因为 ^...$ 锚定针对的是**行**而不是整份文件。
  string(REGEX REPLACE "--[^\n]*" "" content "${content}")
  string(REPLACE "\n" ";" lines "${content}")
  set(acc "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*${prefix}([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*([^ \t;].*)$")
      set(name "${CMAKE_MATCH_1}")
      set(val "${CMAKE_MATCH_2}")
      # 去掉值尾部的空白
      string(STRIP "${val}" val)
      # 只收「纯字面量或字面量算术」——含标识符的值（如 bus.UART_READ_MAX_MS、
      # UART_B_CONST_HZ）不是常量表项，跳过：本检查的定位是编号/布局常量。
      if(val MATCHES "^[0-9A-Fa-fxX|+() \t]+$")
        list(APPEND acc "${name}=${val}")
      endif()
    endif()
  endforeach()
  set(${out_var} "${acc}" PARENT_SCOPE)
endfunction()

# require_const(<列表> <NAME>)：列表里必须有该 NAME，否则 FATAL_ERROR。
function(require_const lst name)
  foreach(entry IN LISTS lst)
    if(entry MATCHES "^${name}=")
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "gen_const_check.cmake: 未能从 Lua 源抽取常量 '${name}' —— 常见成因："
    "源文件里的书写格式变了（本脚本按 `前缀<NAME> = <值>` 纯文本匹配），或该常量被删除/改名。"
    "抽取失效必须失败：静默生成空头会让常量自检退化为自证（A2 的原始缺陷形态）。")
endfunction()

set(SRC_MODULES "${SRC}/modules")

read_const("${SRC_MODULES}/lib/HAL.lua" "HAL\\." HAL_ALL)
read_const("${SRC_MODULES}/lib/llm_http.lua" "local[ \t]+" LLM_ALL)
# Camera.lua 的 V4L2 ioctl 编号也是 `local NAME = <值>`（同名声明），与 llm_http 同法抽取。
read_const("${SRC_MODULES}/Camera.lua" "local[ \t]+" CAM_ALL)

# 必须存在的常量（少一个就失败）——覆盖四个自检文件当前使用的全部编号/布局常量。
foreach(n IN ITEMS
    I2C_RDWR I2C_M_RD
    SPI_IOC_WR_MODE SPI_IOC_WR_BITS_PER_WORD SPI_IOC_WR_MAX_SPEED_HZ SPI_IOC_MESSAGE_1
    GPIO_GET_LINEHANDLE GPIOHANDLE_GET_LINE_VALUES GPIOHANDLE_SET_LINE_VALUES
    UART_O_FLAGS UART_TCSANOW UART_TCIFLUSH UART_VMIN_IDX UART_VTIME_IDX UART_CREAD_CLOCAL)
  require_const("${HAL_ALL}" "${n}")
endforeach()
foreach(n IN ITEMS
    OPT_URL OPT_POSTFIELDS OPT_POSTFIELDSIZE OPT_WRITEFUNCTION OPT_HTTPHEADER
    OPT_TIMEOUT OPT_CONNECTTIMEOUT OPT_NOSIGNAL INFO_RESPONSE_CODE CURL_GLOBAL_ALL)
  require_const("${LLM_ALL}" "${n}")
endforeach()
foreach(n IN ITEMS
    VIDIOC_QUERYCAP VIDIOC_S_FMT VIDIOC_REQBUFS VIDIOC_QUERYBUF
    VIDIOC_QBUF VIDIOC_DQBUF VIDIOC_STREAMON VIDIOC_STREAMOFF
    BUF_TYPE_VIDEO_CAPTURE MEMORY_MMAP CAP_VIDEO_CAPTURE PIX_FMT_MJPEG)
  require_const("${CAM_ALL}" "${n}")
endforeach()

# Camera.lua 的 ABI 尺寸表 `{ "cam_buffer", 88 }`：ffi 声明尺寸的期望值同样
# 是硬编码在 Lua 里的，一并抽出来钉住（改 Lua 声明不改期望值 → 构建失败）。
function(read_abi_table path out_var)
  file(READ "${path}" content)
  string(REGEX REPLACE "--[^\n]*" "" content "${content}")
  string(REGEX MATCHALL "\\{[ \t]*\"([A-Za-z_][A-Za-z0-9_]*)\"[ \t]*,[ \t]*([0-9]+)[ \t]*\\}" pairs "${content}")
  set(acc "")
  foreach(p IN LISTS pairs)
    string(REGEX MATCH "\\{[ \t]*\"([A-Za-z_][A-Za-z0-9_]*)\"[ \t]*,[ \t]*([0-9]+)[ \t]*\\}" _ "${p}")
    list(APPEND acc "${CMAKE_MATCH_1}=${CMAKE_MATCH_2}")
  endforeach()
  list(LENGTH acc n)
  if(n LESS 6)
    message(FATAL_ERROR "gen_const_check.cmake: Camera.lua ABI 尺寸表只抽到 ${n} 项（期望 6）")
  endif()
  set(${out_var} "${acc}" PARENT_SCOPE)
endfunction()
read_abi_table("${SRC_MODULES}/Camera.lua" CAM_ABI)

# UART 波特率映射表：`[<hz>] = <const>`（本地表，非 HAL. 字段）。两套表都要抽：
# 旧 ABI 编码（glibc < 2.42）与字面 Hz 编码（glibc >= 2.42），由 test_uart_consts.c
# 各自断言——两套都正确才说明 HAL 的运行时二选一逻辑两侧都有依据。
function(read_baud_table path varname out_var)
  file(READ "${path}" content)
  string(REGEX REPLACE "--[^\n]*" "" content "${content}")
  # 截取 `local UART_B_CONST_<varname> = {` 到下一个 `}` 之间的块
  string(REGEX MATCH "UART_B_CONST_${varname}[ \t]*=[ \t]*\\{([^}]*)\\}" block "${content}")
  if(block STREQUAL "")
    message(FATAL_ERROR "gen_const_check.cmake: 未找到 UART_B_CONST_${varname} 表块")
  endif()
  set(acc "")
  # 逐个 `[<hz>] = <值>` 取对；同一行多个条目也能覆盖（全局匹配）。
  string(REGEX MATCHALL "\\[([0-9]+)\\][ \t]*=[ \t]*([0-9A-Fa-fxX]+)" pairs "${block}")
  foreach(p IN LISTS pairs)
    string(REGEX MATCH "\\[([0-9]+)\\][ \t]*=[ \t]*([0-9A-Fa-fxX]+)" _ "${p}")
    list(APPEND acc "${CMAKE_MATCH_1}=${CMAKE_MATCH_2}")
  endforeach()
  list(LENGTH acc n)
  if(n LESS 13)
    message(FATAL_ERROR
      "gen_const_check.cmake: UART_B_CONST_${varname} 只抽到 ${n} 个条目（期望 13）")
  endif()
  set(${out_var} "${acc}" PARENT_SCOPE)
endfunction()

read_baud_table("${SRC_MODULES}/lib/HAL.lua" "LEGACY" BAUD_LEGACY)
read_baud_table("${SRC_MODULES}/lib/HAL.lua" "HZ" BAUD_HZ)

# ---- 生成头文件 ----
set(hdr "/* 本文件由 test/gen_const_check.cmake 在 cmake 配置期生成，请勿手工编辑。\n")
string(APPEND hdr " * 事实源：modules/lib/HAL.lua 与 modules/lib/llm_http.lua（纯文本抽取）。\n")
string(APPEND hdr " * 常量自检（test_ioctl_nums.c / test_uart_consts.c / test_curl_consts.c）\n")
string(APPEND hdr " * 用它把 Lua 值钉进 C 断言——A2：此前 C 里手抄数字，Lua 漂移无人发现。 */\n")
string(APPEND hdr "#pragma once\n\n")

function(emit_defs hdr_var prefix lst)
  set(buf "")
  foreach(entry IN LISTS lst)
    string(REGEX MATCH "^([^=]+)=(.*)$" _ "${entry}")
    set(name "${CMAKE_MATCH_1}")
    set(val "${CMAKE_MATCH_2}")
    string(APPEND buf "#define ${prefix}${name} (${val})\n")
  endforeach()
  set(${hdr_var} "${${hdr_var}}${buf}" PARENT_SCOPE)
endfunction()

emit_defs(hdr "HAL_CONST_" "${HAL_ALL}")
string(APPEND hdr "\n")
emit_defs(hdr "LLM_CONST_" "${LLM_ALL}")
string(APPEND hdr "\n")
emit_defs(hdr "V4L2_CONST_" "${CAM_ALL}")
string(APPEND hdr "\n")
foreach(entry IN LISTS CAM_ABI)
  string(REGEX MATCH "^([^=]+)=(.*)$" _ "${entry}")
  string(APPEND hdr "#define V4L2_ABI_${CMAKE_MATCH_1} (${CMAKE_MATCH_2})\n")
endforeach()
string(APPEND hdr "\n")
foreach(entry IN LISTS BAUD_LEGACY)
  string(REGEX MATCH "^([0-9]+)=(.*)$" _ "${entry}")
  string(APPEND hdr "#define HAL_BAUD_LEGACY_${CMAKE_MATCH_1} (${CMAKE_MATCH_2})\n")
endforeach()
string(APPEND hdr "\n")
foreach(entry IN LISTS BAUD_HZ)
  string(REGEX MATCH "^([0-9]+)=(.*)$" _ "${entry}")
  string(APPEND hdr "#define HAL_BAUD_HZ_${CMAKE_MATCH_1} (${CMAKE_MATCH_2})\n")
endforeach()

file(WRITE "${OUT}" "${hdr}")
message(STATUS "gen_const_check: 已从 Lua 源抽取常量 -> ${OUT}")
