#pragma once
/* jsonparse.hpp —— 严格 JSON 文本解析（JSON 文本 → bot::Json）
 *
 * 为什么需要它：本项目原有的 Json 只做「程序构造 + 序列化」（矩阵/元数据内部
 * 流转），从不需要读入外部 JSON 文本。agent 模式要解析模型响应（OpenAI 兼容
 * 协议的 JSON 报文），这是仓库里第一条「外部 JSON 文本入口」，故单列一层。
 *
 * 与 Lua/驱动的边界无关：本层不依赖 LuaJIT，纯值解析，可离线单测。
 */

#include <cstddef>
#include <string>

#include "json.hpp"

namespace bot {

/* 解析上限（拒绝而非截断）：
 * - 字节数 8 MiB：与脚本输入上限同量级，模型响应实测为 KB 级；
 * - 嵌套深度 64：递归下降解析深嵌套会吃栈，而模型可被输入诱导吐出
 *   "[[[[..." 之类的深嵌套；上限之下任何合法响应都绰绰有余。 */
inline constexpr std::size_t kJsonMaxBytes = 8u << 20;
inline constexpr int kJsonMaxDepth = 64;

/* 解析整串 JSON。
 * 严格语义：必须整体消费（尾部多余非空白字符即报错），不接受注释、
 * 单引号、尾随逗号、NaN/Infinity 等扩展。失败抛 IllegalArgumentException，
 * 消息带出错字节偏移（定位长报文里的坏点）。 */
Json parseJson(const std::string& text);

} // namespace bot
