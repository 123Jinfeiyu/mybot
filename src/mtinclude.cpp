/* mtinclude.cpp —— @Include 展开（实现契约见 mtinclude.hpp）
 *
 * 结构：Expander 持一次 expandIncludes 调用的全部跨层状态（visited 表、
 * 行数/字节预算），按层递归——每层（主脚本或某个片段）把**自己的**
 * @Alias/@Vars/@Action 作为累加器初值，再按声明序并入自己 @Include 行
 * 指向的子片段的有效产物；「自己的定义」名字集在并入子片段前快照，
 * 作为主从覆盖的判定依据（同层自己获胜，子片段互相之间报错）。
 *
 * 信任边界（务必与 docs/dsl.md 保持一致）：--tcp 下发的动作脚本可借
 * @Include 触达本地文件。防线按层：
 *   1) 词法：路径必须是相对名——拒绝绝对路径、'..' 分量、空段、NUL，
 *      只放行 .mt 后缀（规则集与 agent::resolveActionScriptPath 同族）；
 *   2) 硬限：全树 @Include 行数 ≤ kMaxIncludes、嵌套深度 ≤
 *      kMaxIncludeDepth、字节总量 ≤ 8 MiB、单文件 ≤ 8 MiB；
 *   3) O_NOFOLLOW：拒绝符号链接（残余同 agent::readFileLimited 的声明：
 *      路径中间的目录软链仍可穿越——能写 base 树的进程本就能直接改脚本，
 *      不构成增量能力）；
 *   4) 内容不回显：读取/解析失败只报路径类信息，被包含文件的内容不会
 *      进入报错文本。
 */

#include "mtinclude.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <optional>
#include <set>
#include <unistd.h>

#include "actionsource.hpp"
#include "mtmerge_core.hpp"
#include "util.hpp"

namespace mtinclude {

namespace {

constexpr std::size_t kMaxIncludeTotalBytes = bot::kMaxScriptBytes;

/* include-once 复用标记：文件名后缀（与 _VIRT 工厂后缀同族的命名约定）。
 * 放在文件而非包含行：片段自己声明复用策略，所有包含点免于各自记忆。 */
bool endsWithOnceSuffix(const std::string& path) {
	const std::string suffix = ".once.mt";
	return path.size() >= suffix.size()
		&& path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* include 路径词法校验（规则集 = agent::resolveActionScriptPath 同族）：
 * 通过则原样返回，失败抛 IllegalArgumentException。 */
std::string validateIncludePath(const std::string& requested) {
	if (requested.empty()) {
		throw bot::IllegalArgumentException("@Include path is empty"
			" (the 'path' column must carry a relative .mt path)");
	}
	if (requested[0] == '/') {
		throw bot::IllegalArgumentException("@Include path must be relative to the"
			" include base directory, absolute path rejected: " + requested);
	}
	if (requested.find('\0') != std::string::npos) {
		throw bot::IllegalArgumentException("@Include path contains NUL");
	}
	std::string last;
	std::size_t b = 0;
	while (b <= requested.size()) {
		std::size_t e = requested.find('/', b);
		std::string part = (e == std::string::npos) ? requested.substr(b) : requested.substr(b, e - b);
		if (part.empty() || part == ".") {
			throw bot::IllegalArgumentException("@Include path contains an empty or '.'"
				" segment: " + requested);
		}
		if (part == "..") {
			throw bot::IllegalArgumentException("@Include path must not traverse outside"
				" the include base directory: " + requested);
		}
		last = part;
		if (e == std::string::npos) { break; }
		b = e + 1;
	}
	if (last.size() < 3 || last.compare(last.size() - 3, 3, ".mt") != 0) {
		throw bot::IllegalArgumentException("@Include only accepts .mt action scripts: "
			+ requested);
	}
	return requested;
}

/* 读取被包含文件：O_NOFOLLOW 拒绝软链，流式累计双上限（单文件/总量），
 * 超限即抛（先拒绝后使用——整读进内存再判上限形同虚设）。 */
std::string readIncludeFile(const std::string& path, std::size_t& totalBytes) {
	int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ELOOP) {
			throw bot::IllegalArgumentException("include rejected: symbolic link: " + path);
		}
		throw bot::IllegalArgumentException("cannot open include file: " + path
			+ " (" + std::strerror(errno) + ")");
	}
	std::string data;
	char buf[8192];
	for (;;) {
		ssize_t n = ::read(fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EINTR) { continue; }
			::close(fd);
			throw bot::IllegalArgumentException("read include file failed: " + path);
		}
		if (n == 0) { break; }
		data.append(buf, static_cast<std::size_t>(n));
		totalBytes += static_cast<std::size_t>(n);
		if (data.size() > bot::kMaxScriptBytes) {
			::close(fd);
			throw bot::ScriptTooLarge(bot::kMaxScriptBytes, "included action script");
		}
		if (totalBytes > kMaxIncludeTotalBytes) {
			::close(fd);
			throw bot::ScriptTooLarge(kMaxIncludeTotalBytes,
				"total size of included action scripts");
		}
	}
	::close(fd);
	return data;
}

/* @Include 矩阵单块行读取：列头白名单（path 必需、phase 可选）+ 每行的
 * 路径/相位解析。phase 沿用矩阵 "-" 继承语义（继承上一行相位，首行 "-" = 0），
 * 数值域与 mtmerge --phase 同界（mtmerge::kMaxPhase）。 */
struct IncludeRow {
	std::string path;	/* 词法校验后的相对路径 */
	std::int64_t phase = 0;
};

class IncludeBlockReader {
	const mt::MatrixObject& block_;
	std::int64_t idxPath_;
	std::int64_t idxPhase_;

public:
	explicit IncludeBlockReader(const mt::MatrixObject& block) : block_(block) {
		for (std::size_t j = 0; j < block.cols(); ++j) {
			std::string head = bot::trim(block(0, j));
			if (head != "path" && head != "phase") {
				throw bot::IllegalArgumentException("@Include matrix column '" + head
					+ "' is not supported (only 'path' and 'phase')");
			}
		}
		idxPath_ = block.requireCol(0, "path", "@Include matrix");
		idxPhase_ = block.indexOfCol(0, "phase");
	}

	std::size_t rowCount() const {
		return block_.rows() > 0 ? static_cast<std::size_t>(block_.rows()) - 1 : 0;
	}

	IncludeRow row(std::int64_t r) const {
		IncludeRow out;
		out.path = validateIncludePath(bot::trim(block_(r, idxPath_)));
		if (idxPhase_ >= 0) {
			std::string ph = bot::trim(block_.inherited(r, idxPhase_));
			if (ph != "-") {	/* "-"：继承上一行相位；首行 "-" = 缺省 0 */
				auto [st, num] = bot::parseLongStrict(ph);
				if (st != bot::IntParse::OK) {
					throw bot::IllegalArgumentException("@Include phase is not an integer: '"
						+ ph + "' (row " + std::to_string(r) + ")");
				}
				if (num > mtmerge::kMaxPhase || num < -mtmerge::kMaxPhase) {
					throw bot::IllegalArgumentException("@Include phase out of range ±"
						+ std::to_string(mtmerge::kMaxPhase) + ": " + ph);
				}
				out.phase = num;
			}
		}
		return out;
	}
};

/* 一个文件（主脚本或片段）展开后的有效产物。path 供上层归并的报错/注记归因。 */
struct ScriptParts {
	std::optional<mt::MatrixObject> alias, vars, action;
	std::string path;
};

/* 一层的归并累加器 + 来源标签（报错文案指明冲突两侧）。 */
struct MergeAcc {
	std::optional<mt::MatrixObject> alias, vars, action;
	std::string aliasPath = "<main script>", varsPath = "<main script>",
		    actionPath = "<main script>";
};

/* 本层「自己的定义」名字集：并入子片段前快照，主从覆盖的判定依据。 */
struct LevelOwned {
	std::set<std::string> alias, vars;
};

LevelOwned ownedNamesOf(const MergeAcc& acc) {
	LevelOwned out;
	auto collect = [](const std::optional<mt::MatrixObject>& m, std::set<std::string>& into) {
		if (!m) { return; }
		std::int64_t idx = m->indexOfCol(0, "name");
		if (idx < 0) { return; }
		for (std::int64_t r = 1; r < static_cast<std::int64_t>(m->rows()); ++r) {
			into.insert((*m)(static_cast<std::size_t>(r), static_cast<std::size_t>(idx)));
		}
	};
	collect(acc.alias, out.alias);
	collect(acc.vars, out.vars);
	return out;
}

/* 主从覆盖的执行点：丢弃 next 中与本层自有定义同名的行（stderr 注记），
 * 其余行原样进入后续归并（子片段互相之间的同名由此仍走 mergeScalarRows
 * 的重复报错）。全未命中时原样拷贝返回。 */
mt::MatrixObject dropOwnedRows(const mt::MatrixObject& m, const std::set<std::string>& owned,
		const std::string& file, const char* kind) {
	std::int64_t idxName = m.indexOfCol(0, "name");
	if (idxName < 0) { return m; }	/* 缺 name 列：原样放行，由 mergeScalarRows 报正错 */
	bool anyDropped = false;
	for (std::int64_t r = 1; r < static_cast<std::int64_t>(m.rows()); ++r) {
		std::string name = m(static_cast<std::size_t>(r), static_cast<std::size_t>(idxName));
		if (owned.count(name) > 0) {
			anyDropped = true;
			std::cerr << "mtinclude: " << kind << " '" << name << "' from '" << file
				<< "' ignored; a definition closer to the main script wins" << std::endl;
		}
	}
	if (!anyDropped) { return m; }
	mt::MatrixBuilder b;
	b.name(m.getName());
	for (std::size_t j = 0; j < m.cols(); ++j) { b.item(m(0, j)); }
	b.line(0);
	for (std::int64_t r = 1; r < static_cast<std::int64_t>(m.rows()); ++r) {
		std::string name = m(static_cast<std::size_t>(r), static_cast<std::size_t>(idxName));
		if (owned.count(name) > 0) { continue; }
		for (std::size_t j = 0; j < m.cols(); ++j) {
			b.item(m(static_cast<std::size_t>(r), j));
		}
		b.line(0);
	}
	return b.take();
}

/* 展开器：一次 expandIncludes 调用的全部跨层状态（visited 表/预算）。 */
class Expander {
public:
	explicit Expander(const std::string& mainBase) : mainBase_(mainBase) {}

	void run(std::vector<mt::MatrixObject>& parsed) {
		MergeAcc acc;
		if (auto* act = mt::findByName(parsed, "Action")) { acc.action = *act; }
		if (auto al = mtmerge::foldNamed(parsed, "Alias", acc.aliasPath)) {
			acc.alias = std::move(*al);
		}
		if (auto va = mtmerge::foldNamed(parsed, "Vars", acc.varsPath)) {
			acc.vars = std::move(*va);
		}
		LevelOwned owned = ownedNamesOf(acc);	/* 快照：仅本层自有定义 */
		runLevel(parsed, mainBase_, acc, owned);

		/* 产物：@Alias、@Vars、@Action（若存在）+ 主脚本的未知矩阵原样透传
		 * （mybot 的消费端按名字取用，顺序无关；规范序只为可读与可测） */
		std::vector<mt::MatrixObject> out;
		if (acc.alias) { out.push_back(std::move(*acc.alias)); }
		if (acc.vars) { out.push_back(std::move(*acc.vars)); }
		if (acc.action) { out.push_back(std::move(*acc.action)); }
		for (auto& m : parsed) {
			std::string n = bot::trim(m.getName());
			if (n == "Alias" || n == "Vars" || n == "Action" || n == "Include") { continue; }
			out.push_back(std::move(m));
		}
		parsed = std::move(out);
	}

private:
	std::string mainBase_;
	std::map<std::string, bool> visited_;	/* resolved 路径 → 是否 include-once 片段 */
	std::size_t totalBytes_ = 0;
	std::size_t rowCount_ = 0;

	/* 处理一层：parsed 的 @Include 行按声明序展开并并入 acc。
	 * owned 是本层「自己的定义」（主从覆盖判据），调用方在并入前快照。 */
	void runLevel(const std::vector<mt::MatrixObject>& parsed, const std::string& baseDir,
			MergeAcc& acc, const LevelOwned& owned) {
		for (const auto& m : parsed) {
			if (bot::trim(m.getName()) != "Include") { continue; }
			if (m.rows() == 0) { continue; }	/* 只有名字没有内容的退化块：无事可做 */
			IncludeBlockReader rows(m);
			for (std::int64_t r = 1; r < static_cast<std::int64_t>(m.rows()); ++r) {
				if (++rowCount_ > kMaxIncludes) {
					throw bot::IllegalArgumentException("@Include declares "
						+ std::to_string(rowCount_ - 1) + " rows before this one,"
						+ " exceeding the limit of " + std::to_string(kMaxIncludes)
						+ " across the whole include tree (each row reads a local file)");
				}
				IncludeRow spec = rows.row(r);	/* 词法校验先于该行 IO */
				ScriptParts parts = expandFile(spec.path, baseDir);
				mergeParts(parts, spec.phase, acc, owned);
			}
		}
	}

	/* 展开一个文件引用（relPath 已词法校验）：visited 表在此实现终止性——
	 * 每个文件至多展开一次；*.once.mt 再触达跳过（注记），普通文件再触达
	 * 报错（抓笔误）。递归深度由行数上限界定（每层嵌套消耗一行）。 */
	ScriptParts expandFile(const std::string& relPath, const std::string& dir) {
		bool once = endsWithOnceSuffix(relPath);
		std::string resolved = dir + "/" + relPath;
		if (visited_.count(resolved) > 0) {
			if (once) {
				std::cerr << "mtinclude: '" << resolved
					<< "' already included, skipped (include-once fragment)" << std::endl;
				return {};
			}
			throw bot::IllegalArgumentException("'" + resolved
				+ "' is included more than once; if it is a shared fragment,"
				" rename it to '*.once.mt' (include-once)");
		}
		visited_[resolved] = once;	/* 入口即标记：自包含/互包含在第二次触达时终止 */

		std::string data = readIncludeFile(resolved, totalBytes_);
		bot::MemoryReader in(std::move(data));
		std::vector<mt::MatrixObject> incParsed;
		mt::readMatrix(in, incParsed);
		if (incParsed.empty()) {
			throw bot::IllegalArgumentException("'" + resolved + "': no matrix found");
		}

		/* 片段自成一层：自己的标量/动作是累加器初值，自己的 @Include 行
		 * （以自身目录为基准）按同一套规则并入。被包含文件可以没有
		 * @Action（纯标量/纯组合片段合法）——整体缺失由派发期报错。 */
		MergeAcc frag;
		frag.aliasPath = resolved;
		frag.varsPath = resolved;
		frag.actionPath = resolved;
		if (auto al = mtmerge::foldNamed(incParsed, "Alias", resolved)) {
			frag.alias = std::move(*al);
		}
		if (auto va = mtmerge::foldNamed(incParsed, "Vars", resolved)) {
			frag.vars = std::move(*va);
		}
		if (auto* act = mt::findByName(incParsed, "Action")) { frag.action = *act; }
		LevelOwned owned = ownedNamesOf(frag);
		runLevel(incParsed, dirOf(resolved), frag, owned);

		return ScriptParts{std::move(frag.alias), std::move(frag.vars),
			std::move(frag.action), resolved};
	}

	/* 子片段的有效产物并入本层累加器：标量走主从覆盖（同层自有定义获胜、
	 * 子片段互相同名报错），动作走设备集不相交归并（相位归属包含行）。 */
	void mergeParts(ScriptParts& parts, std::int64_t phase, MergeAcc& acc,
			const LevelOwned& owned) {
		if (parts.alias) {
			mt::MatrixObject filtered = dropOwnedRows(*parts.alias, owned.alias,
				parts.path, "Alias");
			if (filtered.rows() > 1) {
				if (acc.alias) {
					acc.alias = mtmerge::mergeScalarRows(*acc.alias, filtered, "Alias",
						acc.aliasPath, parts.path);
					acc.aliasPath += "+" + parts.path;
				} else {
					acc.alias = std::move(filtered);
					acc.aliasPath = parts.path;
				}
			}
		}
		if (parts.vars) {
			mt::MatrixObject filtered = dropOwnedRows(*parts.vars, owned.vars,
				parts.path, "Vars");
			if (filtered.rows() > 1) {
				if (acc.vars) {
					acc.vars = mtmerge::mergeScalarRows(*acc.vars, filtered, "Vars",
						acc.varsPath, parts.path);
					acc.varsPath += "+" + parts.path;
				} else {
					acc.vars = std::move(filtered);
					acc.varsPath = parts.path;
				}
			}
		}
		if (parts.action) {
			if (acc.action) {
				acc.action = mtmerge::mergeTwo(*acc.action, *parts.action, phase,
					acc.actionPath, parts.path);
				acc.actionPath += "+" + parts.path;
			} else {
				acc.action = std::move(*parts.action);
				acc.actionPath = parts.path;
			}
		}
	}

	static std::string dirOf(const std::string& resolved) {
		std::size_t slash = resolved.find_last_of('/');
		return slash == std::string::npos ? std::string(".") : resolved.substr(0, slash);
	}
};

} // namespace

void expandIncludes(std::vector<mt::MatrixObject>& parsed, const std::string& baseDir) {
	/* fast path：无 @Include 时逐字不动——既有脚本的解析产物原样返回，
	 * 行为零变化（也保住既有守门对报错文案/时序的全部假设） */
	bool hasInclude = false;
	for (const auto& m : parsed) {
		if (bot::trim(m.getName()) == "Include") { hasInclude = true; break; }
	}
	if (!hasInclude) { return; }

	std::string base = baseDir.empty() ? "." : baseDir;
	while (base.size() > 1 && base.back() == '/') { base.pop_back(); }

	Expander(base).run(parsed);
}

} // namespace mtinclude
