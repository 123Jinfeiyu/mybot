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
 *
 * 归并算法本体见 mtmerge_core.hpp/.cpp（F1/N-30：抽出为可白盒单测的模块，
 * 本文件只留 CLI 解析与输入编排）。
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "actionsource.hpp"
#include "matrix.hpp"
#include "mtmerge_core.hpp"
#include "util.hpp"

namespace {

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

/* 审计 M-1 防线：--phase 合法范围（±10 万行）——常量单点在 mtmerge_core.hpp
 * （与 @Include 的 phase 列共用），此处只引用。 */

int main(int argc, char** argv) {
	std::int64_t phase = 0;
	std::string outPath;
	std::vector<std::string> inputs;
	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a == "--help" || a == "-h") { printHelp(); return 0; }
		/* 审计 #10：参数键精确匹配——旧 rfind 前缀匹配会把 --phaseX=5
		 * 静默当作 --phase 接受，与「未知参数一律拒绝」纪律不一致。
		 * 值的切分（--k=v / --k v / 裸 --k）走 cli::parseKeyValue 单点；
		 * mtmerge 的 takeVal 原本没有「下一参数以 -- 开头即不算值」检查，
		 * 传 strictLookahead=false 保持原语义。 */
		if (a == "--phase" || a.rfind("--phase=", 0) == 0) {
			bot::cli::OptionToken tok = bot::cli::parseKeyValue(k, argc, argv, false);
			if (!tok.hasValue) {
				std::cerr << "mtmerge: --phase 缺少值" << std::endl;
				return 2;	/* 与同文件其余错误路径同形（return 而非 std::exit） */
			}
			std::string v = tok.value;
			auto [st, num] = bot::parseLongStrict(v);
			if (st != bot::IntParse::OK) {
				std::cerr << "mtmerge: --phase 不是整数 '" << v << "'" << std::endl;
				return 2;
			}
			/* 审计 M-1 防线：相位参与输出行数计算（rowsNext + phase，曾为
			 * 有符号溢出 UB）与逐行循环，无界值会把产物撑到无界内存；语义
			 * 上相位是「滞后/提前行数」，10 万行已是极端值。 */
			if (num > mtmerge::kMaxPhase || num < -mtmerge::kMaxPhase) {
				std::cerr << "mtmerge: --phase 超出范围 ±" << mtmerge::kMaxPhase << " '" << v << "'" << std::endl;
				return 2;
			}
			phase = num;
		} else if (a == "--out" || a.rfind("--out=", 0) == 0) {
			bot::cli::OptionToken tok = bot::cli::parseKeyValue(k, argc, argv, false);
			if (!tok.hasValue) {
				std::cerr << "mtmerge: --out 缺少值" << std::endl;
				return 2;	/* 同上：不跳过局部对象析构 */
			}
			outPath = tok.value;
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
			/* 审计 M-2 防线：与动作 stdin 通道共用的整段受限读取（分块 +
			 * 超限即抛 ScriptTooLarge，单点在 actionsource.hpp）——先整体
			 * 读入再判上限对超大文件形同虚设（限流之前就把全部字节吃进
			 * 内存）。 */
			std::string data = bot::readAllLimited(f, bot::kMaxScriptBytes, "action script");
			bot::MemoryReader in(std::move(data));
			std::vector<mt::MatrixObject> parsed;
			mt::readMatrix(in, parsed);
			if (parsed.empty()) {
				throw bot::IllegalArgumentException("'" + path + "': no matrix found");
			}
			const mt::MatrixObject& act = mtmerge::requireAction(parsed, path);
			std::optional<mt::MatrixObject> alias = mtmerge::foldNamed(parsed, "Alias", path);
			std::optional<mt::MatrixObject> vars = mtmerge::foldNamed(parsed, "Vars", path);

			if (!accAction) {
				accAction = act;	/* 拷贝：parsed 出作用域后仍需持有 */
				accActionPath = path;
			} else {
				accAction = mtmerge::mergeTwo(*accAction, act, phase, accActionPath, path);
				accActionPath += "+" + path;
			}
			if (alias) {
				if (!accAlias) {
					accAlias = std::move(*alias);
					accAliasPath = path;
				} else {
					accAlias = mtmerge::mergeScalarRows(*accAlias, *alias, "Alias", accAliasPath, path);
					accAliasPath += "+" + path;
				}
			}
			if (vars) {
				if (!accVars) {
					accVars = std::move(*vars);
					accVarsPath = path;
				} else {
					accVars = mtmerge::mergeScalarRows(*accVars, *vars, "Vars", accVarsPath, path);
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
