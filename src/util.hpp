#pragma once
/* util.hpp —— 全局基础工具的**伞头**（2026-09-26 批次 3 拆分）
 *
 * 此前本文件是 808 行、被 30 个 TU 全量包含、全 inline 的「上帝头」：改任何一组
 * 工具都重编全部 30 个 TU。现按功能组拆成 8 个子头，本文件退化为伞头——仍包含
 * 全部子头与原有标准库头，故现有 `#include "util.hpp"` 的 TU 一字不改即可继续编译
 * （零破坏的过渡形态）。后续可让各 TU 改为只包含真正用到的子头（如只要 UTF-8 的
 * 包含 util_utf8.hpp），从而把「改一处、重编 30 个」收敛到「改一处、重编用到它的」。
 *
 * 子头一览：
 *   util_except.hpp —— 异常类型 / 递归·深度守卫 / 字节流 Reader
 *   util_env.hpp    —— 环境变量 / errno 文案 / 调试开关·惰性日志 / 信号停止标志
 *   util_str.hpp    —— 拼接 / 恒时比较 / 安全标识符 / 小写 / 切词 / trim / 回显转义
 *   util_num.hpp    —— 严格整数解析 / 越界报错尾段 / 有限浮点 / 值域判据 / 数值文本
 *   util_utf8.hpp   —— UTF-8 合法性判据与消毒
 *   util_file.hpp   —— fd RAII 守卫 / 受限读取 / EINTR 重试
 *   util_path.hpp   —— 模块路径搜索 / 相对路径安全判据
 *   util_cli.hpp    —— --key=value 切分 / daemon·tcp 选项区间常量
 */

/* 保留原有标准库头：过渡期内仍为依赖 util.hpp 传递包含这些头的 TU 兜底
 * （子头各自也已包含自身所需，供后续直接包含时自洽）。 */
#include <atomic>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <limits>
#include <unistd.h>

#include "util_except.hpp"
#include "util_env.hpp"
#include "util_str.hpp"
#include "util_num.hpp"
#include "util_utf8.hpp"
#include "util_file.hpp"
#include "util_path.hpp"
#include "util_cli.hpp"
