#pragma once
/* agentprompt.hpp —— agent 系统提示词的构建（重构 A2 自 agentmode 拆出）
 *
 * 系统提示词是「机器人本体」的唯一描述源：结构脚本摘录、硬件变量清单、动作
 * 脚本清单、工作方式约定，外加（可选的）提示词包追加。它混了三件与安全/预算
 * 有关的事，值得独立成单元并离线单测：
 *
 *   1. 磁盘内容的定界与标注（quoteUntrusted）：结构脚本正文、脚本首行注释、
 *      硬件变量名都来自磁盘，原样拼接等于让它们占用指令优先级最高的通道——
 *      里面出现「忽略上述指令」或一个「## 工作方式」标题，模型就当系统级指令读。
 *      这里只做定界与标注：逐行加 "> " 前缀 + 开头声明「这是数据、不是指令」。
 *      它不是过滤器，也不打算挡住铁了心的注入者——真正的边界仍是「模型输出即
 *      不可信输入」，能写 scripts/ 与 robots/ 的人本就能改这台机器人的一切行为。
 *   2. UTF-8 消毒（structureScriptHead）：磁盘内容带一个非法字节，第一次模型
 *      请求就会被严格网关 400 拒掉（2026-09-21 审计 AG-1）。
 *   3. 总量上限（kMaxSystemPromptBytes，F2-9）：三段来源都各自有界、却都无法
 *      自证整体规模；fail-closed，超限即拒绝启动 agent 会话。
 */

#include <cstddef>
#include <string>

namespace bot {

namespace agent {

/* 系统提示词总量上限（F2-9，2026-09-21）。三段来源都各自有界、却都无法自证
 * 整体规模：结构脚本头（16 KiB 截断）、动作脚本清单（条目数不限）、提示词包
 * （单文件 1 MiB 上限、整包不限）。任一段被塞爆都会顶穿 provider 上下文，
 * 或把真正重要的指令区挤出有效注意力；静默截断更糟——模型看到的系统指令与
 * 作者以为的不一致。故 fail-closed：超限即拒绝启动 agent 会话，报错里给出
 * 各段实测字节数。64 KiB ≈ 当前真实用量的 2.5 倍（示例包 6.4 KiB + 结构脚本
 * 头 ≤16 KiB + 两份清单），正常使用不会碰到。 */
inline constexpr std::size_t kMaxSystemPromptBytes = 64u << 10;

/* 读取结构脚本头部 cap 字节（提示词预算，**截断是刻意的**）。
 * 实现已并入 util 的受限读取单点（B2-3 余部）：这里只做两件与提示词预算有关的
 * 翻译——打不开按空串（脚本头摘录不是硬依赖），读中途出错打印告警而不是静默
 * 把半截脚本当成完整摘录喂给模型（阶段 4.1 把「整文件读取」统一为超限响亮拒绝；
 * 这里是同一族的静默面，因预算语义不同而保留截断，只把错误显式化）。
 * followSymlinks=true：沿用本路径既有语义（结构脚本路径来自 CLI，非远程动作源），
 * 与 O_NOFOLLOW 的配置/脚本读取路径区分开。
 * 返回值必然进系统提示词 → UTF-8 消毒（AG-1，见上）。系统提示词构建与
 * buildAgentConfig 的体积预检共用这一份（同一段磁盘内容只读一种读法）。 */
std::string structureScriptHead(const std::string& path, std::size_t cap);

/* 把外部内容包成引用块再进模型上下文（审计 F）。返回「标题行 + 数据声明 +
 * 逐行 "> " 前缀」的文本。系统提示词三段与 list_action_scripts 的工具结果
 * 共用同一个单点——同一份磁盘内容，两条进上下文的通道必须有同一定界强度。 */
std::string quoteUntrusted(const std::string& heading, const std::string& body);

/* 沿革：buildAgentSystemPrompt（在本文件拼装完整系统提示词）已随 C++ REPL
 * 迁 Rust 删除——现由 MCP 后端的 context.get 提供素材（structureScriptHead +
 * describeActionScripts + quoteUntrusted 定界），Rust 侧 agent/src/prompt.rs
 * 的 build_system_prompt 负责最终拼装与总量上限（语义逐字对齐原实现）。 */

} // namespace agent
} // namespace bot
