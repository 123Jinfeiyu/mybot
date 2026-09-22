#include "agentbudget.hpp"

namespace bot {
namespace agent {

bool TaskBudget::admit(std::string& text, std::string& image) {
	/* ---- 任务内预算判定（AG-4 + AG-21）---- */
	const bool textOver = maxToolResultBytes > 0
		&& toolResultBytes + text.size() > maxToolResultBytes;
	const bool imgOver = !image.empty() && maxImageBytes > 0
		&& imageBytes + image.size() > maxImageBytes;
	if (textOver || imgOver) {
		const std::string what = imgOver ? "图片" : "工具输出";
		const std::size_t used = imgOver ? imageBytes : toolResultBytes;
		const std::size_t cap = imgOver ? maxImageBytes : maxToolResultBytes;
		std::string note = "本次工具结果未注入上下文：" + what + "累计体积已达任务上限（已用 "
			+ std::to_string(used) + " + 本次 "
			+ std::to_string(imgOver ? image.size() : text.size())
			+ " 字节，上限 " + std::to_string(cap) + " 字节）。";
		if (imgOver) {
			/* AG-4′（第三轮审计）：这里曾原样拼进 capture_image 的成功文案，而那句
			 * 末尾是「画面已加入你的上下文」——与本条开头的「未注入上下文」直接
			 * 矛盾，而**图片确实没注入**。模型读到自相矛盾的两句，按哪句行事都错。
			 * 本项目既有的口径是「不向模型谎报状态」，所以把描述里那句成功断言
			 * 剥掉，再显式说明「只把描述给你」。 */
			std::string desc = text;
			const std::string claim = "画面已加入你的上下文。";
			if (const std::size_t at = desc.find(claim); at != std::string::npos) {
				desc.erase(at, claim.size());
			}
			note += "已把工具的描述性文字给你（画面本身没有进上下文）：" + desc;
			/* 图片预算是**逐任务单向累加**的：一旦某张图装不下，本任务就再也塞不进
			 * 任何图片（额度只增不减）。不明说的话，模型会一直重试拍照直到轮次上限
			 * （实测：一次任务里连拒 3 次后以「达到工具轮次上限」收场——那 3 轮全
			 * 是白烧的）。这里把「本任务图片额度已尽」讲成硬事实并给出唯一出路。
			 * 判据必须与上面触发降级的判据**逐字一致**（都是「已用 + 本次 > 上限」）：
			 * 写成 used >= cap 是错的——本次 87 / 上限 100 / 已用 87 时 used < cap，
			 * 但再拍一张必然还是 87+87 > 100，额度事实上已经用尽（实测踩过，
			 * 用例 agentloop_task_budget_image_exhaustion_is_terminal_and_stated
			 * 把它钉住了）。 */
			if (used + image.size() > cap) {
				note += "本任务的图片额度已经用尽，**再调用也不会成功**；"
					"请立即改用文字结论或换一个新任务。";
			}
		}
		note += "工具本身已执行完毕（副作用已发生）。请改用更小的步子推进任务，"
			"或直接基于已有信息给出结论。";
		/* AG-21：降级说明**自己也计入预算**——否则「反复超限」可以往历史里无界
		 * 灌说明文本（每条约 300 字节 x 轮数 x 每轮调用数）。计入之后第一次降级
		 * 就把额度顶满，后续调用走同一分支且文案不变（幂等，不会雪崩）。 */
		toolResultBytes += note.size();
		text = std::move(note);
		image.clear();
		return true;
	}
	toolResultBytes += text.size();
	imageBytes += image.size();
	return false;
}

} // namespace agent
} // namespace bot
