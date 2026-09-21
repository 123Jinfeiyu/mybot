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

} // namespace agent
} // namespace bot
