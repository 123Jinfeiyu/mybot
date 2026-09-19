/* mtinclude.cpp —— @Include 展开（实现契约见 mtinclude.hpp）
 *
 * 语义 = mtmerge（离线合成器）的运行端等价物：主脚本的三块矩阵是累加器，
 * @Include 每行按序并入一个 .mt 文件（@Action 带相位归并、@Alias/@Vars
 * 按行叠加），归并内核（mergeTwo/mergeScalarRows/foldNamed/requireAction）
 * 全部复用 mtmerge_core——两条入口的冲突裁决与报错文案由同一份代码保证
 * 不漂移。展开产物仍是普通矩阵列表：未知矩阵原样透传（与既有行为一致：
 * mybot 只认 Alias/Vars/Action，其余忽略）。
 *
 * 信任边界（务必与 docs/dsl.md 的说明保持一致）：--tcp 下发的动作脚本
 * 此前无法触达本地文件，@Include 是第一个例外。防线按层：
 *   1) 词法：路径必须是 base 树内的相对名——拒绝绝对路径、'..' 分量、
 *      空段、NUL，且只放行 .mt 后缀（规则集与 agent::resolveActionScriptPath
 *      同族，文案按 include 语境另写；'..' 逐段精确比较不误伤子串）；
 *   2) 硬限：@Include 行数 ≤ kMaxIncludes、include 字节总量 ≤ 8 MiB、
 *      单文件 ≤ 8 MiB（kMaxScriptBytes 同一上限）——扇出读盘有界；
 *   3) O_NOFOLLOW：拒绝符号链接（与 agent::readFileLimited 同判据。残余
 *      与该处声明一致：只看最后一段，路径中间的目录软链仍可穿越——能写
 *      base 树的进程本就能直接改脚本，不构成增量能力）；
 *   4) 内容不回显：读取/解析失败只报路径与行号类信息，被包含文件的内容
 *      不会进入报错文本，远端脚本无从借报错探测本地文件内容。
 */

#include "mtinclude.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

#include "actionsource.hpp"
#include "mtmerge_core.hpp"
#include "util.hpp"

namespace mtinclude {

namespace {

/* include 字节总量预算：与单文件上限同值（kMaxScriptBytes）。单文件由
 * 读取循环即时拒绝，总量跨多个 include 累计记账——两道都不可少：只限
 * 单文件时 33 个 7MiB 文件仍是 231MiB。 */
constexpr std::size_t kMaxIncludeTotalBytes = bot::kMaxScriptBytes;

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

/* @Include 矩阵单块行遍历：列头白名单（path 必需、phase 可选）+ 每行的
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

	/* 主脚本自身的三块累加器（标签用于报错文案指明来源侧）：
	 * @Action 可缺省——首个 include 的 @Action 顶替累加器，此时其相位
	 * 无对齐目标而不生效，与 mtmerge「--phase 作用于第 2 个及后续输入」
	 * 的口径一致。@Alias/@Vars 经 foldNamed 收集（同输入多块叠加、
	 * 同名行报错），标签 "<main script>" 指明是本体而非某个文件。 */
	std::optional<mt::MatrixObject> accAction, accAlias, accVars;
	std::string accActionPath = "<main script>", accAliasPath = "<main script>",
		    accVarsPath = "<main script>";
	if (auto* act = mt::findByName(parsed, "Action")) { accAction = *act; }
	auto mainAlias = mtmerge::foldNamed(parsed, "Alias", accAliasPath);
	if (mainAlias) { accAlias = std::move(*mainAlias); }
	auto mainVars = mtmerge::foldNamed(parsed, "Vars", accVarsPath);
	if (mainVars) { accVars = std::move(*mainVars); }

	/* 第一遍（零 IO）：收集全部 @Include 数据行并完成列头/路径/相位的
	 * 全部词法校验——行数预算与配置错误在任何文件读取之前响亮失败，
	 * 不给「读到一半才报错」留半途状态 */
	struct IncludeJob {
		std::string path;
		std::int64_t phase = 0;
	};
	std::vector<IncludeJob> jobs;
	for (const auto& m : parsed) {
		if (bot::trim(m.getName()) != "Include") { continue; }
		if (m.rows() == 0) { continue; }	/* 只有名字没有内容的退化块：无事可做 */
		IncludeBlockReader rows(m);
		for (std::int64_t r = 1; r < static_cast<std::int64_t>(m.rows()); ++r) {
			IncludeRow spec = rows.row(r);
			jobs.push_back(IncludeJob{spec.path, spec.phase});
		}
	}
	if (jobs.size() > kMaxIncludes) {
		throw bot::IllegalArgumentException("@Include declares " + std::to_string(jobs.size())
			+ " files, exceeding the limit of " + std::to_string(kMaxIncludes)
			+ " (include fan-out must stay bounded: each row reads a local file)");
	}

	/* 第二遍：逐行读取 + 归并（块按出现序、行按声明序，jobs 顺序即此序） */
	std::size_t totalBytes = 0;
	for (const IncludeJob& job : jobs) {
		std::string resolved = base + "/" + job.path;
		std::string data = readIncludeFile(resolved, totalBytes);

		bot::MemoryReader in(std::move(data));
		std::vector<mt::MatrixObject> incParsed;
		mt::readMatrix(in, incParsed);
		if (incParsed.empty()) {
			throw bot::IllegalArgumentException("'" + resolved + "': no matrix found");
		}
		/* 深度限 1：被包含文件里再出现 @Include 即拒绝——递归包含需要
		 * 环检测与深度记账，而组合可以通过链式主脚本表达，收益不抵复杂度 */
		for (const auto& im : incParsed) {
			if (bot::trim(im.getName()) == "Include") {
				throw bot::IllegalArgumentException("nested @Include in '" + resolved
					+ "' is not supported (depth limit 1;"
					" chain main scripts instead)");
			}
		}

		auto incAlias = mtmerge::foldNamed(incParsed, "Alias", resolved);
		if (incAlias) {
			if (accAlias) {
				accAlias = mtmerge::mergeScalarRows(*accAlias, *incAlias, "Alias",
					accAliasPath, resolved);
				accAliasPath += "+" + resolved;
			} else {
				accAlias = std::move(*incAlias);
				accAliasPath = resolved;
			}
		}
		auto incVars = mtmerge::foldNamed(incParsed, "Vars", resolved);
		if (incVars) {
			if (accVars) {
				accVars = mtmerge::mergeScalarRows(*accVars, *incVars, "Vars",
					accVarsPath, resolved);
				accVarsPath += "+" + resolved;
			} else {
				accVars = std::move(*incVars);
				accVarsPath = resolved;
			}
		}

		const mt::MatrixObject& act = mtmerge::requireAction(incParsed, resolved);
		if (accAction) {
			accAction = mtmerge::mergeTwo(*accAction, act, job.phase, accActionPath, resolved);
			accActionPath += "+" + resolved;
		} else {
			accAction = act;	/* 拷贝：incParsed 出作用域后仍需持有 */
			accActionPath = resolved;
		}
	}

	/* 产物：@Alias、@Vars、@Action（若存在）+ 主脚本的未知矩阵原样透传
	 * （mybot 的消费端按名字取用，顺序无关；规范序只为可读与可测） */
	std::vector<mt::MatrixObject> out;
	if (accAlias) { out.push_back(std::move(*accAlias)); }
	if (accVars) { out.push_back(std::move(*accVars)); }
	if (accAction) { out.push_back(std::move(*accAction)); }
	for (auto& m : parsed) {
		std::string n = bot::trim(m.getName());
		if (n == "Alias" || n == "Vars" || n == "Action" || n == "Include") { continue; }
		out.push_back(std::move(m));
	}
	parsed = std::move(out);
}

} // namespace mtinclude
