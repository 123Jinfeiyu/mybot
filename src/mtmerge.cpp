/* mtmerge.cpp —— .mt 动作脚本合成器（离线编译目标，独立于 mybot 运行时）
 *
 * 把两个（或多个）设备集不相交的 @Action 行列式脚本合并为一个：
 *   - 设备集（列头 <device>.<param> 切出的 device 名）相交即报错退出 2；
 *   - 行相位：整数 p，B 相对 A 整体滞后 p 行（负值提前），每次归并一个偏移；
 *   - 较短一方行数耗尽后补字面 "-"（继承链自然延伸，保持最后姿势）；
 *   - 列 = A 全列（原序） ∪ B 全列（原序）；产物为规范 .mt 文本，
 *     可直接喂给 mybot，也可继续喂回 mtmerge 做下一轮两两归并。
 *
 * 用法：./build/mtmerge [--phase=N] [--out merged.mt] a.mt b.mt [c.mt ...]
 * N 个输入按序两两归并：先合 a+b，结果再合 c，依此类推。
 */

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

/* 找到第一个数据矩阵（@Action）；找不到即报错。
 * mtmerge 只关心矩阵本身：mybot 侧的 @Alias 已在装配期展开成列头替换，
 * 到这里时列头就是最终形态，无需再解析别名。 */
const mt::MatrixObject& actionOf(const mt::MatrixObject& m, const std::string& path) {
	if (m.hasName()) { return m; }
	throw bot::IllegalArgumentException("'" + path + "': no named @Action matrix found");
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
	std::cout << "             N>0 滞后 N 行，N<0 提前，缺省 0" << std::endl;
	std::cout << "  --out=F    写入文件；缺省打印 stdout（可管道给 mybot）" << std::endl;
	std::cout << "冲突语义：同一设备（<device>.<param> 的 device 名）出现在两个输入中即报错退出 2。" << std::endl;
}

} // namespace

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
		if (a.rfind("--phase", 0) == 0) {
			std::string v = takeVal("phase");
			auto [st, num] = bot::parseLongStrict(v);
			if (st != bot::IntParse::OK) {
				std::cerr << "mtmerge: --phase 不是整数 '" << v << "'" << std::endl;
				return 2;
			}
			phase = num;
		} else if (a.rfind("--out", 0) == 0) {
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
		/* 逐个读入 .mt（字节上限与 mybot 动作脚本一致），按序两两归并：
		 * 累积器先吃第 1 个输入，之后每个输入带同一个 phase 参与归并。 */
		std::optional<mt::MatrixObject> acc;
		std::string accName;
		for (const auto& path : inputs) {
			std::ifstream f(path, std::ios::binary);
			if (!f) {
				std::cerr << "mtmerge: 无法打开 '" << path << "'" << std::endl;
				return 1;
			}
			std::string data((std::istreambuf_iterator<char>(f)),
					std::istreambuf_iterator<char>());
			if (data.size() > bot::kMaxScriptBytes) {
				throw bot::ScriptTooLarge(bot::kMaxScriptBytes, "action script");
			}
			bot::MemoryReader in(std::move(data));
			std::vector<mt::MatrixObject> parsed;
			bot::ScriptLoader::readMatrix(in, parsed);
			if (parsed.empty()) {
				throw bot::IllegalArgumentException("'" + path + "': no matrix found");
			}
			const mt::MatrixObject& act = actionOf(parsed.front(), path);
			if (!acc) {
				acc = act;	/* 拷贝：parsed 出作用域后仍需持有 */
				accName = path;
				continue;
			}
			acc = mergeTwo(*acc, act, phase, accName, path);
			accName += "+" + path;
		}

		if (outPath.empty()) {
			mt::MatrixSaver::save({ *acc }, std::cout);
		} else {
			std::ofstream out(outPath, std::ios::binary);
			if (!out) {
				std::cerr << "mtmerge: 无法写入 '" << outPath << "'" << std::endl;
				return 1;
			}
			mt::MatrixSaver::save({ *acc }, out);
		}
	} catch (const std::exception& e) {
		std::cerr << "mtmerge: " << e.what() << std::endl;
		return 2;
	}
	return 0;
}
