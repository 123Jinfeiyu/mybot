#pragma once
/* promptpack.hpp —— 提示词包加载（--prompt-file / 配置文件 prompt_files）
 *
 * 移植自 robot_agent 的提示词包能力：把一组 Markdown 文件（机器人身份、行为
 * 纪律、能力约束等）拼接成一段文本，追加到系统提示词之后，用于把通用 agent
 * 绑定成具体机器人的"大脑"。内置的系统提示词只讲机器人本体（结构/硬件/脚本
 * 清单/工作方式），"这个机器人是谁、该怎么行事"交给提示词包定义。
 *
 * 加载发生在 buildAgentConfig（配置组装期）而非系统提示词拼装期：提示词路径
 * 写错属于配置错误，必须在结构装载与上电之前失败（main 的约定「配置错误不碰
 * 真机」）。因此本模块只被配置组装调用一次，产出成品文本供 buildAgentSystemPrompt
 * 直接追加——后者保持纯函数，不做 I/O。
 *
 * 与 robot_agent 的语义差异（其余逐字保留）：
 *   - 异常改 bool + err 人话报错（与 loadLlmConfigFile 同一口径）；
 *   - 单文件读取加字节上限（robot_agent 无上限；本仓库所有宿主读取都设限：
 *     脚本 8 MiB、密钥文件 64 KiB、结构脚本 16 KiB，提示词为 KB 级文本，
 *     1 MiB 已留三个数量级余量，超限拒绝而非静默截断）。
 */

#include <cstddef>
#include <string>
#include <vector>

namespace bot {
namespace agent {

/* 单个提示词文件的字节上限（整包体积 = 文件数 × 上限，文件数由输入清单决定） */
inline constexpr std::size_t kMaxPromptFileBytes = 1u << 20;

/* 加载提示词包。inputs 为「文件或目录」路径清单（可混合、可重复传入）：
 *   - 目录：收集其中全部 .md（只收普通文件），按文件名自然排序——"2-身份.md"
 *     排在 "10-xx.md" 之前（字典序会把 10 排在 2 前面），编号即装载顺序；
 *   - 显式文件：保持传入顺序，不限扩展名。
 * 拼接规则：每个文件去首尾空白后为空则跳过；只有一份时保持原文不加标题；
 * 多份之间以空行分隔、每份前加 "## 文件名" 分节（分节标题用文件名，模型据此
 * 引用某一段约束）。
 * inputs 为空不是错误（提示词包是可选项），返回 true 且 out 为空串。
 * 失败返回 false 并把人话报错写进 err：路径不存在、目录没有 .md、文件不可读、
 * 超字节上限、全部内容为空。 */
bool loadPromptPack(const std::vector<std::string>& inputs, std::string& out,
		std::string& err);

} // namespace agent
} // namespace bot
