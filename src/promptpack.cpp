#include "promptpack.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include "util.hpp"

namespace bot {
namespace agent {

namespace fs = std::filesystem;

namespace {

/* 读取单个提示词文件。超限是配置错误，要响亮拒绝而不是静默截断（截断过的
 * 提示词悄悄丢约束，比缺文件更难查）——阶段 4.1 起与配置/密钥文件共用
 * util::readWholeFileLimited（同一超限语义，此前 3 份实现里 2 份会静默截断）。 */
std::string readTextFile(const std::string& path, std::string& err) {
	std::string data;
	if (!readWholeFileLimited(path, kMaxPromptFileBytes, data, err, "提示词文件")) {
		return "";
	}
	return data;
}

/* 数字段按数值比较（去前导零后先比长度再比字典序，避免溢出） */
int cmpDigitRun(std::string_view a, std::string_view b) {
	size_t az = a.find_first_not_of('0');
	size_t bz = b.find_first_not_of('0');
	a = (az == std::string_view::npos) ? std::string_view{} : a.substr(az);
	b = (bz == std::string_view::npos) ? std::string_view{} : b.substr(bz);
	if (a.size() != b.size()) { return a.size() < b.size() ? -1 : 1; }
	int c = a.compare(b);
	return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

/* 文件名自然排序：2-x.md 排在 10-x.md 之前（字典序会把 10 排在 2 前面） */
bool naturalNameLess(const std::string& a, const std::string& b) {
	size_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (std::isdigit(static_cast<unsigned char>(a[i]))
			&& std::isdigit(static_cast<unsigned char>(b[j]))) {
			size_t ai = i, bi = j;
			while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) { ++i; }
			while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) { ++j; }
			int c = cmpDigitRun(std::string_view(a).substr(ai, i - ai),
					std::string_view(b).substr(bi, j - bi));
			if (c != 0) { return c < 0; }
		} else {
			if (a[i] != b[j]) {
				return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
			}
			++i;
			++j;
		}
	}
	return a.size() - i < b.size() - j;
}

/* 展开全部输入为有序文件列表：目录收集 *.md 按文件名自然排序，显式文件保持
 * 传入顺序。ec 贯穿全程（目录不可读等折成 err，不抛）。 */
bool expandPromptPaths(const std::vector<std::string>& inputs,
		std::vector<fs::path>& files, std::string& err) {
	for (const auto& in : inputs) {
		fs::path p(in);
		std::error_code ec;
		if (fs::is_directory(p, ec)) {
			std::vector<fs::path> mds;
			for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
				bool regular = it->is_regular_file(ec);
				if (ec) { break; }
				if (regular && it->path().extension() == ".md") { mds.push_back(it->path()); }
			}
			if (ec) {
				err = "无法读取提示词目录 " + in + ": " + ec.message();
				return false;
			}
			if (mds.empty()) {
				err = "目录中没有 .md 提示词文件: " + in;
				return false;
			}
			std::sort(mds.begin(), mds.end(), [](const fs::path& x, const fs::path& y) {
				return naturalNameLess(x.filename().string(), y.filename().string());
			});
			files.insert(files.end(), mds.begin(), mds.end());
		} else if (fs::exists(p, ec)) {
			files.push_back(p);
		} else {
			err = "提示词路径不存在: " + in;
			return false;
		}
	}
	return true;
}

/* real 是否落在 root 之内。两者都应是 canonical 后的绝对路径。
 * 逐段比较而不是字符串前缀：`/srv/prompts-evil` 的字符串前缀正是 `/srv/prompts`，
 * 前缀判定会把它当成子路径放行——逐段比较不会。 */
bool pathUnder(const fs::path& real, const fs::path& root) {
	auto p = real.begin();
	for (auto r = root.begin(); r != root.end(); ++r, ++p) {
		if (p == real.end() || *p != *r) { return false; }
	}
	return true;
}

/* 解析一个「根」：存在则 canonical（展开软链），否则退一步 weakly_canonical
 * （解析到最深的已存在父目录）。两步都拿不到绝对路径即视为不可用——
 * 与 agenttools 的 simSrcParentDir 同一口径，不回落成 "."。 */
bool resolveRoot(const std::string& root, fs::path& out) {
	std::error_code ec;
	fs::path real = fs::canonical(root, ec);
	if (ec) { real = fs::weakly_canonical(root, ec); }
	if (ec || !real.is_absolute()) { return false; }
	out = std::move(real);
	return true;
}

} // namespace

bool loadPromptPack(const std::vector<std::string>& inputs, std::string& out,
		std::string& err) {
	out.clear();
	if (inputs.empty()) { return true; }

	std::vector<fs::path> files;
	if (!expandPromptPaths(inputs, files, err)) { return false; }

	std::vector<std::pair<std::string, std::string>> sections;	/* {文件名, 内容} */
	for (const auto& p : files) {
		std::string content = readTextFile(p.string(), err);
		if (!err.empty()) { return false; }
		content = trim(content);
		if (!content.empty()) {
			/* 提示词内容进系统提示词（指令优先级通道），与 AG-1 同款消毒：
			 * 文件里带一个非法字节就会让首个模型请求被严格网关 400 拒掉。 */
			sections.emplace_back(sanitizeUtf8(p.filename().string()),
					      sanitizeUtf8(content));
		}
	}
	if (sections.empty()) {
		err = "提示词内容为空";
		return false;
	}
	if (sections.size() == 1) {
		out = sections[0].second;	/* 单文件保持原样，不加标题 */
		return true;
	}
	std::string pack;
	for (size_t i = 0; i < sections.size(); ++i) {
		if (i) { pack += "\n\n"; }
		pack += "## " + sections[i].first + "\n\n" + sections[i].second;
	}
	out = std::move(pack);
	return true;
}

std::vector<std::string> promptRootsFromEnv() {
	std::vector<std::string> out;
	auto v = getEnvVar("BOT4CJ_PROMPT_DIR");
	if (!v) { return out; }
	/* 冒号分隔，空项忽略（与 BOT4CJ_CAMERA_SRC_DIR 同款解析口径） */
	std::size_t pos = 0;
	while (pos <= v->size()) {
		const std::size_t colon = v->find(':', pos);
		std::string item = (colon == std::string::npos)
			? v->substr(pos) : v->substr(pos, colon - pos);
		item = trim(item);
		if (!item.empty()) { out.push_back(std::move(item)); }
		if (colon == std::string::npos) { break; }
		pos = colon + 1;
	}
	return out;
}

bool promptPathsWithinRoots(const std::vector<std::string>& inputs,
		const std::vector<std::string>& roots, std::string& err) {
	if (inputs.empty()) { return true; }
	std::vector<fs::path> realRoots;
	for (const auto& r : roots) {
		fs::path p;
		if (resolveRoot(r, p)) { realRoots.push_back(std::move(p)); }
	}
	if (realRoots.empty()) {
		/* 一个根都解析不出来即拒绝（fail-closed）：正常路径下 CWD 总是可解析，
		 * 故这条只在「CWD 被删/无权限」这类异常态命中——那时放行等于没有判据。 */
		err = "无法确定提示词白名单根目录（当前工作目录与 BOT4CJ_PROMPT_DIR 都解析失败）："
			"配置文件来源的 prompt_files 需要至少一个可用的根";
		return false;
	}
	for (const auto& in : inputs) {
		std::error_code ec;
		if (!fs::exists(in, ec)) { continue; }	/* 由加载期报「不存在」 */
		fs::path real = fs::canonical(in, ec);
		if (ec) { continue; }			/* 打不开等同样交加载期 */
		bool ok = false;
		for (const auto& root : realRoots) {
			if (pathUnder(real, root)) { ok = true; break; }
		}
		if (ok) { continue; }
		std::string allowed;
		for (const auto& root : realRoots) {
			if (!allowed.empty()) { allowed += ", "; }
			allowed += root.string();
		}
		err = concatMsg("配置文件来源的提示词路径 '", in, "' 落在允许的根之外（允许: ", allowed,
			"）：提示词内容会进系统提示词、随首个请求发往**同一份配置可控的**"
			  " endpoint，若允许任意路径，一份落在工作目录的配置文件就成了一条"
			  "「读任意文件并外发」的链路（/proc/self/environ 这类伪文件读得到，"
			  "里面就有密钥）。配置文件来源的路径只能指向工作目录之内；"
			  "确实要读该路径请改用命令行 --prompt-file（操作者本人授意），"
			  "或把它加进环境变量 BOT4CJ_PROMPT_DIR（冒号分隔）");
		return false;
	}
	return true;
}

} // namespace agent
} // namespace bot
