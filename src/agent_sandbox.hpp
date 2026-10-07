#pragma once
/* agent_sandbox.hpp —— 通用「根目录白名单」路径解析（Phase 1，2026-10-07）
 *
 * 缺口（调研结论，见 .zcode/plans 同日计划文件）：本仓已有两套路径沙箱，但都不是
 * 通用的「canonical 根目录包含性」判据：
 *   - `resolveActionScriptPath`（agentview.cpp）只做**词法**校验（`checkRelPath` +
 *     `.mt` 后缀白名单），不查符号链接是否把访问点引出 scriptsDir——真正的防线是
 *     打开时的 `O_NOFOLLOW`（只挡最后一段，中间目录软链仍可穿过，残余与既有声明
 *     一致，见 util_file.hpp 头注）。
 *   - `capture_image` 的 `camera_src_dir` 白名单其实在 **Lua** 侧
 *     （`modules/Camera.lua` 的 `path_within` + `realpath` 前缀比较），C++ 侧只是
 *     传参，不做判定。
 * 两者形态不同、各自独立实现，不构成「新增一个 host 文件工具」可以直接复用的通用
 * 单点。本文件补上这一个：`resolveUnderRoot`——给定一个 canonical 的 root 与一个
 * 声称的相对路径 requested，判定 requested 是否（词法 + 语义）落在 root 之下，
 * 供未来的 `read_file`/`write_file`/`list_dir` 等通用工具复用，不再各自发明一套。
 *
 * 判据分两层：
 *   1) 词法层：复用 `checkRelPath`（绝对路径 / NUL / 空段 / `.` / `..` 一律拒绝，
 *      发生在任何文件系统访问之前，与 `resolveActionScriptPath` 同一判据来源）。
 *   2) 语义层：root 必须是一个真实存在的目录（否则是**配置**错误，不是**请求**
 *      错误——「配置错误不碰真机」的既有口径，这里是其文件系统版）；requested
 *      拼接到 root 之后，沿路径逐级回退找到**已存在的最长前缀**做 `canonical`
 *      （写文件场景：目标文件可能还不存在，但其所有已存在的祖先目录必须真的在
 *      root 之下——这样即使 root/evil -> /etc 这类符号链接目录下的文件尚未创建，
 *      仍能在父目录这一级被拦下，不必等到 open() 才发现逃逸）。
 *
 * 本函数只管「路径本身是否在 root 之下」，不替调用方做存在性/类型/权限判断，也不
 * 取代 open() 时的 `O_NOFOLLOW`——两层防线职责不同（这里挡的是「目录级」逃逸，
 * `O_NOFOLLOW` 挡的是「目标文件本身是符号链接」），同时具备才完整，见
 * `readFdNoFollowLimited`（util_file.hpp）。 */

#include <filesystem>
#include <string>
#include <system_error>

#include "util_path.hpp"	/* checkRelPath / RelPathCheck */

namespace bot {
namespace agent {

enum class SandboxCheck {
	OK,
	ABSOLUTE,     /* requested 以 '/' 开头 */
	NUL,          /* requested 含 '\0' */
	EMPTY_OR_DOT, /* requested 为空，或含空段 / '.' 段 */
	DOTDOT,       /* requested 含 '..' 段 */
	ROOT_MISSING, /* root 本身不是一个真实存在的目录（配置错误） */
	ESCAPES_ROOT, /* requested 的已存在前缀 canonical 后不在 root 之下（符号链接逃逸等） */
};

struct SandboxResult {
	SandboxCheck check = SandboxCheck::OK;
	/* check==OK 时：root 下的完整路径（lexically_normal，未对目标本身 canonical——
	 * 目标是否存在、是否是符号链接，留给调用方按自己的读/写语义决定，通常是
	 * open() 时再加 O_NOFOLLOW）。check!=OK 时为空。 */
	std::string resolved;
};

inline SandboxResult resolveUnderRoot(const std::string& root, const std::string& requested) {
	SandboxResult r;

	switch (checkRelPath(requested)) {
	case RelPathCheck::ABSOLUTE:     r.check = SandboxCheck::ABSOLUTE;     return r;
	case RelPathCheck::NUL:          r.check = SandboxCheck::NUL;          return r;
	case RelPathCheck::EMPTY_OR_DOT: r.check = SandboxCheck::EMPTY_OR_DOT; return r;
	case RelPathCheck::DOTDOT:       r.check = SandboxCheck::DOTDOT;       return r;
	case RelPathCheck::OK: break;
	}

	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path canonicalRoot = fs::canonical(root, ec);
	if (ec || !fs::is_directory(canonicalRoot, ec) || ec) {
		r.check = SandboxCheck::ROOT_MISSING;
		return r;
	}

	fs::path full = canonicalRoot / requested;
	/* 回退到「已存在的最长前缀」：写文件场景目标还没创建，但它所有已存在的
	 * 祖先目录都必须真的在 root 之下——root 本身总是存在，循环必然终止。 */
	fs::path probe = full;
	while (!probe.empty() && !fs::exists(probe, ec)) {
		fs::path parent = probe.parent_path();
		if (parent == probe) { break; }	/* 到达根 "/"，防死循环 */
		probe = parent;
	}
	fs::path canonicalProbe = fs::canonical(probe, ec);
	if (ec) {
		r.check = SandboxCheck::ESCAPES_ROOT;
		return r;
	}

	const std::string rootStr = canonicalRoot.string();
	const std::string probeStr = canonicalProbe.string();
	const bool within = probeStr == rootStr
		|| (probeStr.size() > rootStr.size()
			&& probeStr.compare(0, rootStr.size(), rootStr) == 0
			&& probeStr[rootStr.size()] == fs::path::preferred_separator);
	if (!within) {
		r.check = SandboxCheck::ESCAPES_ROOT;
		return r;
	}

	r.check = SandboxCheck::OK;
	r.resolved = full.lexically_normal().string();
	return r;
}

} // namespace agent
} // namespace bot
