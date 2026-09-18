/* mtmerge.cpp —— .mt 动作脚本合成器（离线编译目标，独立于 mybot 运行时）
 *
 * 把两个（或多个）设备集不相交的 @Action 行列式脚本合并为一个：
 *   - 设备集（列头 <device>.<param> 切出的 device 名）相交即报错退出 2；
 *   - 行相位：整数 p，B 相对 A 整体滞后 p 行（负值提前），每次归并一个偏移；
 *   - 较短一方行数耗尽后补字面 "-"（继承链自然延伸，保持最后姿势）；
 *   - 列 = A 全列（原序） ∪ B 全列（原序）；产物为规范 .mt 文本，
 *     可直接喂给 mybot，也可继续喂回 mtmerge 做下一轮两两归并。
 *   - @Alias / @Vars 按行叠加（不做相位）：各输入的 data 行拼接；同名行
 *     （name 列）即报错退出 2；两侧列头必须一致。
 *
 * 用法：./build/mtmerge [--phase=N] [--out merged.mt] a.mt b.mt [c.mt ...]
 * N 个输入按序两两归并：先合 a+b，结果再合 c，依此类推。
 */

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "actionsource.hpp"
#include "matrix.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace {

/* 按矩阵名查找（trim 后比较）；不存在返回 nullptr。 */
const mt::MatrixObject* findNamed(const std::vector<mt::MatrixObject>& ll, const std::string& name) {
	for (const auto& m : ll) {
		if (bot::trim(m.getName()) == name) { return &m; }
	}
	return nullptr;
}

/* @Action：找到列头即最终形态（别名替换由 mybot 在装配期完成）。找不到即报错。 */
const mt::MatrixObject& requireAction(const std::vector<mt::MatrixObject>& parsed,
		const std::string& path) {
	const mt::MatrixObject* act = findNamed(parsed, "Action");
	if (act == nullptr) {
		throw bot::IllegalArgumentException("'" + path + "': no @Action matrix found");
	}
	return *act;
}

/* @Alias / @Vars：按行叠加（不含相位）。列头必须完全一致；name 列（首个名为
 * 'name' 的列）出现同名行即报错——同一名字不能映射到两个不同的值/别名。 */
mt::MatrixObject mergeScalarRows(const mt::MatrixObject& acc, const mt::MatrixObject& next,
		const std::string& label, const std::string& pa, const std::string& pb) {
	if (acc.cols() != next.cols()) {
		throw bot::IllegalArgumentException(label + " matrix column count differs: '" + pa
			+ "' has " + std::to_string(acc.cols()) + ", '" + pb + "' has "
			+ std::to_string(next.cols()));
	}
	for (std::size_t j = 0; j < acc.cols(); ++j) {
		if (acc(0, j) != next(0, j)) {
			throw bot::IllegalArgumentException(label + " matrix header differs at column "
				+ std::to_string(j) + ": '" + acc(0, j) + "' (" + pa + ") vs '" + next(0, j)
				+ "' (" + pb + ")");
		}
	}
	std::int64_t idxName = acc.indexOfCol(0, "name");
	if (idxName < 0) {
		throw bot::IllegalArgumentException(label + " matrix has no 'name' column");
	}

	mt::MatrixBuilder b;
	b.name(acc.getName());
	for (std::size_t j = 0; j < acc.cols(); ++j) { b.item(acc(0, j)); }
	b.line(0);

	std::set<std::string> seen;
	auto appendRows = [&](const mt::MatrixObject& m) {
		for (std::int64_t r = 1; r < static_cast<std::int64_t>(m.rows()); ++r) {
			std::string name = m(r, idxName);
			if (!seen.insert(name).second) {
				throw bot::IllegalArgumentException("duplicate " + label + " name '" + name
					+ "' (merging '" + pa + "' and '" + pb + "'; names must be unique"
					+ " across merged scripts)");
			}
			for (std::size_t j = 0; j < m.cols(); ++j) {
				b.item(m(static_cast<std::size_t>(r), j));
			}
			b.line(0);
		}
	};
	appendRows(acc);
	appendRows(next);
	return b.take();
}

/* 收集同一输入内所有 <name> 矩阵并按行叠加（一个 .mt 里可以出现多个 @Alias/@Vars
 * 块，mybot 会全部注册；mtmerge 不能只看第一个而静默丢掉其余块）。 */
std::optional<mt::MatrixObject> foldNamed(const std::vector<mt::MatrixObject>& parsed,
		const std::string& name, const std::string& path) {
	std::optional<mt::MatrixObject> acc;
	for (const auto& m : parsed) {
		if (bot::trim(m.getName()) != name) { continue; }
		if (!acc) {
			acc = m;
		} else {
			acc = mergeScalarRows(*acc, m, name, path, path);
		}
	}
	return acc;
}

/* 设备名集合：列头按 <device>.<param> 切分（与 ActionRunner::parseActionColumn 同规则）。
 * mtmerge 是独立产物，不依赖 robot.cpp 的 ActionRunner，故此处内联同一切分规则。 */
std::pair<std::string, std::string> splitColumn(const std::string& nameHV) {
	std::size_t dot = nameHV.find('.');
	if (dot == std::string::npos || nameHV.find('.', dot + 1) != std::string::npos) {
		throw bot::IllegalArgumentException("Action column '" + nameHV
			+ "' is not valid (expected format: <device>.<param>)");
	}
	return { nameHV.substr(0, dot), nameHV.substr(dot + 1) };
}

/* 校验设备集不相交：相交即抛，指明冲突设备与两侧列头（装配期静态可判，
 * 不静默串行化——把竞态伪装成功能是本项目一贯拒绝的行为）。 */
void checkDisjoint(const mt::MatrixObject& a, const mt::MatrixObject& b,
		const std::string& pa, const std::string& pb) {
	std::map<std::string, std::string> seen;	/* device -> 首个列头原文 */
	for (std::size_t j = 0; j < a.cols(); ++j) {
		auto [dev, param] = splitColumn(a(0, j));
		seen.emplace(dev, a(0, j));
	}
	for (std::size_t j = 0; j < b.cols(); ++j) {
		auto [dev, param] = splitColumn(b(0, j));
		auto it = seen.find(dev);
		if (it != seen.end()) {
			throw bot::IllegalArgumentException("device '" + dev + "' appears in both scripts: '"
				+ it->second + "' (" + pa + ") and '" + b(0, j) + "' (" + pb + ")");
		}
	}
}

/* 一次两两归并：acc（已合并结果）与 next 合成新矩阵。
 * 相位 p>0 表示 next 整体滞后 p 行；数据行 1-based：
 *   acc 行取 r，next 行取 r - p；越界（<1 或 >行数）填 "-"。
 * 总行数 = max(acc 行数, next 行数 + p)（数据行，不含表头）。 */
mt::MatrixObject mergeTwo(const mt::MatrixObject& acc, const mt::MatrixObject& next,
		std::int64_t phase, const std::string& pa, const std::string& pb) {
	checkDisjoint(acc, next, pa, pb);

	mt::MatrixObject out;
	/* 列 = acc 全列（原序） ∪ next 全列（原序）；表头行照抄 */
	mt::MatrixBuilder b;
	b.name(acc.getName());
	for (std::size_t j = 0; j < acc.cols(); ++j) { b.item(acc(0, j)); }
	for (std::size_t j = 0; j < next.cols(); ++j) { b.item(next(0, j)); }
	b.line(0);

	const std::int64_t rowsAcc = static_cast<std::int64_t>(acc.rows()) - 1;	/* 去表头 */
	const std::int64_t rowsNext = static_cast<std::int64_t>(next.rows()) - 1;

	/* 负相位把 next 整体上移：其开头 |phase| 行落到输出窗口之外而被丢弃。
	 * 部分丢弃只告警（产物仍含该输入的其余动作）；**全部**丢弃则报错退出 2
	 * （N-8）：产物不含该输入的任何动作、脚本管道化后警告必被淹没——与
	 * 「设备集相交报错退出 2」同一严格风格，拒绝静默丢动作。 */
	if (phase < 0) {
		const std::int64_t dropped = std::min(-phase, rowsNext);
		if (dropped == rowsNext && rowsNext > 0) {
			throw bot::IllegalArgumentException("phase " + std::to_string(phase)
				+ " drops all " + std::to_string(rowsNext) + " row(s) of '" + pb
				+ "' (the merged output would contain no action from this input);"
				+ " use a smaller negative phase");
		}
		if (dropped > 0) {
			std::cerr << "mtmerge: 警告：相位 " << phase << " 使 '" << pb << "' 的前 "
				<< dropped << " 行被丢弃" << std::endl;
		}
	}

	const std::int64_t total = std::max(rowsAcc, rowsNext + phase);
	for (std::int64_t r = 1; r <= total; ++r) {
		/* acc 侧：r 恒在 [1, rowsAcc] 内当 r<=rowsAcc，否则补 "-" */
		if (r <= rowsAcc) {
			for (std::size_t j = 0; j < acc.cols(); ++j) {
				b.item(acc(static_cast<std::size_t>(r), j));
			}
		} else {
			for (std::size_t j = 0; j < acc.cols(); ++j) { b.item("-"); }
		}
		/* next 侧：行号 r - phase，越界补 "-"（开头未起拍 / 结尾已耗尽） */
		std::int64_t rn = r - phase;
		if (rn >= 1 && rn <= rowsNext) {
			for (std::size_t j = 0; j < next.cols(); ++j) {
				b.item(next(static_cast<std::size_t>(rn), j));
			}
		} else {
			for (std::size_t j = 0; j < next.cols(); ++j) { b.item("-"); }
		}
		b.line(0);
	}
	return b.take();
}

void printHelp() {
	std::cout << "用法：mtmerge [--phase=N] [--out merged.mt] a.mt b.mt [c.mt ...]" << std::endl;
	std::cout << "把设备集不相交的 @Action 行列式脚本合并为一个（N 个输入按序两两归并）。" << std::endl;
	std::cout << "  --phase=N  第 2 个（及后续每个）输入相对已合并结果的行相位：" << std::endl;
	std::cout << "             N>0 滞后 N 行（前导行填 \"-\"，派发时该设备未起拍被跳过）；" << std::endl;
	std::cout << "             N<0 提前（开头 |N| 行被丢弃，部分丢弃在 stderr 告警；" << std::endl;
	std::cout << "             全部丢弃报错退出 2），缺省 0（仅作用于 @Action）" << std::endl;
	std::cout << "  --out=F    写入文件；缺省打印 stdout（可管道给 mybot）" << std::endl;
	std::cout << "退出码：0 成功；1 文件打不开/写不进；2 语义错误（设备集相交、@Alias/@Vars" << std::endl;
	std::cout << "        同名行、负相位把某输入全部丢弃、参数非法/缺少值、输入超限）。" << std::endl;
	std::cout << "冲突语义：同一设备（<device>.<param> 的 device 名）出现在两个输入中即报错退出 2。" << std::endl;
	std::cout << "          @Alias / @Vars 按行叠加，同名行（name 列）即报错退出 2。" << std::endl;
}

} // namespace

/* 审计 M-1 防线：--phase 合法范围（±10 万行） */
static constexpr std::int64_t kMaxPhase = 100000;

int main(int argc, char** argv) {
	std::int64_t phase = 0;
	std::string outPath;
	std::vector<std::string> inputs;
	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a == "--help" || a == "-h") { printHelp(); return 0; }
		auto takeVal = [&](const std::string& key) -> std::string {
			auto eq = a.find('=');
			if (eq != std::string::npos) { return a.substr(eq + 1); }
			if (k + 1 < argc) { return argv[++k]; }
			std::cerr << "mtmerge: --" << key << " 缺少值" << std::endl;
			std::exit(2);
		};
		/* 审计 #10：参数键精确匹配——旧 rfind 前缀匹配会把 --phaseX=5
		 * 静默当作 --phase 接受，与「未知参数一律拒绝」纪律不一致。 */
		if (a == "--phase" || a.rfind("--phase=", 0) == 0) {
			std::string v = takeVal("phase");
			auto [st, num] = bot::parseLongStrict(v);
			if (st != bot::IntParse::OK) {
				std::cerr << "mtmerge: --phase 不是整数 '" << v << "'" << std::endl;
				return 2;
			}
			/* 审计 M-1 防线：相位参与输出行数计算（rowsNext + phase，曾为
			 * 有符号溢出 UB）与逐行循环，无界值会把产物撑到无界内存；语义
			 * 上相位是「滞后/提前行数」，10 万行已是极端值。 */
			if (num > kMaxPhase || num < -kMaxPhase) {
				std::cerr << "mtmerge: --phase 超出范围 ±" << kMaxPhase << " '" << v << "'" << std::endl;
				return 2;
			}
			phase = num;
		} else if (a == "--out" || a.rfind("--out=", 0) == 0) {
			outPath = takeVal("out");
		} else if (a.rfind("--", 0) == 0) {
			std::cerr << "mtmerge: 未知参数 '" << a << "'" << std::endl;
			return 2;
		} else {
			inputs.push_back(a);
		}
	}

	if (inputs.size() < 2) {
		printHelp();
		return inputs.empty() ? 0 : 2;
	}

	try {
		/* 逐个读入 .mt（字节上限与 mybot 动作脚本一致）：
		 *   @Action 按序两两归并（每个输入带同一个 phase）；
		 *   @Alias / @Vars 按行叠加，同名即报错。
		 * 累积器先吃第 1 个输入，之后两两归并。 */
		std::optional<mt::MatrixObject> accAction, accAlias, accVars;
		std::string accActionPath, accAliasPath, accVarsPath;
		for (const auto& path : inputs) {
			std::ifstream f(path, std::ios::binary);
			if (!f) {
				std::cerr << "mtmerge: 无法打开 '" << path << "'" << std::endl;
				return 1;
			}
			/* 审计 M-2 防线：与 actionsource 的 stdin 通道同款「分块读 +
			 * 超限即拒」——先整体读入再判上限对超大文件形同虚设
			 * （限流之前就把全部字节吃进内存）。 */
			std::string data;
			char buf[65536];
			for (;;) {
				f.read(buf, static_cast<std::streamsize>(sizeof buf));
				std::streamsize got = f.gcount();
				if (got <= 0) { break; }
				data.append(buf, static_cast<std::size_t>(got));
				if (data.size() > bot::kMaxScriptBytes) {
					throw bot::ScriptTooLarge(bot::kMaxScriptBytes, "action script");
				}
			}
			bot::MemoryReader in(std::move(data));
			std::vector<mt::MatrixObject> parsed;
			bot::ScriptLoader::readMatrix(in, parsed);
			if (parsed.empty()) {
				throw bot::IllegalArgumentException("'" + path + "': no matrix found");
			}
			const mt::MatrixObject& act = requireAction(parsed, path);
			std::optional<mt::MatrixObject> alias = foldNamed(parsed, "Alias", path);
			std::optional<mt::MatrixObject> vars = foldNamed(parsed, "Vars", path);

			if (!accAction) {
				accAction = act;	/* 拷贝：parsed 出作用域后仍需持有 */
				accActionPath = path;
			} else {
				accAction = mergeTwo(*accAction, act, phase, accActionPath, path);
				accActionPath += "+" + path;
			}
			if (alias) {
				if (!accAlias) {
					accAlias = std::move(*alias);
					accAliasPath = path;
				} else {
					accAlias = mergeScalarRows(*accAlias, *alias, "Alias", accAliasPath, path);
					accAliasPath += "+" + path;
				}
			}
			if (vars) {
				if (!accVars) {
					accVars = std::move(*vars);
					accVarsPath = path;
				} else {
					accVars = mergeScalarRows(*accVars, *vars, "Vars", accVarsPath, path);
					accVarsPath += "+" + path;
				}
			}
		}

		/* 产物顺序：@Alias、@Vars（若存在），最后 @Action。 */
		std::vector<mt::MatrixObject> outMats;
		if (accAlias) { outMats.push_back(std::move(*accAlias)); }
		if (accVars) { outMats.push_back(std::move(*accVars)); }
		outMats.push_back(std::move(*accAction));

		if (outPath.empty()) {
			mt::MatrixSaver::save(outMats, std::cout);
		} else {
			std::ofstream out(outPath, std::ios::binary);
			if (!out) {
				std::cerr << "mtmerge: 无法写入 '" << outPath << "'" << std::endl;
				return 1;
			}
			mt::MatrixSaver::save(outMats, out);
		}
	} catch (const std::exception& e) {
		std::cerr << "mtmerge: " << e.what() << std::endl;
		return 2;
	}
	return 0;
}
