/* hw_registry.cpp —— HwRegistry：硬件变量注册表（拆自 robot.cpp 阶段 R-1）
 * 所有权（pool_）+ 销毁序（seq_）+ 名字索引（byName_）+ 流引用（flowRefs_）
 * 的单一来源，与解析/装配/派发解耦。 */

#include "robot.hpp"

/* 本 TU 只装配标量变量（@Vars），显式引入 */
#include "luavar.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace bot {

/* ---- HwRegistry：结构期注册（重名即抛）---- */
void HwRegistry::add(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw,
		const std::string& source) {
	if (byName_.count(name)) {
		throw IllegalArgumentException("Duplicate hardware variable name: '" + name + "'" + source);
	}
	/* R-2 遗留缺口：三个容器的写入顺序原为 byName_ → seq_ → pool_，一旦后两次
	 * push_back 抛 bad_alloc，byName_ 已指向 hw 持有的对象，而 hw 随栈展开析构
	 * ——注册表留下悬垂指针，后续 find/派发即 UB。
	 * 收口：先 reserve 两个 vector（此后其指针元素 push_back 不再分配），再把所有权
	 * 落到 pool_，最后登记非所有权索引。唯一可能失败的 map 节点分配排在 pool_ 已
	 * 提交之后，失败时回滚 pool_ ——对象随 unique_ptr 析构，不留半个条目。 */
	pool_.reserve(pool_.size() + 1);
	seq_.reserve(seq_.size() + 1);
	pool_.push_back(std::move(hw));
	MatrixVars_Hardware* p = pool_.back().get();
	try {
		byName_.emplace(name, p);
	} catch (...) {
		pool_.pop_back();
		throw;
	}
	seq_.push_back(p);		/* reserve 之后无分配，不抛 */
}

/* ---- HwRegistry：热插事务回滚（受控放宽见头文件 Mark 注释）---- */
void HwRegistry::rollbackTo(const Mark& m) {
	/* 不变量：pool_ 与 seq_ 锁步等长（add 里同序 push、失败路径成对回滚） */
	if (m.seq > seq_.size() || seq_.size() != pool_.size()) {
		throw IllegalArgumentException("HwRegistry::rollbackTo: mark is stale (seq="
			+ std::to_string(m.seq) + ", registry seq=" + std::to_string(seq_.size())
			+ ", pool=" + std::to_string(pool_.size()) + ")");
	}
	/* 1) 摘名字索引：按尾部指针集合过滤（byName_ 是名字 → 指针的只读视图，
	 *    没有反向索引，故这里只能扫一遍；条目数是结构规模，热插批次无感）。 */
	std::set<const MatrixVars_Hardware*> doomed(seq_.begin() + static_cast<std::ptrdiff_t>(m.seq),
		seq_.end());
	for (auto it = byName_.begin(); it != byName_.end();) {
		if (doomed.count(it->second) != 0) { it = byName_.erase(it); } else { ++it; }
	}
	/* 2) 弹出销毁序与所有权（元素析构；调用方已先行 destroy() 断电） */
	seq_.resize(m.seq);
	pool_.resize(m.seq);
	/* 3) 流引用精确还原：批次内 addFlowRef 过的新名字不得留下——否则该名字
	 *    永久拒绝会话别名覆盖（N-6），而它已随本批次一起撤销。 */
	flowRefs_ = m.flowRefs;
}

/* ---- HwRegistry：会话期别名覆盖（阶段 1：写进会话覆盖层）----
 * 不变量：会话层非空 ⟺ 正在一次 RobotStruct::run() 之内（由 SessionScope 维护）。
 * 与结构期彻底分离的收益：
 *   ① 结构定义不可能被会话变量永久遮蔽——旧实现直接改写 byName_，进程退出前
 *      一直生效（实测：一个连 @Action 都没有、以报错收场的脚本也能改写注册表）；
 *   ② pool_ 只属于结构期，MetaRef::target 的裸指针不变式与 MtVars_Lua::flowTarget
 *      的指针缓存从人工约定升级为构造保证；
 *   ③ 长驻（agent REPL）反复 run() 不再无界增长——旧实现每轮都往 pool_ 堆一份
 *      永不回收的旧别名（实测 200 轮 → byName_ 多 200 个名字）。 */
void HwRegistry::override(const std::string& name, std::unique_ptr<MatrixVars_Hardware> hw) {
	/* N-6：被引用为 controller/trigger 的名字不得覆盖——否则派发按名
	 * 查到别名（MtVars_Alias），报出的 "non-table result" 与真实成因
	 * （引用目标被动作脚本覆盖）完全脱节。 */
	if (flowRefs_.count(name)) {
		throw IllegalArgumentException("action script alias must not override '"
			+ name + "': it is referenced as a controller or trigger by another hardware variable");
	}
	/* 强异常安全：map 节点分配失败时旧条目原样保留（调用前状态），
	 * 不留半个条目、也不动结构期三个容器。 */
	session_[name] = std::move(hw);
}

/* N-15：表达式标识符求值器单点——三个调用面（元数据 $(...)、@Vars 装配、
 * 动作表达式列）此前各自内联同一形态的 lambda。 */
namespace {
double resolveVarsNumber(const HwRegistry* reg, const std::string& name);
} // namespace

std::function<double(const std::string&)> HwRegistry::makeVarsResolver() const {
	const HwRegistry* reg = this;
	return [reg](const std::string& nm) { return resolveVarsNumber(reg, nm); };
}

/* 表达式标识符求值器（@Action 求值面）：@Vars → 数值真值（numericValue，
 * 经 formatNumber 渲染为拼接文本——"2e3" 的真值是 "2000"）、@Alias → 文本
 * （别名值）；驱动/守护名与未定义名抛错。仅 Action 路径使用（数据格/列头），
 * @Vars 值与元数据列维持 vars-only 的 makeVarsResolver。 */
std::function<ExprValue(const std::string&)> HwRegistry::makeActionExprResolver() const {
	const HwRegistry* reg = this;
	return [reg](const std::string& nm) -> ExprValue {
		MatrixVars_Hardware* hv = reg->find(nm);
		if (hv == nullptr) {
			throw IllegalArgumentException("expression references '" + nm
				+ "', but it is not a defined hardware variable");
		}
		if (!hv->isScalarText()) {
			throw IllegalArgumentException("expression references '" + nm
				+ "', which is a driver/daemon variable; only @Alias/@Vars may"
				+ " appear in an expression");
		}
		if (std::string_view(hv->kindName()) == "vars") {
			return ExprValue::makeNum(static_cast<MtVars_Vars*>(hv)->numericValue());
		}
		return ExprValue::makeText(hv->read().asString().getValue());	/* kindName 已判型 */
	};
}

/* 表达式标识符求值：查注册表，必须是 @Vars（数值），递归取数。
 * 环（A→B→A）由 MtVars_Vars::numericValue 的深度计数兜底。 */
namespace {
double resolveVarsNumber(const HwRegistry* reg, const std::string& name) {
	MatrixVars_Hardware* hv = reg->find(name);
	if (hv == nullptr) {
		throw IllegalArgumentException("expression references '" + name
			+ "', but it is not a defined hardware variable");
	}
	if (std::string_view(hv->kindName()) != "vars") {
		throw IllegalArgumentException("expression references '" + name
			+ "', which is not an @Vars; only numeric @Vars may appear in an expression");
	}
	return static_cast<MtVars_Vars*>(hv)->numericValue();	/* kindName 已判型 */

}
} // namespace

} // namespace bot
