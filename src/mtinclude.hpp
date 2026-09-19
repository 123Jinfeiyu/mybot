#pragma once
/* mtinclude.hpp —— .mt 动作脚本 @Include 展开（动作脚本合并机制的运行端入口）
 *
 * 离线合成器 mtmerge 的归并内核（mtmerge_core）抽出后即可被两端共用：
 * mtmerge 把 N 个输入两两归并成产物文件；本模块让主脚本用一张 @Include 矩阵
 * 声明要并入的 .mt（每行可带相位），RobotStruct::run 解析主脚本后调用
 * expandIncludes 就地展开，后续 @Alias/@Vars 注册与 @Action 派发走的路径
 * 与手写大脚本完全一致。
 *
 * 只依赖 matrix / mtmerge_core / actionsource 头，不触碰 LuaJIT——与
 * mtmerge_core 同层，白盒单测（test_mtinclude）无须硬件与 Lua 环境。
 */

#include <cstddef>
#include <string>
#include <vector>

#include "matrix.hpp"

namespace mtinclude {

/* @Include 数据行数上限：每行触发一次本地文件读取，无界行数等价于
 * 「远端下发的脚本扇出读盘」（--tcp 通道此前无法触达本地文件，@Include
 * 是第一个例外）。32 行覆盖任何现实编排；配合总字节预算双保险。 */
inline constexpr std::size_t kMaxIncludes = 32;

/* 展开主脚本矩阵中的全部 @Include（原地替换 parsed）：
 *   - 无 @Include 时逐字不动（fast path，既有脚本行为零变化）；
 *   - @Action 按 mtmerge 语义两两归并（设备集相交报错、相位补 "-"、
 *     负相位部分丢弃告警/全部丢弃报错）；
 *   - @Alias / @Vars 按行叠加（列头一致 + 同名行报错）；
 *   - 被包含文件里再出现 @Include 即报错（深度限 1，拒绝递归防环）。
 * 路径相对 baseDir 解析（缺省 "."），词法规则见实现处。 */
void expandIncludes(std::vector<mt::MatrixObject>& parsed, const std::string& baseDir);

} // namespace mtinclude
