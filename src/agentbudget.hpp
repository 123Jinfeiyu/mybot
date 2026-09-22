#pragma once
/* agentbudget.hpp —— 任务内聚合预算（重构 A1，从 AgentSession 拆出）
 *
 * maxTurns 只管「模型回合数」，历史裁剪（HistoryManager::trim）只管「已完结
 * 的旧任务块」且最新块无条件保留——于是**正在进行的任务**原本没有任何体积
 * 上限：模型多调几次工具就能在一个任务里把请求体顶穿网关限额，而那块历史
 * 既不在回滚范围内、也不能被裁掉，REPL 从此每条任务都以同一个请求体失败。
 *
 * TaskBudget 约束「单个任务往上下文里灌了多少字节」：
 *   * 文本与图片（data URL）两条额度逐次累加，超限时**不把该次结果放进
 *     历史**，原地替换为一句降级说明（admit 返回 true）；
 *   * 为什么是「换掉」而不是「截断」：截断工具输出会让模型看到半截内容却
 *     以为完整（比没有更危险）；为什么不是「报错终止」：那是把可用性问题
 *     升级成任务失败，而模型完全有能力换一个小一点的步子重试；
 *   * 判定在**执行之后**做：体积要拿到结果才知道，而工具已经产生副作用
 *     （比如动作脚本已经跑了），此时谎称「没执行」会误导模型——降级说明
 *     必须如实写明「已执行，但结果体积超限」（项目口径：不向模型谎报状态）。
 * 每轮任务开始时由调用方 resetTask() 清零；跨任务的体积由 HistoryManager 管。
 */

#include <cstddef>
#include <string>

namespace bot {
namespace agent {

struct TaskBudget {
	std::size_t maxToolResultBytes;	/* 0 = 不限 */
	std::size_t maxImageBytes;	/* 0 = 不限 */
	std::size_t toolResultBytes = 0;	/* 当前任务已注入的文本字节 */
	std::size_t imageBytes = 0;		/* 当前任务已注入的图片字节 */

	void resetTask() {
		toolResultBytes = 0;
		imageBytes = 0;
	}

	/* 预算判定与记账（原地改写）。超限时把 *text 替换为降级说明、*image
	 * 清空并返回 true；否则把两者计入额度并返回 false。
	 * 文案与记账语义与拆分前逐字一致（含 AG-21 三条边界：降级说明自己也
	 * 计入预算、图片额度逐任务单向累加并在用尽时明示终态、降级拼图时不
	 * 复用工具成功文案里的「画面已加入你的上下文」断言）。 */
	bool admit(std::string& text, std::string& image);
};

} // namespace agent
} // namespace bot
