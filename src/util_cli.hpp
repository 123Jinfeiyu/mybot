#pragma once
/* util_cli.hpp —— 命令行基础设施：--key=value/--key value/裸 --key 的切分单点，
 * 与 daemon/TCP 选项的参数化区间常量（自 util.hpp 拆出，2026-09-26 批次 3）。 */

#include <string>

namespace bot {

/* ---- CLI 超时/上限参数化区间（2026-09-22 F-1/F-2/F-3，原 TD-2 剩余部分）----
 * 三个此前硬编码的运行期数值改为可选 CLI 参数（同款「不传 = 现状缺省值」
 * 零变更契约）。与 llm_limits（llmconfig.hpp）同一惯例：区间常量单点，
 * --help / 校验共用，避免「帮助写一个数、校验用另一个数」的漂移。这三个
 * 值不属于 agent 配置面（daemon/TCP 是全局能力，不限 --agent），故不进
 * llmconfig 的 LlmFileConfig，走普通 CLI 选项。 */
namespace daemon_limits {
/* RobotStruct::deactivate 的二段屏障宽限上限（原写死 kMaxGraceMs=30000）。
 * 下限 0：允许「不给弃管守护额外宽限」（宁可提前进入 destroy 竞态窗口，
 * 也要缩短停机时间——特殊场景下操作者可自行承担）。上限 300000（5 分钟）：
 * 弃管本就是「join 超时已经用尽」的兜底，再长的等待收益不大且会拖慢停机。 */
inline constexpr int kGraceMsMin = 0;
inline constexpr int kGraceMsMax = 300000;
inline constexpr int kGraceMsDefault = 30000;
} // namespace daemon_limits

namespace tcp_limits {
/* --tcp-max-auth-attempts：口令连续失败上限（原写死 kTcpAuthMaxFails=5）。
 * 下限 1：0 会让第一次失败就退出，与「重试」语义矛盾，不如干脆不设口令；
 * 上限 1000：远超任何合理部署，但不放开「无限」——无限重试等于放弃这条
 * 防爆破的护栏，与 max_calls_per_round 等「能力只能收紧」的口径不同源
 * （这里没有安全下界要保护，纯粹是「离谱数值大概率是打错」的常识上限）。 */
inline constexpr int kAuthMaxFailsMin = 1;
inline constexpr int kAuthMaxFailsMax = 1000;
inline constexpr int kAuthMaxFailsDefault = 5;
/* --tcp-total-timeout：连接总时长预算下限（原写死 tcpTotalBudgetSec 里的
 * 600）。生效值语义不变：max(该下限, idleSec*20)。下限 1：允许收紧到
 * 「基本不给慢滴对端机会」；上限 86400（24 小时）与 --tcp-idle 的既有上限
 * 同数量级，超过它的部署诉求应该用文件/TCP 之外的通道，而不是把连接
 * 挂一整天。 */
inline constexpr int kTotalTimeoutMin = 1;
inline constexpr int kTotalTimeoutMax = 86400;
inline constexpr int kTotalTimeoutDefault = 600;
} // namespace tcp_limits

namespace cli {

/* 单个 argv 选项 token 的解析结果。hasValue=false 表示裸 flag——值语义
 * 由调用方裁决（布尔 flag 置位 / 缺值报错），本层不掺和。 */
struct OptionToken {
	std::string key;
	std::string value;
	bool hasValue = false;
};

/* --key=value / --key value / 裸 --key 的切分单点（main.cpp 与 mtmerge.cpp
 * 此前各写一份）。调用方保证 argv[k] 以 "--" 开头：
 * - '=' 内联值优先，key 不含 '='；
 * - 无 '=' 时看 argv[k+1]：strictLookahead=true（mybot CLI，审计中-3）要求
 *   下一参数不以 "--" 开头才算值——裸 --tcp 不得把后续 flag 吞成端口；
 *   false（mtmerge takeVal 原语义）只要存在即消费。消费与否同步推进 k。 */
inline OptionToken parseKeyValue(int& k, int argc, char** argv, bool strictLookahead) {
	OptionToken tok;
	std::string a = argv[k];
	tok.key = a.substr(2);
	auto eq = tok.key.find('=');
	if (eq != std::string::npos) {
		tok.value = tok.key.substr(eq + 1);
		tok.key = tok.key.substr(0, eq);
		tok.hasValue = true;
	} else if (k + 1 < argc
			&& (!strictLookahead || std::string(argv[k + 1]).rfind("--", 0) != 0)) {
		tok.value = argv[++k];
		tok.hasValue = true;
	}
	return tok;
}

} // namespace cli

} // namespace bot
